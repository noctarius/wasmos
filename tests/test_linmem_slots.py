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

# The boot peaks at about 28 slots (transient apps included) on both runtimes,
# and the process table (64) leaves about 28 spawns once the prompt is up, so 36
# boots cleanly and runs out long before the process table does. The test reads
# the pool back and fails with the numbers if that stops being true.
SLOT_LIMIT = 36
REFUSED = b"has no slot (none free, or commit failed); refusing the module"
SLOTS_LINE = re.compile(rb"slots:\s+(\d+) used of (\d+) \(pool (\d+)\)")


class LinmemSlotTests(unittest.TestCase):
    """Linear memory never relocates; without a slot the module is refused.

    Regression: 2026-11-16-warp-linmem-relocated -- under WARP a linear-memory
    block that did not move into its dedicated VA slot stayed a contiguous block
    that warp_krealloc RELOCATED on growth. A buffer overlay remaps pages of that
    block's kernel alias and its unmap clears them, so the next grow copied the
    old block through the hole and faulted (CI filesystem warp_smp: #PF in
    memcpy <- warp_krealloc, gfx-smoke). The move was lost when two WARP spawns
    raced for a global reserve hint under SMP, which a test cannot time; the move
    failing because no slot is free is the same path, and the linmem.slots boot
    option makes it reachable.

    linmem_hold grows its linear memory and then holds it. Spawned until the
    pool is full, the next one must be refused with the kernel's reason. Before
    the fix every WARP holder ran, its linear memory relocated. wasm3 reserves
    its slot at load and has no relocating path; it runs the same contract.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cfg.kernel_options = (f"linmem.slots={SLOT_LIMIT}",)
        cls.session = QemuSession(cfg, timeout_s=300, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _slots(self):
        start = self.session.send_command("sysinfo", timeout_s=20)
        if not self.session.expect_from(start, b"processes", timeout_s=20):
            self.fail(f"sysinfo did not finish.\n--- tail ---\n{self.session.tail()}\n")
        match = SLOTS_LINE.search(self.session.buf[start:])
        if not match:
            self.fail(
                f"sysinfo printed no slot line.\n--- tail ---\n{self.session.tail()}\n"
            )
        return tuple(int(v) for v in match.groups())

    def test_a_module_without_a_slot_is_refused_not_relocated(self):
        used, limit, _ = self._slots()
        self.assertEqual(
            limit, SLOT_LIMIT, "the linmem.slots boot option did not take effect"
        )
        self.assertLess(
            used, limit, f"the boot alone filled the pool ({used} of {limit})"
        )

        refused = False
        for i in range(limit - used + 4):
            start = self.session.send_command("spawn linmem_hold", timeout_s=20)
            self.session.expect_from(start, b"linmem-hold:", timeout_s=20)
            if REFUSED in self.session.buf[start:]:
                refused = True
                break
        if not refused:
            self.fail(
                f"{i + 1} holders ran on a pool of {limit} with {used} in use and none "
                "was refused: linear memory relocated instead.\n--- tail ---\n"
                f"{self.session.tail()}\n"
            )
        # No read-back of the full pool: sysinfo is a WASM app too, and under
        # wasm3 (slot taken at load) a full pool refuses it as well.


if __name__ == "__main__":
    unittest.main()
