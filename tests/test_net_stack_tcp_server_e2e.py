import os
import shutil
import socket
import unittest

from scripts.qemu_test_framework import QemuSession, default_config

# Host port forwarded by QEMU SLIRP into the guest listener (10.0.2.15:5571).
HOST_PORT = 5570
GUEST_PORT = 5571
PAYLOAD = b"wasmos-tcp-srv"


class NetStackTcpServerE2ETest(unittest.TestCase):
    """Exercise a guest TCP listener/accept: the host connects in via SLIRP
    hostfwd, and net-stack pairs the connection with a posted accept slot."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        # Forward host 127.0.0.1:HOST_PORT -> guest 10.0.2.15:GUEST_PORT so the
        # host can initiate the connection into the guest listener.
        cfg.netdev = (
            f"user,id=net0,hostfwd=tcp:127.0.0.1:{HOST_PORT}-10.0.2.15:{GUEST_PORT}"
        )
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
        if cls.session:
            cls.session.force_stop()
            cls.session.close()

    def test_tcp_server_accepts_and_echoes(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=90)
        )
        self.session.send("spawn /apps/net_tcp_server")
        self.assertTrue(
            self.session.expect(b"[net-tcp-srv] listening", timeout_s=60),
            "server did not reach the listening state",
        )
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
            client.settimeout(60)
            client.connect(("127.0.0.1", HOST_PORT))
            self.assertTrue(
                self.session.expect(b"[net-tcp-srv] accepted", timeout_s=60),
                "net-stack did not pair the connection with the accept slot",
            )
            client.sendall(PAYLOAD)
            echoed = b""
            while len(echoed) < len(PAYLOAD):
                chunk = client.recv(len(PAYLOAD) - len(echoed))
                if not chunk:
                    break
                echoed += chunk
            self.assertEqual(echoed, PAYLOAD, "guest did not echo the payload back")
        self.assertTrue(
            self.session.expect(b"[net-tcp-srv] echoed", timeout_s=60),
            "server did not report echoing the segment",
        )


if __name__ == "__main__":
    unittest.main()
