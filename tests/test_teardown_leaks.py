"""Building and tearing down an address space gives back every frame it took.

The boot option debug.leak_test=1 runs each build/teardown cycle a fixed number
of times on the boot CPU, before the other CPUs start, and logs free physical
memory before and after; a cycle that leaks shows as a smaller "after".

Regression: 2026-11-26-cloned-low-slot-tables-leak -- paging_destroy_address_space
and paging_strip_low_slot_in_root freed a private low slot's top table only.
The PD and PT frames (aarch64: level-2 and level-3 tables) that
paging_clone_low_slot_in_root gives every process root stayed allocated, on
every process teardown.

Regression: 2026-11-26-warp-r3-trampoline-leak -- warp_r3_teardown relied on
paging_destroy_address_space to free the two trampoline pages mapped into a
WARP guest's ring-3 root, but that frees page-table frames only: 8 KiB per WARP
guest teardown.
"""

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

RESULT = re.compile(
    rb"\[leak-test\] ([^:\r\n]+): cycles=(\d+) before=(\d+) after=(\d+)"
)
NOT_RUN = re.compile(rb"\[leak-test\] ([^:\r\n]+): [^\r\n]*not run")


class TeardownLeakTest(unittest.TestCase):
    def test_build_and_teardown_cycles_return_every_frame(self):
        cfg = default_config()
        cfg.kernel_options = ("debug.leak_test=1",)
        session = QemuSession(cfg, timeout_s=120, echo=False)
        session.start()
        try:
            session.require_prompt()
            # Kernel log, which may reach only the kernel-log port.
            log = session.buf + session.syslog_tail(1 << 20).encode()
            tail = f"--- tail ---\n{session.tail(4096)}\n"
            self.assertIn(
                b"[leak-test] done", log, f"the leak test did not run.\n{tail}"
            )
            # The console and the kernel-log port can both carry a line.
            results = sorted(set(RESULT.findall(log)))
            skipped = sorted({m.decode() for m in NOT_RUN.findall(log)})
            self.assertTrue(results, f"the leak test reported no cycle.\n{tail}")
            for what, cycles, before, after in results:
                with self.subTest(what=what.decode()):
                    leaked = int(before) - int(after)
                    self.assertEqual(
                        leaked,
                        0,
                        f"{what.decode()}: {leaked} bytes not returned over "
                        f"{int(cycles)} cycles",
                    )
            if skipped:
                print(f"leak test not run here: {', '.join(skipped)}")
        finally:
            session.close()


if __name__ == "__main__":
    unittest.main()
