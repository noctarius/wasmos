import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class VirtioNetE2ETest(unittest.TestCase):
    """End-to-end virtio-net data-path test.

    The virtio-net driver, once its RX/TX virtqueues are set up over
    region_alloc'd rings, broadcasts an ARP request for the SLIRP gateway
    (10.0.2.2) and polls the RX ring for the reply. QEMU's user-mode network
    (default `-netdev user`) answers, exercising the full path: region_alloc'd
    rings, vring publish/kick, the device doorbell, TX DMA read, RX DMA write,
    and used-ring completion. Requires the default virtio-net NIC (do not run
    with WASMOS_QEMU_NIC_MODEL=none).
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

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_arp_roundtrip(self) -> None:
        assert self.session is not None
        # TX: the driver queued the ARP request into its region_alloc'd TX ring.
        self.assertTrue(
            self.session.expect(b"[virtio-net] selftest arp request sent", timeout_s=90),
            "virtio-net did not send the ARP request",
        )
        # TX completion: the device consumed the frame (TX used-ring completion).
        self.assertTrue(
            self.session.expect(b"[virtio-net] selftest tx complete", timeout_s=30),
            "device did not complete the transmit",
        )
        # RX: SLIRP's ARP reply arrived on the RX ring.
        self.assertTrue(
            self.session.expect(b"[virtio-net] selftest rx=", timeout_s=30),
            "no frame received on the RX ring",
        )
        # The received frame must be the ARP reply (ethertype 0x0806).
        self.assertIn(
            b"ethertype=0x0806", self.session.buf,
            "received frame was not the expected ARP reply",
        )


if __name__ == "__main__":
    unittest.main()
