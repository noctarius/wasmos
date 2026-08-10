import os
import re
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path


class VirtioNetE2ETest(unittest.TestCase):
    """End-to-end virtio-net data-path test.

    The virtio-net driver, once its RX/TX virtqueues are set up over
    region_alloc'd rings, binds its device interrupt to its endpoint and
    broadcasts an ARP request for the SLIRP gateway (10.0.2.2). QEMU's user-mode network
    (default `-netdev user`) answers, and the reply is delivered via the device
    interrupt — exercising the full path: region_alloc'd rings, vring
    publish/kick, the device doorbell, TX DMA read, RX DMA write, used-ring
    completion, and IRQ-driven RX drain. Requires the default virtio-net NIC
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

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_arp_roundtrip(self) -> None:
        assert self.session is not None
        # The driver bound its device interrupt to its endpoint. Either path
        # counts: MSI-X is the default (and then INTx is deliberately NOT routed,
        # because binding a vector disables the device's INTx), with the shared
        # line as the fallback when the device or pci-bus cannot provide vectors.
        self.assertTrue(
            self.session.expect(
                re.compile(rb"\[virtio-net\] (msix enabled|irq routed)"), timeout_s=90
            ),
            "virtio-net did not bind its device interrupt",
        )
        # TX: the driver queued the ARP request into its region_alloc'd TX ring.
        self.assertTrue(
            self.session.expect(b"[virtio-net] arp request sent", timeout_s=30),
            "virtio-net did not send the ARP request",
        )
        # RX: SLIRP's ARP reply was delivered via the device interrupt.
        self.assertTrue(
            self.session.expect(b"[virtio-net] irq rx=", timeout_s=30),
            "no frame delivered via the device IRQ",
        )
        # The received frame must be the ARP reply (ethertype 0x0806).
        self.assertIn(
            b"ethertype=0x0806",
            self.session.buf,
            "IRQ-delivered frame was not the expected ARP reply",
        )


if __name__ == "__main__":
    unittest.main()
