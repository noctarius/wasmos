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


class SyslogPortTests(unittest.TestCase):
    """COM2 carries the kernel log, and only the kernel log.

    The shipped kernel.cmdline sets log.serial=com2, and the harness captures
    COM2 into a file per session. The kernel writes that port directly, so it
    holds the boot from its first lines (the early-log replay) and every service
    line after it, independent of the vt. A shell's output belongs to its slot
    and stays on COM1.
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

    def _syslog_contains(self, needle: str, timeout_s: float = 10.0) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if needle in self.session.syslog_tail(1 << 20):
                return True
            time.sleep(0.25)
        return False

    def test_com2_holds_the_kernel_log_and_not_shell_output(self):
        self.assertTrue(
            self._syslog_contains("[kernel] kmain"),
            "COM2 does not start with the boot: the early-log replay is missing.\n"
            f"--- syslog ---\n{self.session.syslog_tail()}\n",
        )
        self.assertTrue(
            self._syslog_contains("[kernel] log.serial=com2: kernel log also on COM2"),
            f"the kernel did not enable COM2.\n--- syslog ---\n{self.session.syslog_tail()}\n",
        )
        self.assertTrue(
            self._syslog_contains("[calculator] ready"),
            "a service's log line is missing from COM2.\n"
            f"--- syslog ---\n{self.session.syslog_tail()}\n",
        )

        start = self.session.send_command("echo shell_only_marker", timeout_s=20)
        if not self.session.expect_from(start, b"shell_only_marker", timeout_s=20):
            self.fail(
                f"the shell did not answer.\n--- tail ---\n{self.session.tail()}\n"
            )
        if not self.session.expect_from(start, b"wamos> ", timeout_s=20):
            self.fail(f"no prompt after echo.\n--- tail ---\n{self.session.tail()}\n")
        self.assertNotIn(
            "shell_only_marker",
            self.session.syslog_tail(1 << 20),
            "a shell's output reached the kernel-log port",
        )


if __name__ == "__main__":
    unittest.main()
