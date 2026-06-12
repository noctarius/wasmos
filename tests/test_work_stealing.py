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

from qemu_test_framework import QemuSession, default_config


class WorkStealingTest(unittest.TestCase):
    """Verify that APs steal and execute threads from CPU 0's ready queue.

    The test spawns sched_info (which prints per-CPU steal_count) and
    checks that at least one non-BSP CPU has steal_count > 0.  This is a
    cumulative counter so it is not sensitive to snapshot timing.

    The test only runs when more than one CPU is present (WASMOS_SMP=1 with
    QEMU -smp N, N > 1).  It is skipped on single-CPU builds.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            try:
                cls.session.send("halt")
            except Exception:
                pass
            cls.session.close()

    def _run_sched_info(self):
        """Spawn sched_info and return its output lines."""
        self.session.send("spawn /system/utils/sched_info.wap")
        # Wait for the header line that sched_info always prints
        if not self.session.expect(b"cpu  ready", timeout_s=15):
            self.fail("sched_info did not produce output")
        # Give it a moment to finish, then collect the buffered output
        import time
        time.sleep(0.5)
        lines = self.session.buf.decode("utf-8", errors="replace").splitlines()
        return lines

    def test_work_stealing_on_aps(self):
        lines = self._run_sched_info()

        # Parse rows: " cpu  ready  running(pid)  steals"
        # Each data row looks like "   1      0            0       0"
        cpu_rows = {}
        for line in lines:
            m = re.match(r"(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", line.strip())
            if m:
                cpu_id    = int(m.group(1))
                steal_cnt = int(m.group(4))
                cpu_rows[cpu_id] = steal_cnt

        if len(cpu_rows) <= 1:
            self.skipTest("Only one CPU present — work-stealing test requires SMP")

        ap_steals = {cpu: cnt for cpu, cnt in cpu_rows.items() if cpu > 0}
        any_stolen = any(cnt > 0 for cnt in ap_steals.values())

        if not any_stolen:
            self.fail(
                f"No AP has performed any work steals.\n"
                f"Per-CPU steal counts: {ap_steals}\n"
                f"--- sched_info output ---\n"
                + "\n".join(l for l in lines if re.search(r"\d", l))
            )


if __name__ == "__main__":
    unittest.main()
