"""A package is granted privileged capabilities only when it is signed with the
build's key, and only for a requester entitled to hand them out.

Regression: 2026-11-26-manifest-grants-itself-privilege -- the kernel granted
whatever a package's manifest named. Any process could write a .wap declaring
spawn.grant, svc.control or system.control to a writable volume (/boot, /user
and /wfs all are) and spawn it, or spawn a privileged utility such as `svc` as
a deputy and steer it with arguments of its choosing.

priv_probe declares svc.control and reports that it started. The build packs
it signed (priv_probe.wap), unsigned (priv_unsigned.wap), and signed then
altered by one byte (priv_tampered.wap). priv_deputy exits only ipc.basic and
asks for priv_probe to be spawned; `priv_deputy exit` asks and exits without
waiting for the answer. The shell holds svc.control, so it may start
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
APP_START_FAILED = b"[pm] app start failed"


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
        # only reach the kernel-log port. Pumping the session moves pending
        # serial output into buf; syslog_tail reads its own file.
        self.session._pump(0.25)
        return (
            bytes(self.session.buf[start:])
            + self.session.syslog_tail(1 << 20)[syslog_start:].encode()
        )

    def _wait_for(self, start: int, syslog_start: int, needle: bytes) -> bytes:
        deadline = time.time() + 20
        log = self._log_since(start, syslog_start)
        while needle not in log and time.time() < deadline:
            log = self._log_since(start, syslog_start)
        return log

    def _run(self, command: str, expect: bytes, refused: bool = False) -> None:
        syslog_start = len(self.session.syslog_tail(1 << 20))
        start = self.session.send_command(command, timeout_s=20)
        log = self._wait_for(start, syslog_start, expect)
        tail = f"--- tail ---\n{self.session.tail()}\n"
        self.assertTrue(expect in log, f"`{command}`: no {expect!r}.\n{tail}")
        if refused:
            # The refused start's own failure is the child's terminal result;
            # only after it may the probe's absence be read as "never ran".
            log = self._wait_for(start, syslog_start, APP_START_FAILED)
            self.assertTrue(
                APP_START_FAILED in log,
                f"`{command}`: the refused child did not fail its start.\n{tail}",
            )
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=20),
            f"no prompt after `{command}`.\n{tail}",
        )
        if refused:
            log = self._log_since(start, syslog_start)
            self.assertFalse(
                STARTED in log, f"`{command}`: {STARTED!r} appeared.\n{tail}"
            )

    def test_a_signed_package_from_an_entitled_requester_starts(self):
        self._run("priv_probe", STARTED)

    def test_an_unsigned_package_gets_no_privilege(self):
        self._run("priv_unsigned", UNSIGNED, refused=True)

    def test_an_altered_package_gets_no_privilege(self):
        self._run("priv_tampered", UNSIGNED, refused=True)

    def test_a_requester_cannot_hand_out_what_it_does_not_hold(self):
        self._run("priv_deputy", REQUESTER, refused=True)

    def test_a_requester_that_exits_at_once_hands_out_nothing(self):
        # Regression: PR #88 review -- the requester's capabilities were read
        # when the child was created, after the process manager's filesystem
        # read, and a requester whose app had returned by then had lost its app
        # slot while its process still existed, so it was taken for the kernel,
        # which holds everything.
        # The requester may be gone before the process manager takes the
        # request, which then spawns nothing; either that or a refusal is
        # correct. Whether its exit lands inside the window the regression
        # needed -- after the request was taken, before the child started --
        # is timing, so this case can catch the regression but not always. The
        # outcome is read once the prompt is back and a settle window passed.
        syslog_start = len(self.session.syslog_tail(1 << 20))
        start = self.session.send_command("priv_deputy exit", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=20),
            f"no prompt after `priv_deputy exit`.\n{self.session.tail()}\n",
        )
        deadline = time.time() + 10
        log = self._log_since(start, syslog_start)
        while time.time() < deadline and APP_START_FAILED not in log:
            log = self._log_since(start, syslog_start)
        tail = f"--- tail ---\n{self.session.tail()}\n"
        self.assertFalse(
            STARTED in log, f"the exited requester's child started.\n{tail}"
        )
        if APP_START_FAILED in log:
            self.assertTrue(
                REQUESTER in log,
                f"the exited requester's child failed for another reason.\n{tail}",
            )


if __name__ == "__main__":
    unittest.main()
