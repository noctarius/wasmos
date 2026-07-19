import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class NetStackArpE2ETest(unittest.TestCase):
    """Verify lwIP reaches the SLIRP gateway through the virtio-net service."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        kernel_src = os.path.join("build", "kernel.elf")
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src) and os.path.isdir(cfg.esp_dir):
            shutil.copyfile(kernel_src, kernel_dst)
        cls.session = QemuSession(cfg, timeout_s=150)
        cls.session.start()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_lwip_arp_roundtrip(self) -> None:
        assert self.session is not None
        self.assertTrue(self.session.expect(b"wamos> ", timeout_s=120))
        self.session.send("spawn /init/system/services/net_stack")
        self.assertTrue(
            self.session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=90),
            "net-stack did not bind the virtio.net interface",
        )
        self.assertTrue(
            self.session.expect(b"[net-stack] arp rx", timeout_s=30),
            "lwIP ARP request did not receive the SLIRP gateway reply",
        )


if __name__ == "__main__":
    unittest.main()
