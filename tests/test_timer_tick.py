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


class TimerTickTests(unittest.TestCase):
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

    def test_timer_ticks_do_not_block_boot(self):
        # Boot-to-prompt is ~20s; a 20s cap is right at the edge and flakes on a
        # loaded CI runner.  This test only asserts the prompt is EVER reached
        # (i.e. the PIT doesn't wedge boot), so use a generous margin.
        ok = self.session.expect(b"wamos> ", timeout_s=40)
        if not ok:
            self.fail(f"CLI prompt not found while PIT was running.\n--- tail ---\n{self.session.tail()}\n")


if __name__ == "__main__":
    unittest.main()
