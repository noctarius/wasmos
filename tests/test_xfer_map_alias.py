"""Does a guest's xfer-buffer mapping alias the buffer's own frames?

`xfer_buffer_map` promises that an owned buffer's bytes are "directly
addressable" at the returned linear-memory offset. Whether that holds for a
GUEST depends on the runtime's linear-memory model, and the two differ: under
WARP the guest's linear memory is the mapped frames, while the wasm3 interpreter
reads and writes linear memory through its own kernel-side buffer, so an overlay
that only rewrites the process page tables would be invisible to it. The same
distinction is recorded for the shmem overlay in examples/rust/tetris/tetris.rs,
which drops its per-frame flush and is WARP-only as a result.

The graphics migration off shmem depends on that promise holding for a guest, so
this pins it rather than assuming it. The probe is deterministic -- the mapping
either aliases or it does not -- and both directions are asserted separately so a
failure says which half broke:

    guest -> frames   a compositor reading an app's surface would see stale
                      pixels if this fails
    frames -> guest   an app reading a buffer a service filled would see stale
                      data if this fails

This is a CHARACTERIZATION test: it records what the memory model does today
under each runtime. If a runtime reports MISSING, this becomes the red test for
whichever fix follows -- either the overlay learns to remap the interpreter's
view, or the guest ABI documents that a mapping needs an explicit write-back
there.

RUN IT UNDER BOTH RUNTIMES. A WARP-only pass proves nothing about wasm3, which
is the runtime the question is actually about:

    cmake -S . -B build-wasm3 -DWASMOS_DOTCONFIG=configs/wasm3_smp_defconfig
    cmake -S . -B build-warp  -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig
"""

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


class XferMapAliasTest(unittest.TestCase):
    session = None

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=150, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_a_guest_mapping_aliases_the_buffers_frames(self):
        """A guest's own mapping of an xfer buffer is the buffer, both ways."""
        assert self.session is not None

        # mark() before send, then expect_from(), so this test reads only output
        # its own command produced. A bare expect() scans forward from wherever
        # the stream happens to sit and would consume what a later test needs.
        mark = self.session.mark()
        self.session.send("spawn /apps/xfer_map_alias")

        if not self.session.expect_from(mark, b"[test] xfer map alias done", timeout_s=60):
            self.fail(
                "the probe never finished -- it did not reach its own exit, so "
                "neither direction was measured.\n--- tail ---\n"
                f"{self.session.tail()}\n"
            )

        # Both halves are already in the buffer by now, since the probe prints
        # them before its done marker; expect_from searches from `mark` without
        # consuming, so the same mark serves every assertion.
        self.assertTrue(
            self.session.expect_from(
                mark, b"[test] xfer map alias guest-to-frames ok", timeout_s=5
            ),
            "writes THROUGH the mapping did not reach the buffer's frames, so a "
            "guest that draws into a mapped buffer publishes nothing and a "
            "compositor would composite stale pixels.\n--- tail ---\n"
            f"{self.session.tail()}\n",
        )
        self.assertTrue(
            self.session.expect_from(
                mark, b"[test] xfer map alias frames-to-guest ok", timeout_s=5
            ),
            "a kernel-side write to the buffer was not visible through the "
            "mapping, so a guest reading a mapped buffer sees stale data."
            "\n--- tail ---\n"
            f"{self.session.tail()}\n",
        )


if __name__ == "__main__":
    unittest.main()
