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


class VtCliLockupRegressionTests(unittest.TestCase):
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

    def _cmd_expect_prompt(self, cmd: str, needle: bytes, timeout_s: int = 12) -> bytes:
        mark = self.session.mark()
        self.session.send(cmd)
        if not self.session.expect_from(mark, needle, timeout_s=timeout_s):
            self.fail(f"Expected output not found for '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s):
            self.fail(f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")
        return self.session.buf[mark:]

    def test_prompt_recovers_after_repeated_commands(self):
        # Regression target: avoid framebuffer/VT path stalls where one command
        # works but the following prompt/input loop never recovers.
        commands = (
            ("help", b"commands:"),
            ("ps", b"processes:"),
            ("ls", b"apps"),
            ("help", b"commands:"),
            ("ps", b"processes:"),
            ("ls", b"apps"),
            ("help", b"commands:"),
            ("ps", b"processes:"),
        )
        for cmd, needle in commands:
            self._cmd_expect_prompt(cmd, needle)

    def test_input_echo_not_doubled_for_ps(self):
        out = self._cmd_expect_prompt("ps", b"processes:")
        self.assertNotIn(
            b"ppss",
            out,
            msg=f"Detected duplicated command echo for 'ps'.\n--- tail ---\n{self.session.tail()}\n",
        )


if __name__ == "__main__":
    unittest.main()
