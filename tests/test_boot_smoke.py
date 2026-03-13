import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class BootSmokeTest(unittest.TestCase):
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
        cls.session = QemuSession(cfg, timeout_s=60)
        cls.session.start()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_boot_reaches_cli(self) -> None:
        assert self.session is not None
        ok = self.session.expect(b"[kernel] kmain", timeout_s=30)
        self.assertTrue(ok, "kernel entry marker not observed")
        ok = self.session.expect(b"[irq] pic remapped", timeout_s=10)
        self.assertTrue(ok, "PIC remap marker not observed")
        ok = self.session.expect(b"wamos> ", timeout_s=30)
        self.assertTrue(ok, "CLI prompt not reached")
