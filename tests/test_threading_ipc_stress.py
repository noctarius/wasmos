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


class ThreadingIpcStressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            try:
                cls.session.send("halt")
            except Exception:
                pass
            cls.session.close()

    def test_threading_ipc_stress_marker(self):
        ok = self.session.expect(b"[test] threading ipc stress ok", timeout_s=20)
        if not ok:
            self.fail(f"Threading IPC stress marker not found.\n--- tail ---\n{self.session.tail()}\n")


if __name__ == "__main__":
    unittest.main()
