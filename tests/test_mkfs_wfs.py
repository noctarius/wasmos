#!/usr/bin/env python3
"""`mkfs.wfs` writes a volume the rest of the system agrees is a volume.

A formatter cannot be checked against itself. Everything asserted here is
asserted by a DIFFERENT implementation reading what the formatter wrote:

    fsck.wfs      the checker, which walks the metadata and the bitmaps
    fs_wfs        the driver, which mounts it and reports how it got there
    volinfo       the volume manager's view, which probes the superblock

A test that merely re-read the image with the formatter's own code would pass
against a formatter that is internally consistent and wrong.

The disk is a ZEROED scratch image in place of the usual WFS fixture, which makes
it the case the tool is actually for: a device with no filesystem, and therefore
no volume for anything to have claimed. The default boot's WFS disk is mounted
throughout, so formatting it would only ever exercise the refusal.

`[fs-wfs] mounted from a clean unmount` is the strongest of these assertions and
the least obvious. WFS_STATE_CLEAN means the volume's log holds nothing to
replay; a driver reaching it on a freshly formatted volume says the superblock's
state field, its checksum, and the empty journal behind it were all written the
way a mount expects to find them. A volume that merely parsed would say
something else.

Regression: 2026-09-21-mkfs-wfs-guest-formatter
"""

import os
import shutil
import tempfile
import unittest

from scripts.qemu_test_framework import QemuSession, default_config

# The scratch disk takes the place of build/wfs.img, so it arrives as ata unit 2.
DEVICE = "block:ata:2"
# Matches the fixture it replaces, so the geometry the rest of the suite is
# written against does not change with it.
IMAGE_BYTES = 16 * 1024 * 1024


class MkfsWfsTest(unittest.TestCase):
    """Formatting a blank disk, then proving the result is a WFS volume."""

    session: QemuSession | None = None
    tmpdir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmpdir = tempfile.mkdtemp(prefix="wasmos-mkfs-")
        image = os.path.join(cls.tmpdir, "blank.img")
        with open(image, "wb") as handle:
            handle.truncate(IMAGE_BYTES)

        cfg = default_config()
        # Never build/wfs.img: this test destroys what it is given, and that file
        # is a fixture several other suites read as a fixed input.
        cfg.wfs_image = image
        cls.session = QemuSession(cfg, timeout_s=300)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=200):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.tmpdir:
            shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def _run(self, cmd: str, needles: list[bytes], timeout_s: int = 240) -> bytes:
        """Run `cmd`, require each needle, and return everything it printed.

        Marked and expected FROM the mark, so a line an earlier case produced
        cannot satisfy an assertion about this one.
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
        return self.session.buf[mark:]

    def test_1_a_blank_disk_is_formatted(self) -> None:
        """The format runs and reports the geometry it derived from the device.

        Nothing about the size is passed in -- the tool asks the backend how big
        the disk is -- so the block count appearing at all is what says IDENTIFY
        was consulted rather than a default assumed.
        """
        out = self._run(
            f"mkfs.wfs {DEVICE}",
            [b"[mkfs.wfs] formatting", b"[mkfs.wfs] done"],
        )
        self.assertIn(
            b"4096 blocks",
            out,
            "a 16 MiB volume at the default 4096-byte block size is 4096 blocks; "
            "a different count means the size did not come from the device\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )

    def test_2_the_free_area_is_left_alone_by_default(self) -> None:
        """Only the blocks a format defines are written, and the tool says so.

        The distinction is the difference between 4 seconds and 110 on this
        volume, and it is only sound because nothing reaches a free block. The
        count is printed precisely so the claim is checkable from outside.
        """
        out = self._run(f"mkfs.wfs {DEVICE}", [b"[mkfs.wfs] done"])
        self.assertIn(
            b"133 written",
            out,
            "the metadata plus the root's directory block is 133 blocks of the "
            "4096; a different number means the skip rule changed and the "
            "assertion below about what fsck finds no longer measures it\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )

    def test_3_the_volume_layer_sees_the_new_filesystem(self) -> None:
        """The format is visible WITHOUT a reboot, which it was not before.

        A device is probed once, when it arrives under the `block` class, so a
        disk formatted from inside the guest kept whatever the volume manager saw
        on it at boot -- `unknown` for a blank one. Every `SUBSYSTEM=="volume"`
        rule reads that layer, so the filesystem existed and nothing that decides
        what gets mounted could tell.

        mkfs.wfs sends VOLUME_IPC_RESCAN_REQ once its write has landed. Asserted
        here BEFORE anything mounts the volume, because a mount would make the
        type observable by other means and this case would stop measuring the
        rescan.
        """
        out = self._run("volinfo", [b"[volinfo] volumes="])
        line = b""
        for candidate in out.split(b"\n"):
            if f"volume:{DEVICE}".encode() in candidate:
                line = candidate
                break
        self.assertTrue(
            line,
            f"no volume for {DEVICE}\n--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        self.assertIn(
            b" wfs ",
            line,
            "the volume still carries the type it had before the format; the "
            "rescan did not reach the volume manager or did not update in place\n"
            f"--- line ---\n{line.decode('utf-8', 'replace')}\n",
        )
        self.assertIn(
            b"volumes=3",
            out,
            "the volume count changed, so the rescan added a second volume for "
            "the device instead of updating the one already there\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )

    def test_4_the_checker_finds_it_consistent(self) -> None:
        """A second implementation reads what the formatter wrote.

        fsck.wfs walks the superblock, the group descriptors, the object table
        and both bitmaps, and recomputes every checksum from the volume's own
        uuid. `consistent` means the writer and the reader agree; `1 objects` is
        the root and nothing else, which is what an empty volume should hold.
        """
        self._run(
            f"fsck.wfs {DEVICE}",
            [b"[fsck.wfs] 1 objects", b"[fsck.wfs] consistent"],
        )

    def test_5_the_driver_mounts_it_from_a_clean_unmount(self) -> None:
        """The volume is not merely parseable -- it is CLEAN.

        A mount that had to replay, or that found the state field unreadable,
        would print something else. Reaching the clean path means the superblock,
        its checksum and the empty journal were all written the way a mount
        expects, which is more than "the magic matched".
        """
        self._run(
            f"mount -t wfs /scratch {DEVICE}",
            [b"[fs-wfs] mounted from a clean unmount", b"mount: mounted"],
        )

    def test_6_the_volume_manager_sees_a_mounted_volume(self) -> None:
        """The volume reaches the layer that arbitrates access to it.

        `claimed` is the flag fs_wfs sets while it holds a volume, and it is what
        mkfs.wfs and fsck.wfs consult before touching a device. Its presence here
        closes the loop the next case opens.
        """
        out = self._run("volinfo", [b"[volinfo] volumes="])
        self.assertIn(
            f"volume:{DEVICE}".encode(),
            out,
            f"no volume for {DEVICE}\n--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        self.assertIn(
            b"claimed",
            out,
            "the freshly mounted volume is not claimed, so nothing would stop a "
            "format from running under the filesystem holding it\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )

    def test_7_a_mounted_volume_is_refused(self) -> None:
        """The check that makes this tool safe to have.

        Formatting under a live filesystem destroys the volume beneath a service
        still writing to it. Nothing below mkfs.wfs prevents that -- the block
        layer has no per-client arbitration -- so the refusal here is the only
        thing standing in the way, and it has to come before the first write.
        """
        out = self._run(
            f"mkfs.wfs {DEVICE}", [b"is mounted; unmount it before formatting"]
        )
        self.assertNotIn(
            b"[mkfs.wfs] formatting",
            out,
            "the tool began formatting a mounted volume -- the claim is "
            "consulted too late to prevent the write it exists to prevent\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )

    def test_8_an_unmounted_volume_can_be_reformatted(self) -> None:
        """Releasing the claim is what makes the device available again.

        Ordered last because it leaves the disk formatted but unmounted, and
        because it is the pair to the case above: the refusal must be a refusal
        of MOUNTED volumes, not of every volume that already carries a
        filesystem.
        """
        self._run("umount /scratch", [b"[fs-wfs] unmounted clean"])
        self._run(f"mkfs.wfs {DEVICE}", [b"[mkfs.wfs] done"])
        self._run(f"fsck.wfs {DEVICE}", [b"[fsck.wfs] consistent"])


if __name__ == "__main__":
    unittest.main()
