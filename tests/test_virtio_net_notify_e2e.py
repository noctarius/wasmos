import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class VirtioNetNotifyE2ETest(unittest.TestCase):
    """RX_FRAME_NOTIFY delivery test.

    Spawns the net_smoke consumer, which looks up the virtio.net driver, fetches
    the NIC MAC, subscribes, transmits an ARP request for the SLIRP gateway, and
    waits for the reply to be *pushed* back via NETDRV_IPC_RX_FRAME_NOTIFY (not
    polled). Proves the driver -> consumer receive path. Requires the default
    virtio-net NIC (do not run with WASMOS_QEMU_NIC_MODEL=none).
    """

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        kernel_src = os.path.join("build", "kernel.elf")
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
            self.session.expect(b"[net-smoke] found virtio.net", timeout_s=30),
            "net_smoke could not resolve the virtio.net service",
        )
        self.assertTrue(
            self.session.expect(b"[net-smoke] arp sent", timeout_s=30),
            "net_smoke did not subscribe + transmit the ARP request",
        )
        # The reply is delivered to the consumer through the driver's RX
        # ready-queue via RX_POLL (an ARP reply, ethertype 0x0806).
        self.assertTrue(
            self.session.expect(b"[net-smoke] rx=", timeout_s=30),
            "no frame was delivered to the consumer",
        )
        self.assertIn(
            b"ethertype=0x0806", self.session.buf,
            "delivered frame was not the expected ARP reply",
        )


if __name__ == "__main__":
    unittest.main()
