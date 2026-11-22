#!/usr/bin/env python3
"""A disk that registers under the `block` class has its partitions published,
however long after the partition manager it arrives.

Regression: 2026-08-29-partmgr-enumerates-once -- `probeAll` resolved the `block`
class exactly once at bring-up, so every disk whose driver registered afterwards
was invisible to the partition manager for the rest of the boot. Its partitions
were never published, so no `SUBSYSTEM=="partition"` rule could match them and no
filesystem on that disk could be mounted, however correct its table.

This is the ONLY path a disk can take. The partition manager is an initfs
payload, spawned ahead of every disk driver so /boot can be mounted from a
volume, so its startup sweep finds nothing and the class subscription publishes
every partition on the system. A subscription that stopped delivering takes the
whole boot with it.

The disk here is virtio-blk rather than ATA because it negotiates a PCI device,
claims an MSI-X vector and sets up a virtqueue before publishing, so it arrives
far enough behind the ATA drives to separate the two paths in a log.

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
# accident. Nothing matches "spare", so the partition is published and left alone,
# which is exactly the property under test.
VOLUME_LABEL = "spare"
IMAGE_MIB = 64

# Files on the partition: one read before the disk driver restarts and one read
# only after each restart, so no read after a restart can be served from what
# fs-fat cached before it. Three restarts fit the supervisor's default intensity
# (5 ends in 60 s), and outnumber the backends the partition manager can lend
# its buffers to unless it forgets the ones that ended.
RESTARTS = 3
BEFORE_TEXT = b"read before the disk driver restarts"
SPARE_FILES = [("BEFORE.TXT", BEFORE_TEXT)] + [
    (f"AFTER{n}.TXT", f"read after restart {n} of the disk driver".encode())
    for n in range(1, RESTARTS + 1)
]
# Read only while the disk driver is stopped.
HELD_TEXT = b"read while the disk driver was stopped"
SPARE_FILES.append(("HELD.TXT", HELD_TEXT))
# The partition manager's word that a transfer waits for the disk's next driver.
PM_HOLD_LINE = " holds a transfer until its driver returns"
MOUNT_POINT = "/spare"

# How many `block` providers a correct boot holds with this disk attached: ATA's
# THREE drives, the one partition the partition manager republishes on each of
# the two that carry a table, the virtio disk, and its partition. The count is
# what asserts that nothing registered twice; the ids below say which ones are
# present.
#
# ATA unit 2 is the raw WFS volume. It contributes a disk and no partition: it
# holds a filesystem written straight to the device with no table for the
# partition manager to parse, which is the case a `volume` would cover and a
# `partition` cannot (architecture/37-volume-manager.md).
EXPECTED_PROVIDERS = 7

# A partition of the virtio disk, whose unit is derived from the device's PCI
# slot and so is matched by shape rather than by a literal -- the same reason
# test_virtio_blk.py does not bake the number in.
VIRTIO_PARTITION_RE = re.compile(
    rb"\[partition-manager\] partition id=block:virtio-blk:\d+p\d+"
)


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
            f"file={disk_path},format=raw,if=none,id=arrivalblk",
            "-device",
            "virtio-blk-pci,drive=arrivalblk,id=arrivalblkdev",
        ),
    )


class PartitionManagerDiskArrivalTest(unittest.TestCase):
    """The partition manager probes disks that appear after its own bring-up."""

    session: QemuSession | None = None
    disk_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-arrival-disk-")
        disk_path = os.path.join(cls.disk_dir, "arrival.img")
        with open(disk_path, "wb") as handle:
            handle.write(build_image(IMAGE_MIB, VOLUME_LABEL, SPARE_FILES))
        cls.session = QemuSession(_config_with_gpt_disk(disk_path), timeout_s=200)
        cls.session.start()
        cls.session.require_prompt(timeout_s=180)

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

        Everything below only means something if this disk registered after the
        partition manager's startup sweep -- otherwise the sweep could have found
        it and the subscription would go untested. That ordering is structural now
        that the manager boots from the initfs, so this should never fail; it is
        kept because the assertions below cannot tell which path published the
        partition, and a sweep that silently started finding disks again would
        make them pass while testing nothing.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[partition-manager] ready disks=", timeout_s=90),
            "the partition manager never finished bring-up",
        )
        ready = self.session.buf.find(b"[partition-manager] ready disks=")
        published = self.session.buf.find(
            b"[virtio-blk] published id=block:virtio-blk:"
        )
        self.assertNotEqual(
            published, -1, "the virtio-blk driver never published its disk"
        )
        self.assertGreater(
            published,
            ready,
            "virtio-blk registered BEFORE the partition manager finished its "
            "startup sweep, so the sweep could have published this partition and "
            "the class subscription goes untested -- the spawn order changed and "
            "this test needs a new lever",
        )

    def test_the_arriving_disk_gets_its_partitions_published(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(VIRTIO_PARTITION_RE, timeout_s=90),
            "the partition manager published no partition for the virtio disk. "
            "Its GPT is the same one scripts/make_gpt_image.py writes for /user, "
            "which parses on ATA, so the table is not the problem: the disk "
            "registered under the `block` class after the partition manager had "
            "already enumerated it, and nothing made it look again",
        )

    def test_probing_the_arriving_disk_stalls_no_filesystem_read(self) -> None:
        """A disk's arrival must not hold up reads from the disks already up.

        Regression: 2026-11-17-partmgr-probe-blocks-on-pm -- the partition
        manager probed an arriving disk on its own thread of control and
        registered each partition with a SYNCHRONOUS call to the process
        manager. The process manager was itself blocked on a module read from
        /boot, which is forwarded by that same partition manager, so each waited
        on the other until the process manager's 15 s fs timeout broke the
        cycle. Every boot with this disk attached lost 15 s and failed one
        driver spawn (`spawn_path fs read failed: .../serial.wap (no reply)`),
        and a peer with no timeout of its own would have hung the boot.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(VIRTIO_PARTITION_RE, timeout_s=90),
            "the virtio disk's partition was never published",
        )
        self.assertFalse(
            b"[pm] fs reply timed out" in self.session.buf,
            "a filesystem read timed out in the process manager while the "
            "partition manager probed the arriving disk: the probe blocked the "
            "partition manager, which forwards every /boot read",
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
            "ATA's three drives, a partition on each of the two that carry a "
            "table, the virtio disk and its partition. A count of 6 means the "
            "virtio disk's partition is "
            "missing; a higher count means something registered twice",
        )

    def test_z_a_mount_on_the_partition_survives_a_restart_of_its_disk_driver(
        self,
    ) -> None:
        """Runs last: it restarts the virtio-blk driver under a mounted partition.

        Regression: 2026-11-21-partmgr-loses-a-restarted-disk -- the partition
        manager ignored its disk's REMOVE and dropped the disk's return as an
        arrival it already knew, so it went on forwarding every transfer for the
        partition to the endpoint of the driver that had ended. A filesystem on a
        partition of a supervised disk lost its I/O for good on the driver's
        first restart, though the disk's content never changed.

        Regression: 2026-11-22-block-transfer-lost-across-a-driver-restart --
        a transfer forwarded to the driver when it ended, or asked for before
        its successor registered, failed with WASMOS_ERR_BLOCK_DEV_GONE,
        although the successor served the same disk a moment later.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(VIRTIO_PARTITION_RE, timeout_s=90),
            "the virtio disk's partition was never published",
        )
        match = VIRTIO_PARTITION_RE.search(self.session.buf)
        assert match is not None
        partition_id = match.group(0).split(b"id=")[1].decode()

        self._run(f"mount -t fat {MOUNT_POINT} {partition_id}", [b"mount: mounted"])
        self._run(f"cat {MOUNT_POINT}/before.txt", [BEFORE_TEXT])

        listing = self._run("svc list", [])
        units = [
            line.split()[0]
            for line in listing.decode("utf-8", errors="replace").splitlines()
            if line.startswith("virtio_blk@")
        ]
        self.assertEqual(
            len(units), 1, f"one supervised virtio-blk driver:\n{listing!r}"
        )
        unit = units[0]
        for n in range(1, RESTARTS + 1):
            log = self.session.syslog_tail(1 << 20)
            published = log.count("[virtio-blk] published id=")
            self._run(f"svc kill {unit}", [f"svc: killing {unit}".encode()])
            # Read at once: a transfer that catches the driver down is held by
            # the partition manager and resent to the successor. The
            # partition, its mount and the fs-fat behind it are the same ones:
            # nothing remounted.
            self._run(
                f"cat {MOUNT_POINT}/after{n}.txt",
                [f"read after restart {n} of the disk driver".encode()],
                timeout_s=60,
            )
            log = self.session.syslog_tail(1 << 20)
            self.assertGreater(
                log.count("[virtio-blk] published id="),
                published,
                f"the read completed, but not on a restarted driver ({n})",
            )
        # A read made while the driver is stopped -- certainly down, where a
        # read after a kill races the restart -- is held, and completes once
        # the driver is started again.
        log = self.session.syslog_tail(1 << 20)
        holds = log.count(PM_HOLD_LINE)
        self._run(f"svc stop {unit}", [f"svc: stopping {unit}".encode()])
        self.assertTrue(
            self.session.syslog_wait_count(f"[supervisor] {unit} stopped", 1, 30),
            f"the driver did not stop.\n--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        start = self.session.send_command(
            f"spawn cat {MOUNT_POINT}/held.txt", timeout_s=20
        )
        log = self.session.syslog_wait_count(PM_HOLD_LINE, holds + 1, 30)
        self.assertTrue(
            log,
            f"the read was not held.\n--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        self.assertNotIn(
            HELD_TEXT,
            bytes(self.session.buf[start:]),
            "the read completed with no driver",
        )
        self._run(f"svc start {unit}", [f"svc: starting {unit}".encode()])
        self.assertTrue(
            self.session.expect_from(start, HELD_TEXT, timeout_s=60),
            f"the held read did not complete on the started driver\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        log = self.session.syslog_tail(1 << 20)
        self.assertNotIn(f"[supervisor] {unit} failed", log)
        self.assertNotIn("[supervisor] critical unit", log)

    def _run(self, cmd: str, needles: list, timeout_s: int = 40) -> bytes:
        """Run `cmd`, require each needle after its echo and the prompt back,
        and return what it printed."""
        assert self.session is not None
        start = self.session.send_command(cmd, timeout_s=timeout_s)
        for needle in needles:
            self.assertTrue(
                self.session.expect_from(start, needle, timeout_s=timeout_s),
                f"{cmd!r} did not produce {needle!r}\n"
                f"--- tail ---\n{self.session.tail()}\n",
            )
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=timeout_s),
            f"{cmd!r} never returned to the prompt\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        return bytes(self.session.buf[start:])
