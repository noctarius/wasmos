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

MESSAGES = "/var/log/messages"
PROBE_MOUNT = "/logprobe"


class LoggerTests(unittest.TestCase):
    """The logger service appends the kernel log to /var/log/messages.

    It reads the kernel's kmsg ring from the oldest byte, so the file starts with
    the boot that ran before the service existed, and it is woken by the kernel
    on every later append, so a kernel-log line produced long after boot reaches
    the file too -- here, the line fs-tmpfs logs when a tmpfs is mounted.
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

    def _run(self, cmd: str, timeout_s: float = 30) -> bytes:
        start = self.session.send_command(cmd, timeout_s=20)
        if not self.session.expect_from(start, b"wamos> ", timeout_s=timeout_s):
            self.fail(
                f"no prompt after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )
        return bytes(self.session.buf[start:])

    def _messages_contain(self, needle: bytes, timeout_s: float = 20) -> bytes:
        deadline = time.time() + timeout_s
        out = b""
        while time.time() < deadline:
            out = self._run(f"cat {MESSAGES}")
            if needle in out:
                return out
            time.sleep(1)
        self.fail(
            f"{needle!r} never reached {MESSAGES}.\n--- last cat ---\n{out[-3000:]!r}\n"
        )

    def test_1_the_file_starts_with_the_boot(self):
        out = self._messages_contain(b"[kernel] kmain")
        self.assertNotIn(
            b"not retained", out, "a fresh boot overflowed the kernel log ring"
        )

    def test_2_a_later_kernel_log_line_is_appended(self):
        self._run(f"mkdir -p {PROBE_MOUNT}")
        out = self._run(f"mount -t tmpfs {PROBE_MOUNT}")
        self.assertIn(
            b"mount: mounted", out, f"the probe mount failed: {out[-1000:]!r}"
        )
        self._messages_contain(f"[fs-tmpfs] ready mount={PROBE_MOUNT}".encode())


if __name__ == "__main__":
    unittest.main()
