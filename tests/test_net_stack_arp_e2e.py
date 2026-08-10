import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path


class NetStackArpE2ETest(unittest.TestCase):
    """Verify lwIP reaches the SLIRP gateway through the virtio-net service."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        kernel_src = default_kernel_path()
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
        self.session.send("spawn /apps/net_udp_echo")
        self.assertTrue(
            self.session.expect(b"[net-udp-echo] found net.stack", timeout_s=30),
            "boot-spawned net-stack was not registered before apps started",
        )


if __name__ == "__main__":
    unittest.main()
