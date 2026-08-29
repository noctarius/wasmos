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
# disk. It is the volume `/boot` will be selected by once mount policy moves to
# volumes, and `tests/unit/fixtures_disk_images.zig` carries the same bytes.
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
                    b"[volume-manager] " + device + b" holds a " + scheme, timeout_s=60
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
        # Three: the two FAT partitions and the raw WFS disk. A higher count means
        # a partitioned disk was published as well; a lower one means a recogniser
        # or a prefix read failed silently.
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[volume-manager] ready volumes=3", timeout_s=90),
            "the volume manager did not publish exactly three volumes",
        )
