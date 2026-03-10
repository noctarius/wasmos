import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class BootSmokeTest(unittest.TestCase):
    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
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
