#!/usr/bin/env python3
"""A FAT volume on a whole virtio-blk disk survives a restart of the disk driver.

The disk carries a FAT16 volume written straight to the device, with no
partition table, so fs-fat talks to the virtio-blk driver itself rather than to
the partition manager. virtio-blk is a supervised unit (`restart=on-failure`):
killing it brings a new instance up under the same `block` class instance, and
the mount above it is expected to follow, not to be remounted.

The image is built here from `scripts/make_gpt_image.py`'s FAT writer. The
label is one no rule mounts, so nothing but this test touches the volume.
"""

import os
import re
import shutil
import sys
import tempfile
import time
import unittest
from dataclasses import replace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.make_gpt_image import build_fat16
from scripts.qemu_test_framework import QemuSession, default_config

VOLUME_LABEL = "spare"
DISK_SECTORS = 32768  # 16 MiB
MOUNT_POINT = "/vfat"

# Three restarts fit the supervisor's default intensity (5 ends in 60 s). Each
# is followed by a read of a file not read before, so no read after a restart
# can be served from what fs-fat cached before it.
RESTARTS = 3
BEFORE_TEXT = b"read before the disk driver restarts"
FILES = [("BEFORE.TXT", BEFORE_TEXT)] + [
    (f"AFTER{n}.TXT", f"read after restart {n} of the disk driver".encode())
    for n in range(1, RESTARTS + 1)
]

DISK_RE = re.compile(rb"\[virtio-blk\] published id=(block:virtio-blk:\d+)")


def _config_with_fat_disk(disk_path: str):
    """The default device set plus the FAT image as a virtio-blk disk.

    if=none keeps QEMU from attaching the drive to the IDE bus, where it would
    become an ATA unit and the driver under test would not be virtio-blk.
    """
    return replace(
        default_config(),
        extra_args=(
            "-drive",
            f"file={disk_path},format=raw,if=none,id=fatblk",
            "-device",
            "virtio-blk-pci,drive=fatblk,id=fatblkdev",
        ),
    )


class FatOverVirtioBlkRestartTest(unittest.TestCase):
    session: QemuSession | None = None
    disk_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.disk_dir = tempfile.mkdtemp(prefix="wasmos-fat-vblk-")
        disk_path = os.path.join(cls.disk_dir, "fat.img")
        with open(disk_path, "wb") as handle:
            handle.write(build_fat16(DISK_SECTORS, VOLUME_LABEL, FILES))
        cls.session = QemuSession(_config_with_fat_disk(disk_path), timeout_s=200)
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

    def test_a_whole_disk_mount_survives_a_restart_of_its_disk_driver(self) -> None:
        """Regression: 2026-11-21-fat-hangs-on-a-dead-block-driver -- fs-fat on
        a whole disk looked its disk's driver up once, at mount, and kept
        sending to that endpoint after the driver ended. Every read of the
        mount failed from the first restart on, though the disk's content
        never changed, and a read the driver held when it ended was never
        answered at all.
        """
        assert self.session is not None
        self.assertTrue(
            self.session.expect(DISK_RE, timeout_s=90),
            "the virtio-blk driver never published its disk",
        )
        match = DISK_RE.search(self.session.buf)
        assert match is not None
        disk_id = match.group(1).decode()

        self._run(f"mount -t fat {MOUNT_POINT} {disk_id}", [b"mount: mounted"])
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
            deadline = time.time() + 60
            while time.time() < deadline:
                log = self.session.syslog_tail(1 << 20)
                if log.count("[virtio-blk] published id=") > published:
                    break
                time.sleep(0.25)
            self.assertGreater(
                log.count("[virtio-blk] published id="),
                published,
                f"the supervisor never brought the virtio-blk driver back ({n})",
            )
            # The same mount and the same fs-fat: nothing remounted, and the
            # read reaches the new driver.
            self._run(
                f"cat {MOUNT_POINT}/after{n}.txt",
                [f"read after restart {n} of the disk driver".encode()],
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


if __name__ == "__main__":
    unittest.main()
