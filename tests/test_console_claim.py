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


class ConsoleClaimTests(unittest.TestCase):
    """Only the console owner may register the console ring.

    Regression: 2026-11-16-console-ring-takeover -- klog_register_ring checked
    only that the caller owned the transfer buffer and the doorbell endpoint, and
    a second registration replaced the first. Any app could register a ring of
    its own and receive every tty's output and the kernel log, own COM1 through
    serial_owner_write, and leave the vt with nothing to draw: the console went
    dark for every user of the machine.

    console_claim_deny holds only ipc.basic and offers a well-formed ring. When
    the kernel accepts it, the app's own report and the shell's next prompt are
    published into the app's ring instead of the vt's, so they never appear.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_an_app_cannot_take_the_console_ring(self):
        start = self.session.send_command("console_claim_deny", timeout_s=20)
        if not self.session.expect_from(start, b"console-claim-deny: ok", timeout_s=30):
            self.fail(
                "the console-ring registration was not refused (or its report never "
                f"reached the console).\n--- tail ---\n{self.session.tail()}\n"
            )
        if not self.session.expect_from(start, b"wamos> ", timeout_s=20):
            self.fail(
                f"no prompt after the attempt.\n--- tail ---\n{self.session.tail()}\n"
            )

        start = self.session.send_command("echo console_still_ours", timeout_s=20)
        if not self.session.expect_from(start, b"console_still_ours", timeout_s=20):
            self.fail(
                f"the shell's output no longer reaches the console.\n--- tail ---\n"
                f"{self.session.tail()}\n"
            )


if __name__ == "__main__":
    unittest.main()
