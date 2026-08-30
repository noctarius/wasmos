#!/usr/bin/env python3
"""The volume manager publishes what can be MOUNTED, which the block layer cannot say.

`block` answers "what storage exists". That does not settle what holds a
filesystem, in either direction: a partition-table entry may hold none, and a
disk with no table may hold one. This boot has one instance of each, so the two
questions have visibly different answers:

    block:ata:0    MBR disk        -> no volume; its partition is the volume
    block:ata:0p1  FAT, "QEMU VVFAT" -> volume  (the ESP)
    block:ata:1    GPT disk        -> no volume; its partition is the volume
    block:ata:1p1  FAT, "USER"     -> volume  (/user)
    block:ata:2    raw WFS, no table -> volume (the case `partition` cannot reach)

The last row is why this layer exists. `SUBSYSTEM=="partition"` got mount policy
as far as matching a partition's label, and a formatted disk with no table at all
is outside what that can name. See architecture/37-volume-manager.md.
"""

import unittest

from scripts.qemu_test_framework import QemuSession, default_config

# Written by QEMU's vvfat into the ESP's FAT boot sector, padded to 11 bytes on
# disk. `tests/unit/fixtures_disk_images.zig` carries the same bytes. It is NOT
# what selects /boot -- that is ATTR{boot}, since this label is QEMU's and an ESP
# on other firmware would carry another.
ESP_LABEL = "QEMU VVFAT"
# The FILESYSTEM label of the /user volume, which is not the same string as its
# GPT PARTITION label: `make_gpt_image.py` is told `user` and writes the FAT
# volume label upper-cased, the way 8.3 short names are. A volume rule matches
# the filesystem's label, so it is this one that matters.
USER_LABEL = "USER"


class VolumeManagerTest(unittest.TestCase):
    """Volumes are published for what holds a filesystem, and only for that."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.session = QemuSession(default_config(), timeout_s=200)
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

    def test_the_service_comes_up(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[volume-manager] ready volumes=", timeout_s=90),
            "the volume manager never finished bring-up -- the boot rule did not "
            "spawn it, or it failed before reporting",
        )

    def test_a_partition_holding_fat_becomes_a_volume(self) -> None:
        """The ESP: an MBR partition, recognised by its filesystem's own label.

        The label is the point. An MBR carries no partition label and no PARTUUID,
        so at the block layer this volume's only identity is its position --
        `ata:0p1`. The FAT boot sector inside it does carry one, which is exactly
        the identity the block layer cannot see and this one can.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(
                f"[volume-manager] volume id=volume:block:ata:0p1 fstype=fat "
                f"label={ESP_LABEL}".encode(),
                timeout_s=90,
            ),
            "the ESP partition published no FAT volume with its label -- the "
            "recogniser did not run on it, or read the label at the wrong offset",
        )

    def test_a_gpt_partition_holding_fat_becomes_a_volume(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(
                f"[volume-manager] volume id=volume:block:ata:1p1 fstype=fat "
                f"label={USER_LABEL}".encode(),
                timeout_s=90,
            ),
            "the /user partition published no FAT volume",
        )

    def test_a_raw_disk_holding_wfs_becomes_a_volume(self) -> None:
        """The case the block layer cannot express, and the reason for this layer.

        ATA unit 2 is a WFS filesystem written straight to the device with no
        partition table. The partition manager publishes nothing for it -- there
        is no table to parse -- so no `SUBSYSTEM=="partition"` rule can ever name
        it, and a `SUBSYSTEM=="block"` rule can only name it by disk and unit.
        As a volume it is named by what it IS.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(
                b"[volume-manager] volume id=volume:block:ata:2 fstype=wfs",
                timeout_s=90,
            ),
            "the raw WFS disk published no volume. The partition manager reports "
            "no table on it, so this is the whole-disk case: either the prefix "
            "read did not reach byte 1024 where the superblock starts, or the "
            "recogniser rejected its magic or version",
        )

    def test_a_partitioned_disk_publishes_no_volume(self) -> None:
        """Suppression, asserted from both sides.

        A disk carrying a table holds partitions, not a filesystem. Publishing a
        volume for it as well would make the disk and the partition on it two
        mountable things covering the same sectors -- /boot would appear twice.
        """
        assert self.session is not None
        for device, scheme in ((b"block:ata:0", b"mbr"), (b"block:ata:1", b"gpt")):
            self.assertTrue(
                self.session.expect(
                    b"[volume-manager] device probed id="
                    + device
                    + b" holds a "
                    + scheme,
                    timeout_s=60,
                ),
                f"{device!r} was not reported as carrying a {scheme!r} table",
            )
            # The trailing space matters: without it this also matches
            # `volume:block:ata:0p1`, which is the volume that SHOULD exist.
            self.assertNotIn(
                b"volume id=volume:" + device + b" ",
                self.session.buf,
                f"a volume was published for {device!r}, which carries a partition "
                "table -- the disk and its partition are now two mountable things "
                "over the same sectors",
            )

    def test_the_count_matches_what_was_published(self) -> None:
        """Three: the two FAT partitions and the raw WFS disk.

        A higher count means a partitioned disk was published as well; a lower one
        means a recogniser or a prefix read failed silently.

        The count comes from a per-device line, not from `ready volumes=`. This
        service is an initfs payload now, so it is up ahead of every disk driver
        and its startup sweep always sees zero -- each device on the system is
        probed as it registers, which is the ordinary path and not an exception.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[volume-manager] device probed", timeout_s=90),
            "no device was probed after bring-up, so the class subscription is not "
            "delivering -- with an empty startup sweep that means no volumes at all",
        )
        self.assertTrue(
            self.session.expect(b" volumes=3", timeout_s=90),
            "the volume manager did not publish exactly three volumes",
        )

    def test_boot_mounts_from_the_volume_the_firmware_named(self) -> None:
        """/boot names no disk either, and it is the case with no other answer.

        Nothing on an ESP can supply it: its filesystem is ordinary FAT, its label
        is firmware-specific, and an MBR gives it no partition label and no
        PARTUUID. The firmware is the only thing that knows which volume this
        system came from, so the bootloader reads the HARDDRIVE node of its own
        device path and the kernel publishes the LBA range.

        Asserted on the PARTITION (`ata:0p1`), which is the point: /boot mounts
        the volume, so fs_fat is handed a device whose LBA 0 is a boot sector and
        has no table to read.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[kernel] boot.partition ", timeout_s=90),
            "the kernel published no boot.partition -- the bootloader found no "
            "HARDDRIVE node in its device path, or boot_info did not carry it",
        )
        self.assertTrue(
            self.session.expect(
                b"[device-manager] volume rule queued spawn mount=/boot "
                b"id=volume:block:ata:0p1 on block:ata:0p1",
                timeout_s=90,
            ),
            "/boot did not bind to the ESP volume -- the boot partition reached "
            "the device manager but matched no volume, or matched the wrong one",
        )

    def test_no_mount_rule_names_a_disk(self) -> None:
        """The layer's whole point, as a property of the boot rather than a rule.

        Every mount is now selected by what its volume IS, or by which partition
        holds it. A `block rule queued spawn` line would mean a filesystem was
        placed by naming a controller and a unit -- which is what this set out to
        retire, and which /boot was the last holdout of.
        """
        assert self.session is not None
        self.assertNotIn(
            b"block rule queued spawn",
            self.session.buf,
            "a filesystem was mounted by a rule naming a disk and a unit",
        )

    def test_waiting_for_a_service_does_not_spin(self) -> None:
        """Regression: 2026-08-30-broker-selftest-polls-for-font-service.

        The kernel's broker self-test returned PROCESS_RUN_YIELDED until
        `font-service` was ready, re-reading the process table on every dispatch
        -- roughly 10^6 of them across a boot. font-service starts from /boot, so
        the wait lasted the whole of storage bring-up, and the polling was itself
        load on the bring-up it was waiting for.

        It blocks on the readiness broadcast now. Asserted through the self-test
        COMPLETING, because that is the observable end of the wait: a boot that
        reaches this line woke from the block and finished. A stall dump on a
        failing boot shows the difference directly -- `st=blocked` with a handful
        of dispatches, where it used to show `st=running` with ~10^6.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[test] broker spawn delegation ok", timeout_s=120),
            "the broker self-test never completed -- it is waiting on a service "
            "that never came up, or its wake was lost",
        )

    def test_the_inventory_receives_what_the_class_advertises(self) -> None:
        """Published to the device manager, not only registered under the class.

        These are two separate paths and only one of them feeds rules. A publish
        needs the buffer BORROWED to the device manager; without that the read is
        refused while class registration still succeeds, so the volumes look
        present -- and every SUBSYSTEM=="volume" rule matches nothing. The
        partition manager shipped with exactly that fault.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(
                b"[device-manager] volume add id=volume:block:ata:2 fstype=",
                timeout_s=90,
            ),
            "the WFS volume never reached the device-manager inventory -- the "
            "publish buffer was not lent, or the descriptor was refused at the read",
        )

    def test_a_volume_rule_mounts_without_naming_a_disk(self) -> None:
        """The point of the layer, end to end.

        /wfs is matched by `SUBSYSTEM=="volume"` on fstype and uuid -- no disk, no
        unit, no backend, no partition. That case is unreachable any other way:
        the WFS image has no partition table, so the partition manager publishes
        nothing for it and no `SUBSYSTEM=="partition"` rule can name it, leaving
        only a rule naming a disk and a unit.

        The spawn is asserted on the BACKING device, because that is what a
        filesystem driver mounts. The volume selected it; the block device is
        what gets opened.

        mount= is part of the expectation: with two WFS volumes on the system it
        is the pairing of path to volume that this layer decides, and a message
        naming only the volume cannot show which path took it.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(
                b"[device-manager] volume rule queued spawn mount=/wfs "
                b"id=volume:block:ata:2 on block:ata:2",
                timeout_s=90,
            ),
            "no volume rule fired for the WFS volume -- it was published but "
            "matched nothing, or its backing device could not be resolved from "
            "the block inventory",
        )
        self.assertTrue(
            self.session.expect(b"[fs-wfs] mounted", timeout_s=90),
            "fs_wfs was spawned by the volume rule but did not mount",
        )
        # Registration is the half that fails when the driver is handed the wrong
        # identity: fs.backend packs (kind, unit), so a volume rule that passed on
        # the volume's own zeroed unit rather than the backing device's collided
        # with another backend and the mount was never usable.
        self.assertNotIn(
            b"[fs-wfs] fs.backend register failed",
            self.session.buf,
            "fs_wfs mounted but could not register its backend -- the volume rule "
            "gave it an identity that is not the backing device's",
        )
