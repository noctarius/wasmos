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


class DmesgTests(unittest.TestCase):
    """`dmesg` prints the kernel log ring, and only kernel log.

    The ring keeps the newest 64 KiB of kernel log from the first line of kmain,
    so a fresh boot is held whole: the oldest line is kmain's, and service lines
    logged long after it are there too. A shell's own output belongs to its slot
    and never enters the ring.
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

    def _run(self, cmd: str) -> bytes:
        start = self.session.send_command(cmd, timeout_s=20)
        if not self.session.expect_from(start, b"wamos> ", timeout_s=30):
            self.fail(
                f"no prompt after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )
        return bytes(self.session.buf[start:])

    def test_dmesg_prints_the_boot_and_not_shell_output(self):
        self._run("echo kmsg_shell_marker")
        out = self._run("dmesg")
        self.assertIn(
            b"[kernel] kmain",
            out,
            f"dmesg does not start with the boot.\n--- output ---\n{out[-4000:]!r}\n",
        )
        self.assertIn(
            b"[fs-wfs] ready",
            out,
            f"a service's log line is missing.\n--- output ---\n{out[-4000:]!r}\n",
        )
        self.assertNotIn(
            b"not retained",
            out,
            "a fresh boot overflowed the 64 KiB kernel log ring",
        )
        self.assertNotIn(
            b"kmsg_shell_marker",
            out,
            "a shell's output reached the kernel log ring",
        )


if __name__ == "__main__":
    unittest.main()
