"""A restart of the ata driver, which carries /boot, is invisible to the
filesystems on its disks.

ata is a supervised unit (ENV{RESTART} on its initfs rule) and is respawned
from the initfs, which ata does not serve. Its disks carry /boot and /user
(partitions, followed by the partition manager) and /wfs (a whole disk,
followed by fs-wfs). Killing it must leave every mount reading the same bytes
from the successor.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.qemu_test_framework import QemuSession, default_config, requires_arch

RESTARTS = 2
BOOT_FILE = "/boot/system/devmgr/rules/default.rules"
BOOT_TEXT = b'SUBSYSTEM=="boot", ENV{MOUNT}="/home/user"'
WFS_FILE = "/wfs/hello.txt"
WFS_TEXT = b"hello from the WFS volume"


@requires_arch(
    "x86_64",
    reason='x86_64 storage: ATA disks and the IDE WFS volume (aarch64 boots from virtio-blk; topology-aware storage tests are in docs/TASKS.md "arm64 Port")',
)
class AtaRestartTest(unittest.TestCase):
    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.session = QemuSession(default_config(), timeout_s=200)
        cls.session.start()
        cls.session.require_prompt(timeout_s=180)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_the_boot_disk_driver_restarts_under_its_mounts(self) -> None:
        """Regression: 2026-11-22-restarted-ata-reads-stale-sectors -- the
        kernel turns bus mastering off on ata's PCI function when ata ends, and
        only an MSI bind through pci-bus turned it back on. ata takes legacy
        interrupts and binds no MSI, so its successor programmed bus-master DMA
        the device could not perform: every read completed without moving a
        byte and returned whatever the buffer held. /boot lookups failed
        ("no such command found: cat"), and the volume manager published the
        boot disk, which carries an MBR, as a FAT volume read from a stale
        sector.
        """
        assert self.session is not None
        listing = self._run("svc list", [])
        units = [
            line.split()[0]
            for line in listing.decode("utf-8", errors="replace").splitlines()
            if line.startswith("ata@")
        ]
        self.assertEqual(len(units), 1, f"one supervised ata driver:\n{listing!r}")
        unit = units[0]
        self._run(f"cat {BOOT_FILE}", [BOOT_TEXT])
        self._run(f"cat {WFS_FILE}", [WFS_TEXT])

        for n in range(1, RESTARTS + 1):
            log = self.session.syslog_tail(1 << 20)
            started = log.count("[ata] block class id=block:ata:0 ")
            self._run(f"svc kill {unit}", [f"svc: killing {unit}".encode()])
            # Read at once: the transfer is held while ata is down and served
            # by the successor, which must move real bytes.
            self._run(f"cat {BOOT_FILE}", [BOOT_TEXT], timeout_s=60)
            self._run(f"cat {WFS_FILE}", [WFS_TEXT], timeout_s=60)
            log = self.session.syslog_tail(1 << 20)
            self.assertGreater(
                log.count("[ata] block class id=block:ata:0 "),
                started,
                f"the reads completed, but not on a restarted ata ({n})",
            )
        log = self.session.syslog_tail(1 << 20)
        self.assertNotIn(f"[supervisor] {unit} failed", log)
        self.assertNotIn("[supervisor] critical unit", log)
        # A disk read through a successor that moved no bytes looks like a
        # different filesystem: the boot disk carries an MBR, never a FAT.
        self.assertNotIn("volume id=volume:block:ata:0 fstype=fat", log)

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
