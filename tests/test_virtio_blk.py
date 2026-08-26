#!/usr/bin/env python3
"""The Zig virtio-blk driver brings a virtio disk up and serves block transfers.

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


def _make_disk(disk_dir: str) -> str:
    """Write the test disk: the signature at LBA 0, zeroes to DISK_SECTORS."""
    disk_path = os.path.join(disk_dir, "vblk.img")
    with open(disk_path, "wb") as handle:
        handle.write(DISK_SIGNATURE)
        handle.write(b"\x00" * (DISK_SECTORS * 512 - len(DISK_SIGNATURE)))
    return disk_path


def _config_with_disk(disk_path: str, device_opts: str = ""):
    """A default config with the test disk attached as a virtio-blk device.

    if=none keeps QEMU from attaching the drive to the default IDE bus, where it
    would become a second ATA unit instead of a virtio device.
    """
    return replace(
        default_config(),
        extra_args=(
            "-drive",
            f"file={disk_path},format=raw,if=none,id=vblk0",
            "-device",
            "virtio-blk-pci,drive=vblk0,id=vblk" + device_opts,
        ),
    )


class VirtioBlkTest(unittest.TestCase):
    """A virtio-blk disk is discovered, identified, read and written end to end."""

    session: QemuSession | None = None
    disk_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-vblk-")
        cls.session = QemuSession(
            _config_with_disk(_make_disk(cls.disk_dir)), timeout_s=180
        )
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

    def test_completion_interrupt_is_message_signalled(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[virtio-blk] msix enabled vector=", timeout_s=60),
            "the completion interrupt fell back to INTx on a device that offers "
            "MSI-X — pci-bus did not answer, or the vector bind was refused",
        )
        # Asserted as an absence rather than a log line, because the two paths
        # are mutually exclusive and bring-up is complete by now: routing the
        # shared line under MSI-X would subscribe this driver to every other
        # device on it, which is the thing MSI-X exists to avoid.
        self.assertNotIn(
            b"[virtio-blk] irq routed",
            self.session.buf,
            "the driver bound an MSI-X vector AND routed its INTx line",
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


class VirtioBlkIntxFallbackTest(unittest.TestCase):
    """With no MSI-X vectors on offer, the driver falls back to a routed INTx line.

    Worth its own boot: the fallback is the only INTx consumer left in the
    system, and enabling MSI-X shifts the device-configuration registers from
    0x14 to 0x18. A driver that hardcoded either offset passes one of these two
    classes and fails the other, which is the point of running both.
    """

    session: QemuSession | None = None
    disk_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-vblk-intx-")
        # vectors=0 makes the device advertise no MSI-X table, so pci-bus reports
        # no MSI-X kind and the driver takes its INTx path.
        cfg = _config_with_disk(_make_disk(cls.disk_dir), device_opts=",vectors=0")
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

    def test_falls_back_to_a_routed_intx_line(self) -> None:
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"[virtio-blk] msix unavailable", timeout_s=60),
            "the driver claimed MSI-X on a device advertising no vectors",
        )
        self.assertTrue(
            self.session.expect(b"[virtio-blk] irq routed line=", timeout_s=30),
            "the driver neither bound a vector nor routed its line, so completions "
            "rest entirely on the timed safety net",
        )

    def test_reads_still_work_on_the_intx_path(self) -> None:
        assert self.session is not None
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(f"sectors={DISK_SECTORS}".encode(), timeout_s=60),
            "IDENTIFY reported the wrong size on the INTx path — the "
            "device-configuration base is only correct for the MSI-X layout",
        )
        self.assertTrue(
            self.session.expect(DISK_SIGNATURE.hex().upper().encode(), timeout_s=60),
            "sector 0 did not come back on the INTx path — completions are not "
            "reaching the driver through the routed line",
        )


if __name__ == "__main__":
    unittest.main()
