#!/usr/bin/env python3
"""A partition table rewritten under live devices is reconciled, not rebuilt.

A disk's table is read once, when the disk arrives under the `block` class.
Until now nothing re-read it, so a table rewritten from inside the guest left
every partition device exactly as it was at boot -- and there was no way to
retire one, because retiring means withdrawing a device from the device
manager's inventory AND unregistering it from the `block` class, and the second
half had no guest-side path at all.

What is asserted here is the DIFFERENCE between a rescan and a republish, which
is invisible from a count of devices. Both leave a device per table entry. Only
a reconciliation leaves an unchanged partition untouched, and that matters
because a filesystem may be mounted on one: re-registering its class or
re-announcing its descriptor would disturb a live mount to restate what is
already true. The test for it is a negative -- no new publish line for a disk
whose table did not change -- which is why it is stated explicitly rather than
inferred from `blkinfo` agreeing with itself.

The table is destroyed with `mkfs.wfs` pointed at the WHOLE disk rather than at
a partition. A 4096-byte block zero covers LBAs 0 through 7, which is the
protective MBR and the GPT header both, so the disk afterwards genuinely holds
no table -- the case the retirement path exists for and the one `fdisk` will
create deliberately.

This file is also the regression test for a second defect it uncovered. Only
`fs_wfs` ever claimed a volume; `fs_fat` never did, so the claim -- the one live
record of what is mounted -- was blind to every FAT mount. Case 4 below retired
a partition out from under a live FAT filesystem until `fs_fat` learned to
claim, and the same gap let `mkfs.wfs` and `fsck.wfs` write over a mounted FAT
volume.

Regression: 2026-10-18-partition-manager-rescan
Regression: 2026-10-18-fs-fat-never-claimed-its-volume
"""

import os
import re
import shutil
import sys
import tempfile
import unittest
from dataclasses import replace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.make_gpt_image import build_image
from scripts.qemu_test_framework import QemuSession, default_config

# Nothing in scripts/system/devmgr/rules matches this label, so the partition is
# published and then left alone -- which is what lets this test decide when it
# is mounted and when it is not.
VOLUME_LABEL = "spare"
IMAGE_MIB = 64
MOUNT_POINT = "/spare"

DISK_RE = re.compile(rb"\[virtio-blk\] published id=(block:virtio-blk:\d+)")


def _config_with_gpt_disk(disk_path: str):
    """A default config with the GPT image attached as a virtio-blk device.

    if=none keeps QEMU from attaching the drive to the default IDE bus, where it
    would become another ATA unit and share a backend with the fixtures the rest
    of the suite reads.
    """
    return replace(
        default_config(),
        extra_args=(
            "-drive",
            f"file={disk_path},format=raw,if=none,id=rescanblk",
            "-device",
            "virtio-blk-pci,drive=rescanblk,id=rescanblkdev",
        ),
    )


class PartitionRescanTest(unittest.TestCase):
    """Rescanning a disk whose table changed, and one whose table did not."""

    session: QemuSession | None = None
    disk_dir: str | None = None
    disk_id: str = ""

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-rescan-disk-")
        disk_path = os.path.join(cls.disk_dir, "rescan.img")
        with open(disk_path, "wb") as handle:
            handle.write(build_image(IMAGE_MIB, VOLUME_LABEL, []))
        cls.session = QemuSession(_config_with_gpt_disk(disk_path), timeout_s=300)
        cls.session.start()
        cls.session.require_prompt(timeout_s=200)

        # The unit is derived from the device's PCI slot, so it is read out of
        # the boot log rather than baked in.
        if not cls.session.expect(DISK_RE, timeout_s=90):
            raise AssertionError("the virtio-blk driver never published its disk")
        match = DISK_RE.search(cls.session.buf)
        assert match is not None
        cls.disk_id = match.group(1).decode()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.disk_dir:
            shutil.rmtree(cls.disk_dir, ignore_errors=True)
            cls.disk_dir = None

    @property
    def partition_id(self) -> str:
        return f"{self.disk_id}p1"

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

    def test_1_the_disk_arrived_with_its_partition_published(self) -> None:
        """The precondition everything below measures a change against."""
        assert self.session is not None
        self.assertTrue(
            self.session.expect(
                f"[partition-manager] partition id={self.partition_id}".encode(),
                timeout_s=90,
            ),
            "the partition manager never published the disk's partition, so "
            "there is nothing for a rescan to reconcile",
        )

    def test_2_an_unchanged_table_leaves_its_partitions_alone(self) -> None:
        """A rescan that finds no change must do NOTHING, not redo the publish.

        The count alone cannot tell the two apart -- a driver that retired and
        republished every partition would report the same one -- so what is
        asserted is the absence of a publish line. A republish here would tear
        the class registration out from under any filesystem mounted on the
        partition and hand the device manager a record it already holds.
        """
        out = self._run(
            f"partprobe {self.disk_id}",
            [b"[partprobe] " + self.disk_id.encode() + b" rescanned; 1 partitions"],
        )
        self.assertNotIn(
            f"[partition-manager] partition id={self.partition_id}".encode(),
            out,
            "the rescan republished a partition whose extent did not change\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        self.assertNotIn(
            b"[partition-manager] partition retired",
            out,
            "the rescan retired a partition the table still describes\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )

    def test_3_a_device_that_is_not_a_disk_is_refused(self) -> None:
        """An id naming nothing is answered, not ignored.

        A class instance is a fingerprint, so a mistyped id resolves to no
        provider rather than to the wrong one; the manager has to say so, or a
        caller that typed a partition's id instead of its disk's waits forever
        for a reply to a request nobody is serving.
        """
        self._run(
            f"partprobe {self.partition_id}",
            [b"was not rescanned"],
        )

    def test_4_a_mounted_partition_cannot_be_retired(self) -> None:
        """The refusal that makes retirement safe to attempt at all.

        The table is destroyed underneath a live mount. The partition no longer
        appears in any table, so the diff marks it for retirement -- and the
        VOLUME manager, which holds the claim a filesystem takes when it mounts
        one, is what refuses. Retiring it anyway would strip the device out of
        discovery and out of the inventory while the mount still wrote to it.

        The device manager cannot answer this. Its `active_service` field is
        never set (TASKS `[BUG][P3]`), so its inventory reads "no filesystem"
        for every device, mounted or not -- which is why the check is where it
        is rather than behind the unpublish opcode.
        """
        self._run(
            f"mount -t fat {MOUNT_POINT} {self.partition_id}", [b"mount: mounted"]
        )
        self._run(f"mkfs.wfs {self.disk_id}", [b"[mkfs.wfs] done"])
        out = self._run(
            f"partprobe {self.disk_id}",
            [b"could not be retired"],
        )
        self.assertIn(
            b"a filesystem is mounted on it",
            out,
            "the claim was not consulted, so nothing stopped a mounted "
            "partition being withdrawn\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        self.assertNotIn(
            b"[device-manager] block retire",
            out,
            "the device manager retired the record although the claim check "
            "refused, so the stages ran out of order\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        # blkinfo enumerates the `block` CLASS, which is the half a refusal has
        # to leave intact: a partition withdrawn from discovery while its mount
        # stayed up is the exact failure the refusal exists to prevent.
        blk = self._run("blkinfo", [b"[blkinfo] providers="])
        self.assertIn(
            self.partition_id.encode(),
            blk,
            "the refused partition left the `block` class anyway\n"
            f"--- output ---\n{blk.decode('utf-8', 'replace')}\n",
        )

    def test_5_an_unmounted_partition_is_retired_from_both_views(self) -> None:
        """Releasing the mount is what lets the retirement complete.

        Both halves are asserted, because retiring from one view and not the
        other is worse than neither -- the inventory and the class registry
        would then disagree about what exists. The device manager's half shows
        in its own log line; the class half is what `blkinfo` reads, since it
        enumerates the `block` class rather than the inventory.
        """
        self._run(f"umount {MOUNT_POINT}", [b"umount"])
        out = self._run(
            f"partprobe {self.disk_id}",
            [b"[partprobe] " + self.disk_id.encode() + b" rescanned; 0 partitions"],
        )
        self.assertIn(
            f"[partition-manager] partition retired id={self.partition_id}".encode(),
            out,
            "the partition was not retired, although nothing held it and no "
            "table describes it\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        self.assertNotIn(
            b"could not be retired",
            out,
            "the retirement was refused with nothing holding the partition\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        self.assertIn(
            f"[device-manager] block retire id={self.partition_id}".encode(),
            out,
            "the device manager never withdrew the record, so the inventory "
            "still holds a partition the class no longer serves\n"
            f"--- output ---\n{out.decode('utf-8', 'replace')}\n",
        )
        blk = self._run("blkinfo", [b"[blkinfo] providers="])
        self.assertNotIn(
            self.partition_id.encode(),
            blk,
            "the retired partition is still registered under the `block` "
            "class, so it can still be resolved and mounted\n"
            f"--- output ---\n{blk.decode('utf-8', 'replace')}\n",
        )


if __name__ == "__main__":
    unittest.main()
