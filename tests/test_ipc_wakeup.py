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


class IpcWakeupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        # Wait for CLI so the full boot output (including the early-boot
        # IPC wakeup marker) is in the accumulated buffer.
        if not cls.session.expect(b"wamos> "):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            try:
                cls.session.send("halt")
            except Exception:
                pass
            cls.session.close()

    def test_ipc_wakeup_marker(self):
        ok = b"[test] ipc wake ok" in self.session.buf
        if not ok:
            self.fail(f"IPC wakeup marker not found.\n--- tail ---\n{self.session.tail()}\n")


if __name__ == "__main__":
    unittest.main()
