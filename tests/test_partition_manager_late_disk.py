#!/usr/bin/env python3
"""A disk that registers under the `block` class after the partition manager has
started still has its partitions published.

Regression: 2026-08-29-partmgr-enumerates-once -- `probeAll` resolved the `block`
class exactly once at bring-up, so every disk whose driver registered afterwards
was invisible to the partition manager for the rest of the boot. Its partitions
were never published, so no `SUBSYSTEM=="partition"` rule could match them and no
filesystem on that disk could be mounted, however correct its table.

The ATA driver happens to register both its drives before the partition manager
is spawned, which is the only reason the boot image looks unaffected. virtio-blk
does not: it negotiates a PCI device, claims an MSI-X vector and sets up a
virtqueue first, and publishes its disk well after `[partmgr] ready`. Attaching a
GPT-partitioned virtio disk therefore reproduces the bug on the shipped
configuration rather than on a contrived one.

The disk is built here rather than checked in: `scripts/make_gpt_image.py` is the
tree's own GPT writer, and QEMU cannot present a GPT any other way -- a
directory-backed `fat:rw:` drive always synthesises a classic MBR.
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

# The volume label. Deliberately not "user": that label has a rule in
# `scripts/system/devmgr/rules/default.rules` that mounts it on /user, and a test
# disk that mounts itself over the real one would be testing the rule engine by
# accident. Nothing matches "late", so the partition is published and left alone,
# which is exactly the property under test.
VOLUME_LABEL = "late"
IMAGE_MIB = 64

# `block` providers a correct boot holds with this disk attached: ATA's two
# drives, the one partition the partition manager republishes on each, the
# virtio disk, and its partition. The count is the assertion that nothing
# registered twice; the ids below say which ones are present.
EXPECTED_PROVIDERS = 6

# A partition of the virtio disk, whose unit is derived from the device's PCI
# slot and so is matched by shape rather than by a literal -- the same reason
# test_virtio_blk.py does not bake the number in.
VIRTIO_PARTITION_RE = re.compile(rb"\[partmgr\] partition id=block:virtio-blk:\d+p\d+")


def _config_with_gpt_disk(disk_path: str):
    """A default config with the GPT image attached as a virtio-blk device.

    if=none keeps QEMU from attaching the drive to the default IDE bus, where it
    would become a third ATA unit -- registered by the ATA driver before the
    partition manager starts, which is the case this test exists to avoid.
    """
    return replace(
        default_config(),
        extra_args=(
            "-drive",
            f"file={disk_path},format=raw,if=none,id=lateblk",
            "-device",
            "virtio-blk-pci,drive=lateblk,id=lateblkdev",
        ),
    )


class PartitionManagerLateDiskTest(unittest.TestCase):
    """The partition manager probes disks that appear after its own bring-up."""

    session: QemuSession | None = None
    disk_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-late-disk-")
        disk_path = os.path.join(cls.disk_dir, "late.img")
        with open(disk_path, "wb") as handle:
            handle.write(build_image(IMAGE_MIB, VOLUME_LABEL, []))
        cls.session = QemuSession(_config_with_gpt_disk(disk_path), timeout_s=200)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=180):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.disk_dir:
            shutil.rmtree(cls.disk_dir, ignore_errors=True)
            cls.disk_dir = None

    def test_the_virtio_disk_registers_after_the_partition_manager(self) -> None:
        """The precondition, asserted rather than assumed.

        Everything below only means something if this disk really is a late
        arrival. Were the spawn order ever to change so that virtio-blk publishes
        first, the partition test would keep passing while testing nothing, and
        the bug it guards could come back unnoticed. This fails loudly instead.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[partmgr] ready disks=", timeout_s=90),
            "the partition manager never finished bring-up",
        )
        ready = self.session.buf.find(b"[partmgr] ready disks=")
        published = self.session.buf.find(
            b"[virtio-blk] published id=block:virtio-blk:"
        )
        self.assertNotEqual(
            published, -1, "the virtio-blk driver never published its disk"
        )
        self.assertGreater(
            published,
            ready,
            "virtio-blk registered BEFORE the partition manager finished "
            "enumerating, so this test no longer exercises a late arrival -- the "
            "spawn order changed and the late-disk case needs a new lever",
        )

    def test_the_late_disk_gets_its_partitions_published(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(VIRTIO_PARTITION_RE, timeout_s=90),
            "the partition manager published no partition for the virtio disk. "
            "Its GPT is the same one scripts/make_gpt_image.py writes for /user, "
            "which parses on ATA, so the table is not the problem: the disk "
            "registered under the `block` class after the partition manager had "
            "already enumerated it, and nothing made it look again",
        )

    def test_every_disk_and_partition_appears_under_the_block_class(self) -> None:
        """The count, from the guest's own view of the registry.

        Asserted through `blkinfo` rather than by reading the partition
        manager's log a second time, because the contract is what a CLIENT
        resolving the class sees. A partition that was logged but never
        registered would pass the test above and fail this one.
        """
        assert self.session is not None
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(
                f"[blkinfo] providers={EXPECTED_PROVIDERS}".encode(), timeout_s=60
            ),
            f"the block class did not hold {EXPECTED_PROVIDERS} providers -- "
            "ATA's two drives and their partitions, the virtio disk and its "
            "partition. A count of 5 means the virtio disk's partition is "
            "missing; a higher count means something registered twice",
        )
