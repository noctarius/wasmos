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

    def _exec_expect(self, cmd: str, timeout_s: int = 30, retries: int = 3) -> None:
        last_tail = b""
        for _ in range(retries):
            mark = self.session.mark()
            self.session.send(cmd)
            if self.session.expect_from(mark, b"spawned pid", timeout_s=timeout_s):
                ok = self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
                if not ok:
                    self.fail(f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")
                return
            self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
            last_tail = self.session.tail()
        self.fail(f"Expected output not found for '{cmd}': b'spawned pid' after {retries} tries\n--- tail ---\n{last_tail}\n")

    def _expect_markers_from(self, mark: int, needles: list[bytes], timeout_s: int = 60) -> None:
        for needle in needles:
            ok = self.session.expect_from(mark, needle, timeout_s=timeout_s)
            if not ok:
                self.fail(
                    f"Expected output not found: {needle!r}\n--- tail ---\n{self.session.tail()}\n"
                )

    def test_shmem_grant_revoke_pair(self):
        self._cmd_expect("cd /apps", [b"/apps wamos>"])
        self._exec_expect("exec shmtgt", timeout_s=20)
        mark = self.session.mark()
        self.session.send("exec shmownr")
        self.assertTrue(self.session.expect_from(mark, b"spawned pid", timeout_s=20))
        self.assertTrue(self.session.expect_from(mark, b"wamos> ", timeout_s=20))
        self._expect_markers_from(
            mark,
            [
                b"[test] shmem e2e forged id deny ok",
                b"[test] shmem e2e map policy deny ok",
                b"[test] shmem e2e pregrant deny ok",
                b"[test] shmem e2e grant map ok",
                b"[test] shmem e2e revoke deny ok",
                b"[test] shmem e2e stale revoke deny ok",
                b"[test] shmem e2e done ok",
            ],
            timeout_s=60,
        )


if __name__ == "__main__":
    unittest.main()
