import os
import re
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config, requires_arch

# kernel/include/process.h. Every per-process kernel table is sized by it, so
# running more apps than this in one session is what proves a table frees its
# entry at reap instead of holding it for the life of the kernel.
PROCESS_MAX_COUNT = 64
RUNS = PROCESS_MAX_COUNT + 8

DATE_RESULT = re.compile(
    rb"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\r?\n|date: [^\r\n]*\r?\n"
)
TIMESTAMP = re.compile(rb"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}")


class AppChurnTests(unittest.TestCase):
    """More short-lived apps than there are process slots, in one session."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=False)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            try:
                cls.session.send("halt")
            except Exception:
                pass
            cls.session.close()

    def _date(self, run: int) -> bytes:
        """One `date`, returning its result line. Waits for the result rather
        than the prompt first: the prompt preceding the echo is on the same
        line and would match before the command has produced anything."""
        mark = self.session.mark()
        self.session.send("date")
        if not self.session.expect_from(mark, DATE_RESULT, timeout_s=30):
            self.fail(
                f"run {run}: `date` produced no result.\n--- tail ---\n{self.session.tail()}\n"
            )
        match = DATE_RESULT.search(bytes(self.session.buf), mark)
        if not self.session.expect_from(match.end(), b"wamos> ", timeout_s=30):
            self.fail(
                f"run {run}: no prompt after `date`.\n--- tail ---\n{self.session.tail()}\n"
            )
        return match.group(0)

    @requires_arch(
        "x86_64",
        reason='no RTC on aarch64 yet: the CMOS rtc is x86_64 hardware and PL031 is in docs/TASKS.md "arm64 Port"',
    )
    def test_more_apps_than_process_slots_all_reach_their_services(self):
        """Regression: 2026-11-15-wasm3-ipc-slot-leak -- under wasm3 the per-pid
        "last received message" slot a process takes on its first IPC receive was
        never returned at reap, and pids are never reused, so the 64-entry table
        filled after a few dozen app runs. From then on every new app's receive
        failed with IPC_ERR_NOENT before reaching the endpoint table: `date`
        printed "rtc service not found" and `mount` "no filesystem service" while
        both services were alive. Cost: every CLI session of more than ~30
        commands lost the ability to run apps. It was misdiagnosed as endpoint
        table corruption because the error named a missing endpoint."""
        for run in range(1, RUNS + 1):
            line = self._date(run)
            self.assertRegex(
                line,
                TIMESTAMP,
                f"run {run} of {RUNS}: `date` could not reach the RTC service: "
                f"{line.decode('utf-8', 'replace').strip()!r}",
            )


if __name__ == "__main__":
    unittest.main()
