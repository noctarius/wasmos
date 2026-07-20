import os
import shutil
import socket
import threading
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class NetStackUdpEchoE2ETest(unittest.TestCase):
    """Exercise UDP socket rings through net-stack and the QEMU SLIRP gateway."""

    session: QemuSession | None = None
    echo_socket: socket.socket | None = None
    echo_thread: threading.Thread | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.echo_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cls.echo_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.echo_socket.bind(("0.0.0.0", 5555))
        cls.echo_socket.settimeout(60)

        def echo() -> None:
            assert cls.echo_socket is not None
            try:
                data, peer = cls.echo_socket.recvfrom(2048)
                cls.echo_socket.sendto(data, peer)
            except OSError:
                pass

        cls.echo_thread = threading.Thread(target=echo, daemon=True)
        cls.echo_thread.start()
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
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
        if cls.echo_socket:
            cls.echo_socket.close()
        if cls.echo_thread:
            cls.echo_thread.join(timeout=1)

    def test_udp_echo_via_socket_rings(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=90)
        )
        self.session.send("spawn /apps/net_udp_echo")
        self.assertTrue(
            self.session.expect(b"[net-udp-echo] echo ok", timeout_s=60),
            "UDP echo did not traverse the socket TX/RX rings through SLIRP",
        )


if __name__ == "__main__":
    unittest.main()
