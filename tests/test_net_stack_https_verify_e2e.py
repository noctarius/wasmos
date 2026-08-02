import os
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest

from scripts.qemu_test_framework import QemuSession, default_config

# Positive server: cert signed by the bundled test CA, SAN IP:10.0.2.2.
# Negative server: a DIFFERENT self-signed cert NOT signed by the bundled CA.
_PORT_OK = 5582
_PORT_BAD = 5583
_BODY = b"wasmos-verify-body-42"
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


def _gen_pki(d: str) -> dict:
    """Generate a self-signed test CA, a CA-signed server cert (SAN IP:10.0.2.2),
    and an unrelated rogue self-signed cert (also SAN IP:10.0.2.2).

    curl connects to https://10.0.2.2:<port> by IP literal, so the SNI/verify name
    is "10.0.2.2" and the server cert MUST carry subjectAltName=IP:10.0.2.2.
    Returns a dict of file paths. Raises on any openssl failure so the caller can
    skip the test cleanly.
    """
    ca_key = os.path.join(d, "ca.key")
    ca_pem = os.path.join(d, "ca.pem")
    srv_key = os.path.join(d, "srv.key")
    srv_csr = os.path.join(d, "srv.csr")
    srv_pem = os.path.join(d, "srv.pem")
    rogue_key = os.path.join(d, "rogue.key")
    rogue_pem = os.path.join(d, "rogue.pem")
    ext = os.path.join(d, "ext.cnf")
    with open(ext, "w") as fh:
        fh.write("subjectAltName=IP:10.0.2.2\n")

    # Self-signed CA (CN=WASMOS Test CA), explicitly a signing CA.
    _run_openssl(
        [
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-keyout",
            ca_key,
            "-out",
            ca_pem,
            "-days",
            "1",
            "-nodes",
            "-subj",
            "/CN=WASMOS Test CA",
            "-addext",
            "basicConstraints=critical,CA:TRUE",
            "-addext",
            "keyUsage=critical,keyCertSign,cRLSign",
        ]
    )
    # Server cert signed by the CA, SAN IP:10.0.2.2.
    _run_openssl(
        [
            "req",
            "-newkey",
            "rsa:2048",
            "-keyout",
            srv_key,
            "-out",
            srv_csr,
            "-nodes",
            "-subj",
            "/CN=10.0.2.2",
        ]
    )
    _run_openssl(
        [
            "x509",
            "-req",
            "-in",
            srv_csr,
            "-CA",
            ca_pem,
            "-CAkey",
            ca_key,
            "-CAcreateserial",
            "-out",
            srv_pem,
            "-days",
            "1",
            "-extfile",
            ext,
        ]
    )
    # Rogue self-signed cert (same name/SAN, but NOT chained to the CA).
    _run_openssl(
        [
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-keyout",
            rogue_key,
            "-out",
            rogue_pem,
            "-days",
            "1",
            "-nodes",
            "-subj",
            "/CN=10.0.2.2",
            "-addext",
            "subjectAltName=IP:10.0.2.2",
        ]
    )
    return {
        "ca": ca_pem,
        "srv_cert": srv_pem,
        "srv_key": srv_key,
        "rogue_cert": rogue_pem,
        "rogue_key": rogue_key,
    }


def _spawn_tls_server(cert: str, key: str, port: int) -> socket.socket:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    srv.settimeout(120)

    def serve() -> None:
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            with conn:
                conn.settimeout(30)
                try:
                    tls = ctx.wrap_socket(conn, server_side=True)
                except OSError:
                    # A client that aborts on verification failure lands here;
                    # that is the expected negative-path behavior.
                    continue
                with tls:
                    try:
                        tls.recv(2048)
                        tls.sendall(_RESPONSE)
                    except OSError:
                        pass

    threading.Thread(target=serve, daemon=True).start()
    return srv


class NetStackHttpsVerifyE2ETest(unittest.TestCase):
    """Milestone C: curl https:// with real chain + hostname verification.

    A CA-signed server (positive) verifies to the bundled test CA and its SAN
    matches 10.0.2.2, so curl prints the body. A rogue self-signed server
    (negative) does NOT chain to the bundled CA, so net-stack rejects the
    handshake and curl prints a failure and never the body. The test overwrites
    the ESP trust store (build/esp/system/net/certificates/ca-certs.pem) with its
    own CA so the check is hermetic (no internet, no real bundle needed).
    """

    session: "QemuSession | None" = None
    ok_server: "socket.socket | None" = None
    bad_server: "socket.socket | None" = None
    tmpdir: "tempfile.TemporaryDirectory | None" = None

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("openssl not available to generate test certs")
        cls.tmpdir = tempfile.TemporaryDirectory()
        try:
            pki = _gen_pki(cls.tmpdir.name)
        except (OSError, subprocess.CalledProcessError) as exc:
            cls.tmpdir.cleanup()
            cls.tmpdir = None
            raise unittest.SkipTest("could not generate test PKI: %r" % exc)

        cfg = default_config()
        if not os.path.isdir(cfg.esp_dir):
            raise unittest.SkipTest("ESP dir missing; build the project first")

        # Overwrite the ESP trust store with our test CA so net-stack trusts the
        # positive server and rejects the rogue one.
        ca_dst_dir = os.path.join(cfg.esp_dir, "system", "net", "certificates")
        os.makedirs(ca_dst_dir, exist_ok=True)
        shutil.copyfile(pki["ca"], os.path.join(ca_dst_dir, "ca-certs.pem"))

        cls.ok_server = _spawn_tls_server(pki["srv_cert"], pki["srv_key"], _PORT_OK)
        cls.bad_server = _spawn_tls_server(
            pki["rogue_cert"], pki["rogue_key"], _PORT_BAD
        )

        kernel_src = os.path.join("build", "kernel.elf")
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src):
            shutil.copyfile(kernel_src, kernel_dst)
        cls.session = QemuSession(cfg, timeout_s=180)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=120):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")
        # Wait for the interface AND the CA trust store to be ready before driving
        # TLS, so neither assertion races the async CA load.
        if not cls.session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=90):
            cls.session.close()
            raise RuntimeError("net-stack interface not ready")
        if not cls.session.expect(
            b"[net-stack] tls: CA trust store loaded", timeout_s=60
        ):
            cls.session.close()
            raise RuntimeError("net-stack CA trust store not loaded")

    @classmethod
    def tearDownClass(cls) -> None:
        for srv in (cls.ok_server, cls.bad_server):
            if srv is not None:
                srv.close()
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.tmpdir is not None:
            cls.tmpdir.cleanup()
            cls.tmpdir = None

    def test_a_positive_verified_chain_fetches_body(self) -> None:
        assert self.session is not None
        session = self.session
        mark = session.mark()
        session.send("spawn /system/utils/curl https://10.0.2.2:%d/hello" % _PORT_OK)
        self.assertTrue(
            session.expect_from(mark, _BODY, timeout_s=60),
            "curl did not print the HTTPS body for the CA-verified server",
        )

    def test_b_negative_untrusted_cert_is_rejected(self) -> None:
        assert self.session is not None
        session = self.session
        mark = session.mark()
        session.send("spawn /system/utils/curl https://10.0.2.2:%d/hello" % _PORT_BAD)
        # curl must report a failure (the handshake is rejected on verification).
        self.assertTrue(
            session.expect_from(mark, b"[curl] connect failed", timeout_s=60),
            "curl did not report a failure for the untrusted (rogue) server",
        )
        # And the body must NEVER appear for the rejected connection.
        self.assertNotIn(
            _BODY,
            session.buf[mark:],
            "curl printed the body for an untrusted server (verification bypassed!)",
        )


if __name__ == "__main__":
    unittest.main()
