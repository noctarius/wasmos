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

    def test_boot_reaches_init_spawn(self) -> None:
        assert self.session is not None
        ok = self.session.expect(b"[init] spawn sysinit", timeout_s=30)
        self.assertTrue(ok, "did not reach sysinit spawn marker")
        ok = self.session.expect(b"[pm] recv type=", timeout_s=10)
        self.assertTrue(ok, "did not receive a proc message in PM")
        ok = self.session.expect(b"[init] sysinit spawn ok", timeout_s=10)
        self.assertTrue(ok, "sysinit spawn reply not received")
        ok = self.session.expect(b"[pm] spawn index=0x0000000000000001", timeout_s=10)
        self.assertTrue(ok, "second spawn index not observed")
        ok = self.session.expect(b"[pm] spawn index=0x0000000000000002", timeout_s=10)
        self.assertTrue(ok, "third spawn index not observed")
        ok = self.session.expect(b"[pm] entry start hw-discovery", timeout_s=10)
        self.assertTrue(ok, "hw-discovery entry not reached")
        ok = self.session.expect(b"[pm] app start ata", timeout_s=10)
        self.assertTrue(ok, "ata start not reached")
