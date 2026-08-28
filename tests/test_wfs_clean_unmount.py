"""The orderly shutdown, asserted where it is actually observable: across a boot.

WFS_STATE_CLEAN is the only thing that tells a mount its log holds nothing to
replay, and only an orderly shutdown writes it. Every other suite runs against
the WFS drive's copy-on-write overlay, which discards the write that matters
here -- so this one owns a scratch copy of the image and turns the overlay off
(`wfs_snapshot=False`), because what it asserts is that a write reached media.

Two boots would not be enough. "The second boot mounted from a clean unmount"
proves nothing unless a boot that was NOT shut down cleanly says something else,
so the crash class below is the control: same image, same app, the VM killed
instead of halted. Without it the clean assertion would still pass against a
driver that printed that line unconditionally, and against one that never marked
the volume dirty in the first place.

The control asserts the volume is NOT clean rather than that it REPLAYS. A kill
after the writer finished leaves a volume that is dirty with an empty log --
every transaction it made had already been checkpointed and retired -- so
demanding a replay would be demanding that the kill land inside a transaction,
which a test cannot time. Dirty-versus-clean is the distinction the clean unmount
actually makes.

Each class needs its own scratch image: one leaves the volume clean and the other
leaves it dirty, so sharing one would make the order they run in part of the
result.

Four boots is four times the exposure to the boot wedge tracked in
docs/TASKS.md, which is the dominant reason this file fails when it fails.

Regression: 2026-08-28-ata-cache-flush-budget

Writing this test is what found that bug, and the bug was invisible to every
other suite by construction. ATA CACHE FLUSH was waiting on the budget sized for
a sector transfer; against the overlay a flush returns instantly, so nothing
noticed, but against a backing store that really persists the driver gave up
while the drive was still BSY and issued its next command into a desynchronised
channel. The write app hung indefinitely -- measured at 302s against a 13s
baseline before the budget was separated out. There is no host unit test for it:
the block stub models replies, not the drive timing the bug is made of.

What this file covers of that bug is the flush ending a WRITE, because that is
what a guest write path issues. The standalone barrier -- BLOCK_IPC_FLUSH_REQ
from the journal -- shares the budget but is not separately provoked here, since
nothing lets a test make one flush slow on demand.
"""

import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


def _scratch_image(prefix: str) -> tuple[str, str]:
    """A private copy of the WFS volume, and the directory holding it.

    Never the shared build/wfs.img: nothing regenerates that once a guest has
    made it newer than its mkfs inputs, so a write through it would silently
    become the fixture every other suite reads.
    """
    cfg = default_config()
    if not cfg.wfs_image or not os.path.exists(cfg.wfs_image):
        raise unittest.SkipTest(f"no WFS image at {cfg.wfs_image!r}")
    tmp = tempfile.mkdtemp(prefix=prefix)
    path = os.path.join(tmp, "wfs.img")
    shutil.copy(cfg.wfs_image, path)
    return path, tmp


def _boot(image: str, timeout_s: int = 150) -> QemuSession:
    cfg = default_config()
    cfg.wfs_image = image
    # Assignment rather than dataclasses.replace: replace() re-runs
    # __post_init__, which refills wfs_image from the environment.
    cfg.wfs_snapshot = False
    session = QemuSession(cfg, timeout_s=timeout_s, echo=False)
    session.start()
    if not session.expect(b"wamos> "):
        tail = session.tail()
        session.close()
        raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")
    return session


class WfsShutdownTestBase(unittest.TestCase):
    prefix = ""
    image = ""
    tmpdir = ""

    @classmethod
    def setUpClass(cls):
        cls.image, cls.tmpdir = _scratch_image(cls.prefix)

    @classmethod
    def tearDownClass(cls):
        if cls.tmpdir:
            shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def _write(self, session: QemuSession) -> None:
        """Dirty the volume, exactly once per image.

        wfs_write_smoke is deliberately not idempotent -- it creates, renames and
        unlinks -- so it runs on the FIRST boot only. What the second boot checks
        is that its bytes are still there.

        The timeout is generous because this is the one suite whose writes reach
        real media: every journal barrier becomes a cache flush the host actually
        honours, where the rest of the suite discards them with the overlay.
        """
        mark = session.mark()
        session.send("wfs_write_smoke")
        if not session.expect_from(mark, b"wfs-write-smoke: ok", timeout_s=180):
            self.fail(f"the write app failed\n--- tail ---\n{session.tail(4096)}\n")

    def _expect_data_survived(self, session: QemuSession) -> None:
        """The inline file wfs_write_smoke rewrote still holds its bytes.

        Its pattern starts 0x40 0x47 0x4E 0x55 -- "@GNU" -- which is both
        printable and nothing the pristine mkfs content contains, so finding it
        means the previous boot's write reached the media rather than dying with
        the overlay.
        """
        mark = session.mark()
        session.send("cat /wfs/hello.txt")
        if not session.expect_from(mark, b"@GNU", timeout_s=30):
            self.fail(
                f"the previous boot's bytes are gone\n--- tail ---\n{session.tail(4096)}\n"
            )

    def _mount_line(self, session: QemuSession) -> str:
        """Which of the three paths the volume took on THIS boot.

        Scans the whole session buffer, not a tail window: the line is printed
        early in boot and the desktop's log keeps growing behind it, so any fixed
        window is a size the test silently starts failing at.
        """
        for line in session.tail(max_bytes=len(session.buf)).splitlines():
            if "[fs-wfs] mounted from" in line or "[fs-wfs] mounted after" in line:
                return line.strip()
        self.fail(f"no fs-wfs mount line\n--- tail ---\n{session.tail(4096)}\n")
        return ""


class WfsCleanUnmountTest(WfsShutdownTestBase):
    """Write, halt in order, and mount the same volume again."""

    prefix = "wasmos-wfs-clean-"

    def test_a_clean_halt_leaves_the_volume_clean_and_writable(self):
        first = _boot(self.image)
        try:
            self._write(first)
            mark = first.mark()
            first.send("halt")
            # Assertions, not teardown. The sequence has to run (the process
            # manager reaches the participants), the participant has to do its
            # work (the volume is recorded clean), and only then does the machine
            # go down -- QEMU exiting on its own is what says the power-off came
            # from the completed sequence rather than from its stall fallback.
            self.assertTrue(
                first.expect_from(mark, b"[pm] shutdown: begin", timeout_s=30),
                f"the shutdown sequence never ran\n--- tail ---\n{first.tail(4096)}\n",
            )
            self.assertTrue(
                first.expect_from(mark, b"[fs-wfs] unmounted clean", timeout_s=30),
                f"the volume was not recorded clean\n--- tail ---\n{first.tail(4096)}\n",
            )
            self.assertTrue(
                first.wait_for_exit(30),
                f"halt did not power off QEMU\n--- tail ---\n{first.tail(4096)}\n",
            )
        finally:
            first.close()

        second = _boot(self.image)
        try:
            self.assertEqual(
                self._mount_line(second),
                "[fs-wfs] mounted from a clean unmount",
                "the volume was not left clean by the previous boot",
            )
            self._expect_data_survived(second)
        finally:
            second.send("halt")
            second.force_stop()
            second.close()


class WfsUncleanShutdownTest(WfsShutdownTestBase):
    """The control: the same volume, killed instead of halted.

    Kept in its own class so its dirty image cannot reach the clean case above.
    """

    prefix = "wasmos-wfs-unclean-"

    def test_a_killed_machine_does_not_leave_the_volume_clean(self):
        first = _boot(self.image)
        try:
            self._write(first)
            # No halt: the machine dies with the volume still marked dirty, which
            # is what every boot did before an orderly shutdown existed.
            first.force_stop()
        finally:
            first.close()

        second = _boot(self.image)
        try:
            self.assertNotEqual(
                self._mount_line(second),
                "[fs-wfs] mounted from a clean unmount",
                "a killed machine must not leave the volume looking cleanly unmounted",
            )
            # The data is there either way. Losing the clean mark costs the next
            # mount a look at the log, not the writes that already landed -- and
            # that is what makes an unclean shutdown survivable rather than
            # destructive.
            self._expect_data_survived(second)
        finally:
            second.send("halt")
            second.force_stop()
            second.close()


if __name__ == "__main__":
    unittest.main()
