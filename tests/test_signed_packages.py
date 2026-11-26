"""A package is granted privileged capabilities only when it is signed with the
build's key, and only for a requester entitled to hand them out.

Regression: 2026-11-26-manifest-grants-itself-privilege -- the kernel granted
whatever a package's manifest named. Any process could write a .wap declaring
spawn.grant, svc.control or system.control to a writable volume (/boot, /user
and /wfs all are) and spawn it, or spawn a privileged utility such as `svc` as
a deputy and steer it with arguments of its choosing.

priv_probe declares svc.control and reports that it started. The build packs
it signed (priv_probe.wap), unsigned (priv_unsigned.wap), and signed then
altered by one byte (priv_tampered.wap). priv_deputy holds only ipc.basic and
asks for priv_probe to be spawned. The shell holds svc.control, so it may start
the signed probe; nothing else here may.
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

STARTED = b"[priv-probe] started"
UNSIGNED = b"[pm] refused svc.control to priv-probe: the image is not signed"
REQUESTER = b"[pm] refused svc.control to priv-probe: the requester holds neither"


class SignedPackagesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = QemuSession(default_config(), timeout_s=120, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _log_since(self, start: int, syslog_start: int) -> bytes:
        # The probe's line and the refusals are kernel log, which may also or
        # only reach the kernel-log port.
        return (
            bytes(self.session.buf[start:])
            + self.session.syslog_tail(1 << 20)[syslog_start:].encode()
        )

    def _run(self, command: str, expect: bytes, forbid: bytes = b"") -> None:
        syslog_start = len(self.session.syslog_tail(1 << 20))
        start = self.session.send_command(command, timeout_s=20)
        deadline = time.time() + 20
        log = b""
        while time.time() < deadline:
            log = self._log_since(start, syslog_start)
            if expect in log:
                break
            time.sleep(0.25)
        tail = f"--- tail ---\n{self.session.tail()}\n"
        self.assertTrue(expect in log, f"`{command}`: no {expect!r}.\n{tail}")
        if forbid:
            self.assertFalse(
                forbid in log, f"`{command}`: {forbid!r} appeared.\n{tail}"
            )
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=20),
            f"no prompt after `{command}`.\n{tail}",
        )

    def test_a_signed_package_from_an_entitled_requester_starts(self):
        self._run("priv_probe", STARTED)

    def test_an_unsigned_package_gets_no_privilege(self):
        self._run("priv_unsigned", UNSIGNED, forbid=STARTED)

    def test_an_altered_package_gets_no_privilege(self):
        self._run("priv_tampered", UNSIGNED, forbid=STARTED)

    def test_a_requester_cannot_hand_out_what_it_does_not_hold(self):
        self._run("priv_deputy", REQUESTER, forbid=STARTED)


if __name__ == "__main__":
    unittest.main()
