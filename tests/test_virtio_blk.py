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

import re

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
# `BLOCK_BACKEND_VIRTIO_BLK` from abi/constants.yaml; the high byte of a `block`
# class instance.
VIRTIO_BLK_BACKEND = 2


def _virtio_instance(session) -> int:
    """The virtio disk's class instance, as the guest reports it.

    Not hardcoded, because the instance is `(backend << 8) | unit` and the unit
    is derived from the device's PCI slot -- so it is stable for a given machine
    configuration but would change if QEMU laid the bus out differently. A test
    that baked the number in would be asserting QEMU's device ordering rather
    than this driver's behaviour.
    """
    match = re.search(
        rb"\[blkinfo\] instance=(\d+) driver=virtio-blk unit=(\d+)", session.buf
    )
    assert match is not None, "blkinfo reported no virtio-blk disk"
    instance = int(match.group(1))
    unit = int(match.group(2))
    assert (
        instance == (VIRTIO_BLK_BACKEND << 8) | unit
    ), f"instance {instance} does not decode to (virtio-blk, {unit})"
    return instance


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
        # Ask the guest which disk the virtio one is rather than assuming: the
        # instance encodes a unit derived from the device's PCI slot.
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(b"driver=virtio-blk", timeout_s=60),
            "blkinfo did not enumerate the virtio disk",
        )
        instance = _virtio_instance(self.session)
        # LBA 1, not 0: the signature at LBA 0 is what the read test asserts on,
        # and a write there would make the two tests order-dependent.
        # --write names the disk, and refuses to run without one, so it can never
        # scribble on the ATA boot disk it now also enumerates.
        self.session.send(f"blkinfo --write {instance} 1")
        self.assertTrue(
            self.session.expect(
                f"[blkinfo] instance={instance} lba=1 write ok".encode(), timeout_s=60
            ),
            "the device refused BLOCK_IPC_WRITE_REQ — the request chain's data "
            "descriptor is device-writable in the OUT direction, or the device "
            "reported a non-OK status",
        )
        self.assertTrue(
            self.session.expect(WRITE_TAG.hex().upper().encode(), timeout_s=30),
            "the sector written did not read back — the write reached the device "
            "but not the sector it named, or the read raced the completion",
        )

    def test_every_backend_appears_as_its_own_disk(self) -> None:
        """Both backends enumerate under the `block` class, as distinct disks.

        The instance encodes (backend << 8) | unit. ATA's drives are 256 and 257
        because its units are drive numbers; the virtio disk's unit comes from
        its PCI slot, so its instance is checked by decoding rather than by a
        literal. Both are derived from what the disk IS rather than handed out
        on registration, which is what makes them the same every boot however
        the drivers raced to probe.
        """
        assert self.session is not None
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(b"[blkinfo] providers=3", timeout_s=60),
            "the block class did not hold all three disks (ATA's two drives plus "
            "the virtio disk) — a backend did not register, or two collided on "
            "one instance",
        )
        for marker in (
            b"instance=256 driver=ata unit=0",
            b"instance=257 driver=ata unit=1",
        ):
            self.assertTrue(
                self.session.expect(marker, timeout_s=30),
                f"{marker!r} missing — an instance did not decode back to the "
                "(backend, unit) pair a device-manager rule names",
            )
        # Asserts the same property for the virtio disk without pinning QEMU's
        # PCI layout: the reported instance must decode to (virtio-blk, unit).
        _virtio_instance(self.session)

    def test_a_rule_is_queued_only_for_its_own_backend(self) -> None:
        """A block rule naming one backend is not queued for a disk on another.

        The shipped rules name DRIVER=="ata", and a virtio disk is attached and
        publishes its own unit 0 — the same unit number the /boot rule asks for.
        If the matcher compared units alone, that rule would be queued for the
        virtio disk and a filesystem would try to mount it.

        This is a regression guard: the device manager had TWO copies of the
        match predicate, and the one on the live publish path never compared the
        backend at all. It was masked only because ATA publishes first, so /boot
        was already mounted by the time the virtio disk arrived.
        """
        assert self.session is not None
        for marker in (
            b"block_fs rule queued spawn driver=ata unit=0",
            b"block_fs rule queued spawn driver=ata unit=1",
        ):
            self.assertTrue(
                self.session.expect(marker, timeout_s=60),
                f"{marker!r} missing — the ATA disks did not match their own rules",
            )
        # No rule names virtio-blk, so none may be queued for it. Asserted as an
        # absence over the accumulated buffer: by now both ATA disks have matched
        # and the virtio disk has published.
        self.assertNotIn(
            b"block_fs rule queued spawn driver=virtio-blk",
            self.session.buf,
            "an ata rule was queued for the virtio disk — the matcher compared "
            "units without comparing backends",
        )

    def test_devmgr_inventory_separates_the_backends(self) -> None:
        """The virtio disk gets its own inventory record, not ATA's.

        The device manager used to key block records on a bare unit and spell
        every one of them `ata<unit>` at the ATA controller's PCI address, so a
        virtio disk publishing unit 0 would have overwritten the ATA boot disk's
        record and inherited its identity.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(b"block add id=block:virtio-blk:", timeout_s=60),
            "the virtio disk is absent from the device-manager inventory, so no "
            "block rule can ever match it",
        )
        self.assertTrue(
            self.session.expect(b"block add id=block:pci:", timeout_s=30),
            "the ATA disks lost their PCI-addressed identity",
        )

    def test_a_mounted_ata_disk_is_still_identifiable(self) -> None:
        """Discovery must not require claiming the disk.

        ATA binds a client endpoint to one unit exclusively so two filesystems
        cannot write one drive. That guard used to cover IDENTIFY too, which made
        a mounted disk unqueryable — the opposite of what discovering it by class
        is for. A transfer is still refused, and now says why.
        """
        assert self.session is not None
        # Each case drives blkinfo itself rather than relying on another having
        # run it: a failed expect() force-stops the VM, so a case that depends on
        # a neighbour's output turns one real failure into a cascade of fake ones.
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(
                b"instance=256 driver=ata unit=0 sectors=", timeout_s=60
            ),
            "the mounted ATA boot disk could not be identified",
        )
        self.assertTrue(
            self.session.expect(b"read failed: block_dev.UNIT_CLAIMED", timeout_s=30),
            "a transfer on a claimed unit was not refused with a named reason",
        )

    def test_block_class_read_returns_the_disk_signature(self) -> None:
        assert self.session is not None
        self.session.send("blkinfo")
        self.assertTrue(
            self.session.expect(b"[blkinfo] providers=3", timeout_s=60),
            'the "block" class did not hold all three disks — a backend did not '
            "reach service registration, or two collided on one instance",
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
