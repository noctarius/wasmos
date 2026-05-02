import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class HwDiscoveryIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _cmd_expect(self, cmd: str, needle: bytes, timeout_s: int = 10) -> None:
        mark = self.session.mark()
        self.session.send(cmd)
        ok = self.session.expect_from(mark, needle, timeout_s=timeout_s)
        if not ok:
            self.fail(f"Expected output not found for '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")
        ok = self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
        if not ok:
            self.fail(f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")

    def test_hw_discovery_running(self):
        self.assertIn(b"[wasmos-app] entry start hw-discovery export=initialize", self.session.buf)
        self._cmd_expect("ps", b"processes:")


if __name__ == "__main__":
    unittest.main()
