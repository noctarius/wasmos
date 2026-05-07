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


class ShmemGrantRevokeE2ETest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=150, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _cmd_expect(self, cmd: str, needles: list[bytes], timeout_s: int = 30) -> None:
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
            self.fail(f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")

    def test_shmem_grant_revoke_pair(self):
        self._cmd_expect("cd /apps", [b"/apps wamos>"])
        self._cmd_expect("exec shmtgt", [b"spawned pid"], timeout_s=20)
        self._cmd_expect(
            "exec shmownr",
            [
                b"spawned pid",
                b"[test] shmem e2e pregrant deny ok",
                b"[test] shmem e2e grant map ok",
                b"[test] shmem e2e revoke deny ok",
                b"[test] shmem e2e done ok",
            ],
            timeout_s=60,
        )


if __name__ == "__main__":
    unittest.main()
