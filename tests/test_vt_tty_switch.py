import os
import sys
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class VtTtySwitchTests(unittest.TestCase):
    """Regression guard for the tty-switch wedge.

    Switching the visible slot to a text VT replays that slot's whole cell grid
    to the framebuffer. The original per-cell IPC replay stormed the framebuffer
    driver's queue and, under SMP, starved the driver — an ~80s stall that looked
    like a hang (which is why test_vt_cli_lockup deliberately avoids switching).
    Phase 5 replaces the per-cell replay with a single shared-buffer grid blit, so
    a switch completes in well under a second even with the compositor holding
    vt-0. This test switches to vt-1 over serial and asserts the CLI keeps
    servicing input promptly afterward.
    """

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

    def test_switch_does_not_wedge(self):
        # A wedge would block the VT (and the switch reply) for ~80s; keep the
        # budget far below that but above a healthy switch.
        t0 = time.time()
        mark = self.session.mark()
        self.session.send("tty 1")
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=15):
            self.fail(
                f"No prompt within 15s after 'tty 1' (switch wedged?).\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )
        self.assertNotIn(
            b"switch failed",
            self.session.buf[mark:],
            msg=f"tty switch reported failure.\n--- tail ---\n{self.session.tail()}\n",
        )
        dt = time.time() - t0
        self.assertLess(
            dt, 15.0, msg=f"tty switch took {dt:.1f}s (expected sub-second)"
        )

    def test_cli_alive_after_switch(self):
        # After the switch, the CLI on the serial-bound slot must still process
        # commands (the serial binding does not move with the visible slot).
        mark = self.session.mark()
        self.session.send("echo tty_switch_ok")
        if not self.session.expect_from(mark, b"tty_switch_ok", timeout_s=12):
            self.fail(
                f"CLI did not echo after tty switch.\n--- tail ---\n{self.session.tail()}\n"
            )


if __name__ == "__main__":
    unittest.main()
