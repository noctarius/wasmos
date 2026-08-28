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

        /user is mounted by exactly such a rule, which makes its presence in the
        mount table the observable proof that a partition reached the registry,
        matched a rule, and spawned a filesystem on the window the table named.
        """
        self._cmd_expect("mount", b"/user")

    def test_user_volume_is_mounted_by_its_gpt_label(self):
        """/user names itself: no disk, no unit, no position in the table.

        The rule that mounts it is `SUBSYSTEM=="partition",
        ATTR{partlabel}=="user"`, so nothing here would work if the GPT were not
        read end to end -- the header and entry-array CRCs verified, the UTF-16
        label decoded, the partition published with it, and the rule matched on
        it. A positional rule would still pass if the label were wrong or absent,
        which is why /user is named this way and not by ATTR{name}.

        Listing the volume's contents rather than only its mount point is what
        makes this an assertion about a FILESYSTEM. The image is built by
        scripts/make_gpt_image.py, which writes the FAT16 volume itself; a BPB
        that merely parses proves the header, while a file read back through a
        cluster chain proves the FAT and the root directory too. The name comes
        back upper-case because FAT stores 8.3 names that way and the builder
        writes no long-name entries.
        """
        self._cmd_expect("cd /user", b"/user wamos> ")
        self._cmd_expect("ls", b"README.TXT")


if __name__ == "__main__":
    unittest.main()
