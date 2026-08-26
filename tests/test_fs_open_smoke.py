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


class FsOpenSmokeTest(unittest.TestCase):
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

    def _cmd_expect(self, cmd: str, needles: list[bytes], timeout_s: int = 20) -> None:
        mark = self.session.mark()
        self.session.send(cmd)
        for needle in needles:
            ok = self.session.expect_from(mark, needle, timeout_s=timeout_s)
            if not ok:
                self.fail(
                    f"Expected output not found for '{cmd}': {needle!r}\n--- tail ---\n{self.session.tail()}\n"
                )
        ok = self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
        if not ok:
            self.fail(
                f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )

    def test_exec_fs_open_smoke(self):
        self._cmd_expect("cd /boot", [b"/boot wamos>"])
        self._cmd_expect("ls", [b"large_read.txt"])
        self._cmd_expect("cd /", [b"/ wamos>"])
        self._cmd_expect("fs_open_smoke", [b"fs-open-smoke: ok"])

    def test_cat_a_boot_partition_file_by_absolute_path(self):
        """`cat` a file on the FAT boot volume, named absolutely.

        The absolute form is the one that does not depend on a spawned utility
        inheriting anything, so it isolates the read path from the working
        directory question below.
        """
        self._cmd_expect("cd /", [b"/ wamos>"])
        self._cmd_expect(
            "cat /boot/write_smoke.txt", [b"WASMOS-WRITE-SMOKE"], timeout_s=30
        )

    def test_cat_a_boot_partition_file_by_relative_name(self):
        """`cat` the same file from inside /boot, named relatively.

        This is the case that decides whether a spawned utility resolves against
        the directory its spawner stood in. `cat` is a separate process, so the
        name it is given has to be resolved against a working directory it did
        not set itself.
        """
        self._cmd_expect("cd /boot", [b"/boot wamos>"])
        self._cmd_expect("cat write_smoke.txt", [b"WASMOS-WRITE-SMOKE"], timeout_s=30)
        self._cmd_expect("cd /", [b"/ wamos>"])


if __name__ == "__main__":
    unittest.main()
