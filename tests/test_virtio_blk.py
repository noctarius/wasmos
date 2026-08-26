#!/usr/bin/env python3
"""The Zig virtio-blk driver brings a virtio disk up and serves block reads.

Boots with a virtio-blk disk attached whose first sector carries a known
signature, then drives the whole path from the CLI: the device-manager rule
spawns the driver, the driver negotiates the device and registers under the
"block" class, and `blkinfo` moves sectors through BLOCK_IPC_READ_REQ and
BLOCK_IPC_WRITE_REQ. Data coming back is what proves the virtqueue data path --
a three-descriptor chain landing in the caller's block buffer -- and not merely
that the device was probed. Both directions are covered, because they differ in
exactly one descriptor flag and a driver can get one right while the other moves
nothing.
"""

import os
import shutil
import tempfile
import unittest
from dataclasses import replace

from scripts.qemu_test_framework import QemuSession, default_config

# Written at the start of LBA 0 of the test disk. Sixteen bytes, because that is
# what blkinfo previews, and recognisable in a serial log that interleaves other
# services' output.
DISK_SIGNATURE = b"WASMOS-BLK-TEST1"
# Disk size in 512-byte sectors, which is what BLOCK_IPC_IDENTIFY_RESP reports.
DISK_SECTORS = 2048
# Leading bytes of the pattern `blkinfo --write` lays down, so the read-back
# assertion names what it is looking for rather than a bare hex string.
WRITE_TAG = b"WASMOS-BLKWRITE"


class VirtioBlkTest(unittest.TestCase):
    """A virtio-blk disk is discovered, identified, and read end to end."""

    session: QemuSession | None = None
    disk_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-vblk-")
        disk_path = os.path.join(cls.disk_dir, "vblk.img")
        with open(disk_path, "wb") as handle:
            handle.write(DISK_SIGNATURE)
            handle.write(b"\x00" * (DISK_SECTORS * 512 - len(DISK_SIGNATURE)))

        # if=none keeps QEMU from attaching the drive to the default IDE bus,
        # where it would become a second ATA unit instead of a virtio device.
        cfg = replace(
            default_config(),
            extra_args=(
                "-drive",
                f"file={disk_path},format=raw,if=none,id=vblk0",
                "-device",
                "virtio-blk-pci,drive=vblk0,id=vblk",
            ),
        )
        cls.session = QemuSession(cfg, timeout_s=180)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=150):
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

    def test_driver_brings_the_device_up(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[virtio-blk] ready qsize=", timeout_s=60),
            "virtio-blk never reported a live virtqueue — the device-manager rule "
            "did not spawn the driver, or feature negotiation / queue setup failed",
        )

    def test_write_then_read_round_trips_a_sector(self) -> None:
        assert self.session is not None
        # LBA 1, not 0: the signature at LBA 0 is what the read test asserts on,
        # and a write there would make the two tests order-dependent.
        self.session.send("blkinfo --write 1")
        self.assertTrue(
            self.session.expect(b"[blkinfo] instance=0 lba=1 write ok", timeout_s=60),
            "the device refused BLOCK_IPC_WRITE_REQ — the request chain's data "
            "descriptor is device-writable in the OUT direction, or the device "
            "reported a non-OK status",
        )
        self.assertTrue(
            self.session.expect(WRITE_TAG.hex().upper().encode(), timeout_s=30),
            "the sector written did not read back — the write reached the device "
            "but not the sector it named, or the read raced the completion",
        )

    def test_block_class_read_returns_the_disk_signature(self) -> None:
        assert self.session is not None
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(b"[blkinfo] providers=1", timeout_s=60),
            'no provider registered under the "block" class — the driver did not '
            "reach service registration",
        )
        self.assertTrue(
            self.session.expect(f"sectors={DISK_SECTORS}".encode(), timeout_s=30),
            "IDENTIFY did not report the attached disk's size — the driver read the "
            "wrong device-configuration offset, or the capacity halves are swapped",
        )
        self.assertTrue(
            self.session.expect(DISK_SIGNATURE.hex().upper().encode(), timeout_s=60),
            "sector 0 did not come back with the signature written to the disk — the "
            "virtqueue request chain did not deliver data into the caller's block buffer",
        )


if __name__ == "__main__":
    unittest.main()
