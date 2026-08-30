#!/usr/bin/env python3
"""`fsck.wfs` refuses a volume a filesystem service has mounted.

Nothing beneath this tool stops it. The block layer used to bind a unit to one
client, which made a mounted disk unreachable by accident; that arbitration was
removed when a request began naming its own target, and the tool's own header
still claimed the protection existed. What replaces it is the volume manager's
CLAIMED flag, set by a filesystem service when it mounts and consulted here --
advisory on both sides, because the volume manager is not in the I/O path
(architecture/37-volume-manager.md §5).

The distinction the cases below draw is between three answers, not two:

    block:ata:2   volume, CLAIMED by fs_wfs   -> refused
    block:ata:0   MBR disk, no volume at all  -> cannot tell, also refused
    --force                                   -> proceeds, saying why it might lie

"Cannot tell" being a refusal rather than a pass is the whole point of splitting
it out: a check that went ahead because it could not find the owner would be
exactly as dangerous as one that ignored the owner it found.
"""

import unittest

from scripts.qemu_test_framework import QemuSession, default_config

# The disk /wfs is mounted from in a default boot: a raw WFS volume with no
# partition table, so the volume manager publishes a volume for the disk itself.
WFS_DEVICE = "block:ata:2"
# The MBR disk holding the ESP. Its PARTITIONS are the volumes, so the whole-disk
# device has none -- which is what makes it the "cannot tell" case rather than a
# second claimed one.
TABLED_DEVICE = "block:ata:0"


class FsckWfsClaimTest(unittest.TestCase):
    """A mounted volume is refused; --force is the documented way past it."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.session = QemuSession(default_config(), timeout_s=200)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=180):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")
        # The claim is sent when fs_wfs mounts, so nothing below is meaningful
        # until that has happened. Asserted here rather than in a case, because a
        # boot without it makes every case below fail for the same wrong reason.
        if not cls.session.expect(b"[fs-wfs] mounted", timeout_s=120):
            cls.session.close()
            raise RuntimeError("no WFS mount in this boot; nothing would be claimed")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def _run(self, cmd: str, needles: list[bytes], timeout_s: int = 60) -> None:
        """Run `cmd` and require each needle, then the prompt back.

        Marked and expected FROM the mark, so a line the boot log already carries
        cannot satisfy an assertion about this invocation.
        """
        assert self.session is not None
        mark = self.session.mark()
        self.session.send(cmd)
        for needle in needles:
            if not self.session.expect_from(mark, needle, timeout_s=timeout_s):
                self.fail(
                    f"'{cmd}' did not produce {needle!r}\n"
                    f"--- tail ---\n{self.session.tail()}\n"
                )
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s):
            self.fail(f"no prompt after '{cmd}'\n--- tail ---\n{self.session.tail()}\n")

    def test_a_mounted_volume_is_refused(self) -> None:
        """The case the flag exists for.

        /wfs is mounted, so its volume is claimed, and checking it would read
        blocks a filesystem is concurrently writing -- reporting damage that is
        only a race.
        """
        self._run(
            f"fsck.wfs {WFS_DEVICE}",
            [f"[fsck.wfs] {WFS_DEVICE} is mounted".encode()],
        )

    def test_the_refusal_happens_before_any_block_is_read(self) -> None:
        """No finding is printed for a volume that was refused.

        A refusal that arrived after the first read would have already done the
        thing it exists to prevent, and would look identical from the exit
        status. `[fsck.wfs] checking` is the tool's first line once it starts
        work, so its absence is what says the refusal came first.
        """
        assert self.session is not None
        mark = self.session.mark()
        self.session.send(f"fsck.wfs {WFS_DEVICE}")
        self.assertTrue(
            self.session.expect_from(mark, b"wamos> ", timeout_s=60),
            f"no prompt after the refusal\n--- tail ---\n{self.session.tail()}\n",
        )
        self.assertNotIn(
            b"[fsck.wfs] checking",
            self.session.buf[mark:],
            "the tool began checking a claimed volume -- the claim is consulted "
            "too late to prevent the read it exists to prevent",
        )

    def test_force_proceeds_and_says_it_may_be_lying(self) -> None:
        """--force is the escape, and it is not silent.

        A claim can outlive its holder -- a filesystem service killed before it
        releases leaves a volume nothing would ever agree to check -- so the
        override has to exist. Findings taken past a live claim may be races, and
        the tool says so rather than presenting them as damage.
        """
        self._run(
            f"fsck.wfs --force {WFS_DEVICE}",
            [b"findings may be races"],
        )

    def test_a_device_with_no_volume_cannot_be_cleared(self) -> None:
        """ "Cannot tell" is refused, not waved through.

        block:ata:0 carries an MBR, so its partitions are the volumes and the disk
        itself has none. Nothing can say whether it is idle, and that is reported
        as its own answer rather than collapsed into "not claimed".
        """
        self._run(
            f"fsck.wfs {TABLED_DEVICE}",
            [f"[fsck.wfs] cannot tell whether {TABLED_DEVICE} is mounted".encode()],
        )


if __name__ == "__main__":
    unittest.main()
