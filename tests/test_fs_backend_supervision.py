"""fs-manager's filesystem backends are supervised units.

fs-manager places a mount by submitting its backend to the supervisor as
`<driver>@<mount path>`, restart=on-failure, instead of spawning it. A backend
that dies is restarted, and the mount it serves comes back when the successor
registers; open files on it are lost with the process that held them. An
unmount quiesces the backend, waits for it to end, retracts its unit, and only
then answers, so the path can be mounted again at once.
"""

import os
import sys
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config

HELLO_TEXT = b"hello from the WFS volume"
WFS_UNIT = "fs_wfs@/wfs"
TMPFS_UNIT = "fs_tmpfs@/home/user"
PROBE_MOUNT = "/svmnt"


class FsBackendSupervisionTest(unittest.TestCase):
    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls):
        cls.session = QemuSession(default_config(), timeout_s=180)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def _run(self, cmd: str, timeout_s: int = 30) -> str:
        assert self.session is not None
        start = self.session.send_command(cmd, timeout_s=timeout_s)
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=timeout_s),
            f"{cmd!r} never returned to the prompt\n--- tail ---\n{self.session.tail()}\n",
        )
        return self.session.buf[start:].decode("utf-8", errors="replace")

    def _units(self) -> dict:
        rows = {}
        for line in self._run("svc list").splitlines():
            fields = line.split()
            if len(fields) >= 5 and fields[0] != "UNIT":
                rows[fields[0]] = fields[1:]
        return rows

    def _wait_unit(self, unit: str, pred, timeout_s: float = 30) -> list:
        deadline = time.time() + timeout_s
        row = []
        while time.time() < deadline:
            row = self._units().get(unit, [])
            if pred(row):
                return row
            time.sleep(0.5)
        self.fail(f"{unit} never reached the expected state; last row {row}")

    def _cat_hello(self) -> None:
        out = self._run("cat /wfs/hello.txt", timeout_s=40)
        self.assertIn(
            HELLO_TEXT.decode(), out, f"/wfs/hello.txt did not read back:\n{out}"
        )

    def test_1_boot_mounts_are_supervised_units(self):
        rows = self._units()
        for unit in (WFS_UNIT, TMPFS_UNIT):
            self.assertIn(
                unit, rows, f"{unit} is not a supervised unit: {sorted(rows)}"
            )
            self.assertIn(rows[unit][0], ("running", "stable"), f"{unit}: {rows[unit]}")

    def test_2_a_killed_backend_is_restarted_and_its_mount_returns(self):
        self._cat_hello()
        before = self._units()[WFS_UNIT]
        manager = self._units()["fs-manager"]
        out = self._run(f"svc kill {WFS_UNIT}")
        self.assertIn(f"svc: killing {WFS_UNIT}", out)
        self._wait_unit(
            WFS_UNIT,
            lambda row: row and row[0] in ("running", "stable") and row[1] != before[1],
        )
        # The successor registers its mount after READY; read until it has.
        deadline = time.time() + 30
        out = ""
        while time.time() < deadline:
            out = self._run("cat /wfs/hello.txt", timeout_s=40)
            if HELLO_TEXT.decode() in out:
                break
            time.sleep(0.5)
        self.assertIn(HELLO_TEXT.decode(), out, "the mount did not come back")
        self.assertEqual(
            self._units()["fs-manager"][1], manager[1], "fs-manager restarted"
        )

    def test_3_an_unmount_retracts_the_unit_and_frees_the_path(self):
        unit = f"fs_tmpfs@{PROBE_MOUNT}"
        for _ in range(2):
            out = self._run(f"mount -t tmpfs {PROBE_MOUNT}", timeout_s=40)
            self.assertIn("mount: mounted", out, out)
            self.assertIn(unit, self._units(), "the mount's backend is not a unit")
            out = self._run(f"umount {PROBE_MOUNT}", timeout_s=40)
            self.assertIn(f"umount: {PROBE_MOUNT} unmounted", out, out)
            self.assertNotIn(unit, self._units(), "the unmount left the unit behind")


if __name__ == "__main__":
    unittest.main()
