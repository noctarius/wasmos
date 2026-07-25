import os
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest

from scripts.qemu_test_framework import QemuSession, default_config

_PORT = 5581
_BODY = b"wasmos-https-body-7"
_RESPONSE = (
    b"HTTP/1.0 200 OK\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: " + str(len(_BODY)).encode() + b"\r\n"
    b"Connection: close\r\n"
    b"\r\n" + _BODY
)


def _run_openssl(args: list) -> None:
    subprocess.run(
        ["openssl"] + args,
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _make_ca_signed(d: str) -> tuple[str, str, str]:
    """Generate a throwaway CA and a CA-signed server cert (SAN IP:10.0.2.2).

    Milestone C verifies the chain and hostname, so a plain self-signed cert no
    longer works: the guest curl reaches the server by the IP literal 10.0.2.2 and
    checks that name against the certificate SAN, and the chain must validate to
    the CA that the test installs into the ESP trust store.

    Returns (ca_pem, server_cert, server_key). Raises if openssl is unavailable.
    """
    ca_key = os.path.join(d, "ca.key")
    ca_pem = os.path.join(d, "ca.pem")
    srv_key = os.path.join(d, "srv.key")
    srv_csr = os.path.join(d, "srv.csr")
    srv_pem = os.path.join(d, "srv.pem")
    ext = os.path.join(d, "ext.cnf")
    with open(ext, "w") as fh:
        fh.write("subjectAltName=IP:10.0.2.2\n")
    _run_openssl([
        "req", "-x509", "-newkey", "rsa:2048", "-keyout", ca_key, "-out", ca_pem,
        "-days", "1", "-nodes", "-subj", "/CN=WASMOS Test CA",
        "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign",
    ])
    _run_openssl([
        "req", "-newkey", "rsa:2048", "-keyout", srv_key, "-out", srv_csr,
        "-nodes", "-subj", "/CN=10.0.2.2",
    ])
    _run_openssl([
        "x509", "-req", "-in", srv_csr, "-CA", ca_pem, "-CAkey", ca_key,
        "-CAcreateserial", "-out", srv_pem, "-days", "1", "-extfile", ext,
    ])
    return ca_pem, srv_pem, srv_key


class NetStackHttpsE2ETest(unittest.TestCase):
    """Fetch a body over TLS through curl https:// with full verification.

    A host TLS server (reached from the guest via the SLIRP gateway 10.0.2.2)
    presents a runtime-generated cert signed by a throwaway CA, whose SAN is
    IP:10.0.2.2. The CA is installed into the ESP trust store, so the guest curl
    performs a TLS 1.2 ECDHE-RSA handshake (net-stack drives mbedTLS via
    altcp_tls), verifies the chain and the 10.0.2.2 hostname, strips the response
    headers, and prints the body to stdout.
    """

    session: "QemuSession | None" = None
    server: "socket.socket | None" = None
    thread: "threading.Thread | None" = None
    tmpdir: "tempfile.TemporaryDirectory | None" = None

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("openssl not available to generate a TLS cert")
        cls.tmpdir = tempfile.TemporaryDirectory()
        try:
            ca, cert, key = _make_ca_signed(cls.tmpdir.name)
        except (OSError, subprocess.CalledProcessError) as exc:
            cls.tmpdir.cleanup()
            cls.tmpdir = None
            raise unittest.SkipTest("could not generate a CA-signed cert: %r" % exc)

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert, keyfile=key)

        cls.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        cls.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.server.bind(("0.0.0.0", _PORT))
        cls.server.listen(4)
        cls.server.settimeout(120)

        def serve() -> None:
            assert cls.server is not None
            while True:
                try:
                    conn, _ = cls.server.accept()
                except OSError:
                    return
                with conn:
                    conn.settimeout(30)
                    try:
                        tls = ctx.wrap_socket(conn, server_side=True)
                    except OSError:
                        continue
                    with tls:
                        try:
                            tls.recv(2048)  # drain the request line/headers
                            tls.sendall(_RESPONSE)
                        except OSError:
                            pass

        cls.thread = threading.Thread(target=serve, daemon=True)
        cls.thread.start()

        cfg = default_config()
        if not os.path.isdir(cfg.esp_dir):
            raise unittest.SkipTest("ESP dir missing; build the project first")
        # Install the test CA into the ESP trust store so net-stack trusts it.
        ca_dst_dir = os.path.join(cfg.esp_dir, "system", "net", "certificates")
        os.makedirs(ca_dst_dir, exist_ok=True)
        shutil.copyfile(ca, os.path.join(ca_dst_dir, "ca-certs.pem"))

        kernel_src = os.path.join("build", "kernel.elf")
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src) and os.path.isdir(cfg.esp_dir):
            shutil.copyfile(kernel_src, kernel_dst)
        cls.session = QemuSession(cfg, timeout_s=180)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=120):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.server is not None:
            cls.server.close()
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.tmpdir is not None:
            cls.tmpdir.cleanup()
            cls.tmpdir = None

    def test_curl_https_fetches_body(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(
            session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=90),
            "net-stack interface not ready",
        )
        self.assertTrue(
            session.expect(b"[net-stack] tls: CA trust store loaded", timeout_s=60),
            "net-stack CA trust store not loaded",
        )
        target = "https://10.0.2.2:%d/hello" % _PORT
        mark = session.mark()
        session.send("spawn /system/utils/curl " + target)
        self.assertTrue(
            session.expect_from(mark, _BODY, timeout_s=60),
            "curl did not print the HTTPS body to stdout",
        )


if __name__ == "__main__":
    unittest.main()
