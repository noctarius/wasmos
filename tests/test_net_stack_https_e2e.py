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


def _make_self_signed(dirpath: str) -> tuple[str, str]:
    """Generate a throwaway self-signed RSA cert/key with openssl.

    Returns (cert_path, key_path). Raises if openssl is unavailable or fails so
    the caller can skip the test cleanly.
    """
    cert = os.path.join(dirpath, "c.pem")
    key = os.path.join(dirpath, "k.pem")
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key, "-out", cert,
            "-days", "1", "-nodes", "-subj", "/CN=localhost",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return cert, key


class NetStackHttpsE2ETest(unittest.TestCase):
    """Fetch a body over TLS through curl https:// (no-verify, milestone B).

    A host TLS server (reached from the guest via the SLIRP gateway 10.0.2.2,
    the same local, no-internet path the plain curl/echo tests use) presents a
    runtime-generated self-signed cert and returns a fixed body. The guest curl
    performs a TLS 1.2 ECDHE-RSA handshake (net-stack drives mbedTLS via
    altcp_tls), does NOT verify the certificate, strips the response headers, and
    prints the body to stdout.
    """

    session: QemuSession | None = None
    server: socket.socket | None = None
    thread: threading.Thread | None = None
    tmpdir: tempfile.TemporaryDirectory | None = None

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("openssl not available to generate a TLS cert")
        cls.tmpdir = tempfile.TemporaryDirectory()
        try:
            cert, key = _make_self_signed(cls.tmpdir.name)
        except (OSError, subprocess.CalledProcessError) as exc:
            cls.tmpdir.cleanup()
            cls.tmpdir = None
            raise unittest.SkipTest("could not generate a self-signed cert: %r" % exc)

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
        target = "https://10.0.2.2:%d/hello" % _PORT
        mark = session.mark()
        session.send("spawn /system/utils/curl " + target)
        self.assertTrue(
            session.expect_from(mark, _BODY, timeout_s=60),
            "curl did not print the HTTPS body to stdout",
        )


if __name__ == "__main__":
    unittest.main()
