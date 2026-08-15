import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path


class VirtioNetNotifyE2ETest(unittest.TestCase):
    """Packet-socket RX push test.

    Spawns net_smoke, which opens an AF_PACKET/NET_SOCKET_RAW socket on
    net-stack, transmits an ARP request for the SLIRP gateway, and waits for the
    reply to be *pushed* back over the socket's RX ring (not polled). ARP has no
    stream or datagram socket to sit on, which is why this uses a packet socket
    rather than the driver directly.

    It exercises the whole receive path at once: the device interrupt, the
    driver's per-consumer RX queues, net-stack's frame fan-out to packet
    sockets, and the socket ring doorbell. Requires the default virtio-net NIC
    (do not run with WASMOS_QEMU_NIC_MODEL=none).
    """

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        kernel_src = default_kernel_path()
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src) and os.path.isdir(cfg.esp_dir):
            try:
                shutil.copyfile(kernel_src, kernel_dst)
            except Exception:
                pass
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
            cls.session = None

    def test_rx_frame_notify(self) -> None:
        assert self.session is not None
        self.session.send("spawn /apps/net_smoke")
        self.assertTrue(
            self.session.expect(b"[net-smoke] packet socket open", timeout_s=30),
            "net_smoke could not open a packet socket on net-stack",
        )
        self.assertTrue(
            self.session.expect(b"[net-smoke] arp sent", timeout_s=30),
            "net_smoke did not transmit the ARP request through the socket",
        )
        # The reply must be PUSHED to the socket, never polled for. This is
        # interrupt #2 (the driver's boot ARP was #1), so it proves the device
        # re-delivers: an MSI-X vector when virtio-net bound one, otherwise the
        # shared PCI INTx line. ARP=0x0806.
        self.assertTrue(
            self.session.expect(b"[net-smoke] notify rx=", timeout_s=30),
            "no frame pushed to the packet socket (IRQ re-delivery, the driver's "
            "per-consumer queues, or net-stack's fan-out is broken)",
        )
        self.assertIn(
            b"ethertype=0x0806",
            self.session.buf,
            "delivered frame was not the expected ARP reply",
        )


if __name__ == "__main__":
    unittest.main()
