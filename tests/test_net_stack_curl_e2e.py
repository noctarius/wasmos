import os
import shutil
import socket
import threading
import unittest

from scripts.qemu_test_framework import QemuSession, default_config

_PORT = 5580
_BODY = b"wasmos-curl-body-42"
_RESPONSE = (
    b"HTTP/1.0 200 OK\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: " + str(len(_BODY)).encode() + b"\r\n"
    b"Connection: close\r\n"
    b"\r\n" + _BODY
)


class NetStackCurlE2ETest(unittest.TestCase):
    """Fetch an HTTP body through curl over the TCP socket rings.

    A host HTTP server (reached from the guest via the SLIRP gateway 10.0.2.2,
    the same local, no-internet path the TCP echo test uses) returns a fixed
    body. curl connects by IPv4 literal (no DNS), strips the response headers,
    and writes the body to stdout and, with -o, to a file in the writable /boot
    FAT volume (verified by reading it back with cat).
    """

    session: QemuSession | None = None
    server: socket.socket | None = None
    thread: threading.Thread | None = None

    @classmethod
    def setUpClass(cls) -> None:
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
                        conn.recv(2048)  # drain the request line/headers
                        conn.sendall(_RESPONSE)
                    except OSError:
                        pass

        cls.thread = threading.Thread(target=serve, daemon=True)
        cls.thread.start()

        cfg = default_config()
        kernel_src = os.path.join("build", "kernel.elf")
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src) and os.path.isdir(cfg.esp_dir):
            shutil.copyfile(kernel_src, kernel_dst)
        cls.session = QemuSession(cfg, timeout_s=150)
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

    def test_curl_fetches_body(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(
            session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=90),
            "net-stack interface not ready",
        )
        target = "10.0.2.2:%d/hello" % _PORT

        # 1) body to stdout
        mark = session.mark()
        session.send("spawn /system/utils/curl " + target)
        self.assertTrue(
            session.expect_from(mark, _BODY, timeout_s=45),
            "curl did not print the HTTP body to stdout",
        )

        # 2) body to a file in the writable /boot volume, then read it back
        mark = session.mark()
        session.send("spawn /system/utils/curl " + target + " -o /boot/curl_dl.txt")
        self.assertTrue(
            session.expect_from(mark, b"[curl] wrote 19 bytes to /boot/curl_dl.txt", timeout_s=45),
            "curl did not report writing the body to the file",
        )
        mark = session.mark()
        session.send("cat /boot/curl_dl.txt")
        self.assertTrue(
            session.expect_from(mark, _BODY, timeout_s=30),
            "the downloaded file did not contain the HTTP body",
        )


if __name__ == "__main__":
    unittest.main()
