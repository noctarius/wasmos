import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class DeviceManagerIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _cmd_expect(self, cmd: str, needle: bytes, timeout_s: int = 10) -> None:
        mark = self.session.mark()
        self.session.send(cmd)
        ok = self.session.expect_from(mark, needle, timeout_s=timeout_s)
        if not ok:
            self.fail(
                f"Expected output not found for '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )
        ok = self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
        if not ok:
            self.fail(
                f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )

    def test_device_manager_running(self):
        self._cmd_expect("ps", b"device-manager")

    def test_partition_rule_mounts_a_volume(self):
        """Regression: 2026-08-28-partition-publish-unreadable.

        The partition manager wrote each partition descriptor into a transfer
        buffer it never lent to the device manager, so every publish was refused
        at the read and no partition ever entered the registry. Partitions
        registered under the `block` CLASS regardless, so `blkinfo` listed them
        and the failure looked cosmetic -- but rules match against the registry,
        so SUBSYSTEM=="partition" matched nothing at all and the entire partition
        half of the rule language was dead.

        /user sits on exactly such a partition. It is mounted by a VOLUME rule,
        and a volume exists only where a partition was published, so its presence
        in the mount table is still the observable proof that a partition reached
        the registry -- one layer removed rather than direct.
        """
        self._cmd_expect("mount", b"/user")

    def test_user_volume_is_mounted_by_its_own_identity(self):
        """/user names itself: no disk, no unit, no position in the table.

        The rule that mounts it is `SUBSYSTEM=="volume", ATTR{uuid}=="49d6938d"`,
        so it selects the FAT volume by the serial in its own boot sector and
        would follow that volume to another disk, another controller, or another
        position in the table. A positional rule would still pass if the volume
        were a different one, which is why /user is named this way.

        The serial is deterministic rather than arbitrary: scripts/make_gpt_image.py
        writes `zlib.crc32(label)` into the FAT extended boot record, so
        crc32(b"user") == 0x8d93d649 and the rule spells the on-disk byte order,
        49d6938d. Note that a reformat regenerates it, where the GPT partition
        name would have survived one.

        Listing the volume's contents rather than only its mount point is what
        makes this an assertion about a FILESYSTEM. A BPB that merely parses
        proves the header, while a file read back through a cluster chain proves
        the FAT and the root directory too. The name comes back upper-case
        because FAT stores 8.3 names that way and the builder writes no
        long-name entries.
        """
        self._cmd_expect("cd /user", b"/user wamos> ")
        self._cmd_expect("ls", b"README.TXT")

    def test_mount_reports_the_filesystem_actually_serving_each_mount(self):
        """Regression: 2026-08-30-fsmgr-backend-identity.

        `mount` derived its filesystem label from fs_backend_t.kind, which is
        FSMGR_BACKEND_BLOCK for every block-backed backend and says nothing about
        which filesystem is mounted. Both WFS volumes were therefore reported as
        fs-fat, and the backend table carried no filesystem identity at all --
        the same gap that let the root filesystem be chosen by registration
        order.

        Asserting both directions matters: a label fixed by inverting the
        hardcoded string would report FAT volumes as WFS and still pass a
        one-sided check. /wfs is the WFS volume on ATA, /boot and /user are the
        FAT volumes. The virtio-blk /vwfs mount is asserted in
        test_wfs_virtio_blk, which is the suite that attaches that disk.
        """
        self._cmd_expect("mount", b"/boot -> fs-fat")
        self._cmd_expect("mount", b"/user -> fs-fat")
        self._cmd_expect("mount", b"/wfs -> fs-wfs")

    def test_initfs_mount_name_follows_from_its_filesystem_type(self):
        """/init is named from FS_TYPE_INITFS, not from a per-backend branch.

        A pseudo-filesystem is spawned by no rule and sits on no volume, so
        nothing tells fs-manager where to mount it. Its name therefore comes from
        the same per-type table that names the filesystem itself
        (`fsmgr_default_mount_name`), which is what keeps "the initfs one is
        called init" from being a case that a devfs and a sysfs would each extend.

        Asserting the path and not only the mount line: if the type carried no
        default the mount silently becomes /fs, and every /init path stops
        resolving while `mount` still lists a backend.
        """
        self._cmd_expect("mount", b"/init -> fs-init")
        self._cmd_expect("cd /init", b"/init wamos> ")
        self._cmd_expect("cd /", b"/ wamos> ")


if __name__ == "__main__":
    unittest.main()
