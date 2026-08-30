"""The client-owned window surface protocol, end to end.

GFX_IPC_ALLOC_SHARED_BUFFER had the compositor allocate a buffer and grant it to
the app, which inverts the transfer-buffer ownership rule: a server cannot own a
buffer it hands to a client, because release is owner-only and nothing tells a
server when a client stopped reading. GFX_IPC_GET_SURFACE_SPEC /
GFX_IPC_ATTACH_SURFACE / GFX_IPC_DETACH_SURFACE put the app on the owning side.

What this pins, per marker, is the part that is not visible from the compositor's
own logs:

  spec ok         the spec reply describes the window rather than the caller's
                  guess -- stride and byte size large enough for the extent, and
                  the extent echoed back
  attach ok       the compositor accepted a buffer it does not own, which means
                  it resolved (buffer_id, borrow_id) into a live grant; neither
                  id is derivable on its side
  present ok      an attached surface composites through the same path an
                  allocated one did
  busy deny ok    detaching a surface that is still a window's current buffer is
                  REFUSED. Without this the app could release out from under a
                  mid-composite read, and there is no unborrow notification that
                  would tell the compositor
  detach ok       after the window is gone the surface withdraws cleanly, which
                  is what returns the entry to the 32-slot native borrow-mapping
                  pool shared by every native service

The app exits non-zero on any failure, but each stage prints its own marker so a
red run says which one broke rather than only that it broke.
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


class SurfaceAttachTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = QemuSession(default_config(), timeout_s=200, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=180):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_a_client_owned_surface_attaches_presents_and_detaches(self):
        # Mark first: expect() scans forward from wherever the stream sits, so a
        # bare one could match a marker an earlier test left behind.
        mark = self.session.mark()
        self.session.send("spawn /apps/surface_attach")

        for needle in (
            b"[test] surface attach spec ok",
            b"[test] surface attach attach ok",
            b"[test] surface attach present ok",
            b"[test] surface attach busy deny ok",
            b"[test] surface attach detach ok",
            b"[test] surface attach done",
        ):
            if not self.session.expect_from(mark, needle, timeout_s=60):
                self.fail(
                    f"{needle!r} never arrived -- the stage before it is where the "
                    f"protocol broke.\n--- tail ---\n{self.session.tail()}\n"
                )


if __name__ == "__main__":
    unittest.main()
