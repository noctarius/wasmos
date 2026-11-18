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

PROBE = "/boot/apps/sv_probe.wap"


def _syslog_wait(session, needle: str, timeout_s: float) -> str:
    """The COM2 kernel log once it contains `needle`, or '' at the deadline."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        log = session.syslog_tail(1 << 20)
        if needle in log:
            return log
        time.sleep(0.25)
    return ""


def _submit(test, session, spec: str) -> None:
    start = session.send_command(f"svc submit {spec}", timeout_s=20)
    unit = spec.split()[0]
    if not session.expect_from(start, f"svc: {unit} accepted".encode(), timeout_s=30):
        test.fail(
            f"the supervisor did not accept {unit!r}.\n--- tail ---\n{session.tail()}\n"
        )
    if not session.expect_from(start, b"wamos> ", timeout_s=20):
        test.fail(f"no prompt after svc.\n--- tail ---\n{session.tail()}\n")


class SupervisorTests(unittest.TestCase):
    """The supervisor keeps submitted units running by their policy.

    The services sysinit.rc starts are units too, so the shell this test types
    into is itself supervised. The probe app (examples/c/sv_probe) is the unit
    under test: it crashes on demand, and a kenv key tells a restarted
    instance that it is one.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_1_boot_services_came_up_without_a_failure(self):
        log = self.session.syslog_tail(1 << 20)
        for line in log.splitlines():
            self.assertFalse(
                line.startswith("[supervisor]"),
                f"the supervisor reported on a clean boot: {line!r}\n"
                f"--- syslog ---\n{log[-3000:]}\n",
            )
            self.assertFalse(
                line.startswith("[sysinit] service failed"),
                f"sysinit could not hand over a unit: {line!r}",
            )

    def test_2_a_crashed_unit_is_restarted(self):
        _submit(self, self.session, f"probe-once {PROBE} -- crash-once probe-once")
        log = _syslog_wait(self.session, "[sv-probe] probe-once up after a restart", 30)
        self.assertTrue(
            log,
            "the crashed probe did not come back.\n"
            f"--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        self.assertIn(
            "[supervisor] probe-once ended (trapped",
            log,
            "the restart was not caused by, or not logged as, the trap",
        )
        self.assertIn("; restart 1 in ", log, "the restart was not the first one")

    def test_3_a_unit_that_keeps_crashing_is_given_up(self):
        _submit(self, self.session, f"probe-loop {PROBE} -- crash probe-loop")
        needle = "[supervisor] probe-loop failed (trapped"
        log = _syslog_wait(self.session, needle, 60)
        self.assertTrue(
            log,
            "the crash-looping probe was never given up on.\n"
            f"--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        self.assertIn("after 4 restarts; giving up", log)
        # Given up means no further start: the count of crashes stays at the
        # five the limit allowed.
        time.sleep(3.0)
        log = self.session.syslog_tail(1 << 20)
        self.assertEqual(log.count(needle), 1, "the give-up was logged more than once")
        self.assertEqual(
            log.count("[sv-probe] probe-loop crashing before READY"),
            5,
            "the probe was started a different number of times than the limit allows",
        )

    def test_4_a_duplicate_unit_is_refused(self):
        start = self.session.send_command(
            f"svc submit probe-once {PROBE} -- crash probe-dup", timeout_s=20
        )
        self.assertTrue(
            self.session.expect_from(
                start, b"svc: a unit of that name exists", timeout_s=30
            ),
            f"a second unit of the same name was accepted.\n--- tail ---\n{self.session.tail()}\n",
        )
        self.session.expect_from(start, b"wamos> ", timeout_s=20)


class SupervisorCriticalTests(unittest.TestCase):
    """A critical unit is never restarted: its death halts the system, through
    the orderly shutdown sequence, with a line naming the unit."""

    def test_a_critical_death_halts(self):
        session = QemuSession(default_config(), timeout_s=120, echo=True)
        session.start()
        try:
            session.require_prompt()
            _submit(
                self,
                session,
                f"probe-crit {PROBE} class=critical -- crash-once probe-crit",
            )
            self.assertTrue(
                _syslog_wait(
                    session, "[supervisor] critical unit probe-crit died (trapped", 30
                ),
                "the critical unit's death was not named.\n"
                f"--- syslog ---\n{session.syslog_tail(4096)}\n",
            )
            self.assertTrue(
                _syslog_wait(session, "[pm] shutdown: begin", 30),
                "the orderly shutdown sequence did not run.\n"
                f"--- syslog ---\n{session.syslog_tail(4096)}\n",
            )
            self.assertTrue(
                session.wait_for_exit(60),
                f"the system did not power off.\n--- syslog ---\n{session.syslog_tail(4096)}\n",
            )
            self.assertNotIn(
                "probe-crit up after a restart",
                session.syslog_tail(1 << 20),
                "a critical unit was restarted",
            )
        finally:
            session.force_stop()
            session.close()


if __name__ == "__main__":
    unittest.main()
