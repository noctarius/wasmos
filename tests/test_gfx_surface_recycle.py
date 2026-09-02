"""Resizing windows on a live display must not break linear-memory mapping.

An xfer-buffer overlay mapped over SLOT-BACKED linear memory replaces that
page's own frame, and `warp_linmem_place_phys` frees the frame it replaced --
otherwise every map/unmap cycle leaks a page. The unmap has to put a frame back.
It did not, so the page stayed inside what linear memory counts as COMMITTED
with nothing behind it, and since `warp_mem_ring3_map_linmem` walks that
committed count to publish the ring-3 user window, ONE such page broke EVERY
later mapping for that process -- not just its own.

libui's `ui_release_surface` unmaps on every `ui_realloc_buffer`, so a single
window resize was enough to poison a guest. The apps that resize most were the
ones that fell over: a half-drawn calculator, a gfx_smoke that never appeared.

Why this test needs a DISPLAY: every report of the failure came from a graphical
boot, and no serial boot ever showed it -- twelve `run-qemu-test` runs said "not
reproducible". A serial boot does not give the compositor a framebuffer to own,
so the repeated surface exchanges that create the hole never happen.
`display = "none"` keeps the VGA device without opening a window.

The absence assertions are the point of the case, so it carries a VACUITY GUARD:
a boot where gfx_smoke never reached its resize would satisfy them while testing
nothing. `resize` and `visible done` are asserted first, and only then is the
absence of the failure meaningful.

WHAT THIS CASE DOES NOT DO, stated because a reader would otherwise assume it:
it was NOT demonstrated to fail against the unfixed tree. It was run against a
worktree with the fix reverted -- same runtime, same SMP count, same display
configuration -- and passed. The reason is in the trigger's precondition: only
SLOT-BACKED linear memory is affected, and a block moves into a dedicated VA slot
only when it is REALLOCATED while the reserve hint is armed and a slot is free
(`src/kernel/warp/shim.cpp`, warp_linmem_move). Whether a given app is slot-backed
therefore depends on startup order and slot availability, which is why the same
tree fails on one machine and not another.

So this is a GUARD, not a demonstration: it will catch the regression wherever
the preconditions hold -- they held reliably on the machine the bug was reported
from -- and it asserts the mechanism directly, so it cannot be satisfied by a
change that hides the symptom. A deterministic version needs an app forced onto a
slot; see docs/TASKS.md.

Regression: 2026-09-02-overlay-unmap-leaves-uncommitted-page
"""

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

# The kernel's report from warp_mem_ring3_map_linmem when it walks the committed
# page count and finds a page with no backing frame. Asserted on directly because
# it names the MECHANISM: a future change that hides the symptom while leaving
# the invariant broken would still print this.
NO_FRAME = b"[warp] r3 linmem: committed page has no frame"
# The consequence one hole produces for every later mapping in that process.
SYNC_FAILED = b"[warp] linmem place: user-window sync failed"
# What the app reports when the mapping it needs for the resized surface fails.
ALLOC_FAILED = b"gfx smoke resize-alloc failed"


class GfxSurfaceRecycleTest(unittest.TestCase):
    """A graphical boot where windows resize, and the mapping still works."""

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        # screendump-style config: -nographic removes the VGA device, and without
        # it the compositor never owns a framebuffer and never paints.
        cfg.nographic = False
        cfg.display = "none"
        cls.session = QemuSession(cfg, timeout_s=240, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=200):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")
        # Wait for gfx_smoke to finish its resize sequence and settle. Its wait
        # loop is the last thing it prints, so reaching it means every surface
        # exchange the boot performs has happened.
        cls.settled = cls.session.expect(
            b"gfx smoke waiting close-request", timeout_s=120
        )
        # The apps that resize on their own schedule (calculator, menu-bar, the
        # libui demo) are still painting; give them a moment so their map/unmap
        # cycles are inside the captured log too.
        time.sleep(4)

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _log(self) -> bytes:
        return bytes(self.session.buf)

    def test_the_boot_actually_resized_a_window(self):
        """The vacuity guard, asserted as its own case so a boot that never got
        this far fails HERE rather than silently passing the two below."""
        log = self._log()
        self.assertTrue(
            self.settled,
            f"gfx_smoke never reached its wait loop, so nothing below is "
            f"tested\n--- tail ---\n{self.session.tail()}\n",
        )
        self.assertIn(
            b"gfx smoke event resize",
            log,
            "no resize happened, so the surface exchange under test never ran",
        )
        self.assertIn(
            b"gfx smoke visible done",
            log,
            "the resize did not complete, so the mapping after it never ran",
        )

    def test_no_committed_page_is_left_without_a_frame(self):
        """The mechanism. An overlay unmap that does not restore a frame leaves a
        page inside the committed count with nothing behind it, and the kernel
        says so every time it walks that count afterwards."""
        log = self._log()
        count = log.count(NO_FRAME)
        self.assertEqual(
            count,
            0,
            f"{count} committed page(s) had no frame -- an overlay unmap left "
            f"linear memory inconsistent\n--- tail ---\n{self.session.tail()}\n",
        )

    def test_the_ring3_window_keeps_publishing_and_surfaces_keep_mapping(self):
        """The consequence, at both levels: the kernel can still publish the
        ring-3 window, and the app can still map the surface it resized to."""
        log = self._log()
        self.assertNotIn(
            SYNC_FAILED,
            log,
            f"the ring-3 linear-memory window could not be published\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertNotIn(
            ALLOC_FAILED,
            log,
            f"a resized surface could not be mapped\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )


if __name__ == "__main__":
    unittest.main()
