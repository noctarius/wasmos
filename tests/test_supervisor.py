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

from qemu_test_framework import GuestRestarted, QemuSession, default_config

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


def _svc(test, session, args: str) -> str:
    """Runs `svc <args>` and returns what it printed before the next prompt."""
    start = session.send_command(f"svc {args}", timeout_s=20)
    if not session.expect_from(start, b"wamos> ", timeout_s=30):
        test.fail(f"no prompt after svc {args}.\n--- tail ---\n{session.tail()}\n")
    out = session.buf[start:].decode("utf-8", errors="replace")
    return out[: out.rfind("wamos> ")]


def _list_rows(test, session) -> dict:
    """`svc list` as {unit: [state, pid, restarts, last...]}."""
    out = _svc(test, session, "list")
    if "UNIT" not in out:
        test.fail(f"svc list printed no table.\n--- output ---\n{out}\n")
    rows = {}
    for line in out[out.index("UNIT") :].splitlines()[1:]:
        cols = line.split()
        if len(cols) >= 5:
            rows[cols[0]] = cols[1:]
    return rows


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

    def test_5_svc_list_reports_every_unit(self):
        rows = _list_rows(self, self.session)
        for unit in ("vt", "logger", "fontsvc", "gfxcomp", "cli@tty1"):
            self.assertIn(unit, rows, f"svc list is missing {unit}: {rows}")
            state, pid = rows[unit][0], rows[unit][1]
            self.assertIn(state, ("running", "stable"), f"{unit} is {state}")
            self.assertNotEqual(pid, "0", f"{unit} reports no pid")
        # test_3 gave this one up: no instance, the four restarts it got, and
        # the trap that ended it.
        self.assertIn("probe-loop", rows, f"svc list is missing probe-loop: {rows}")
        self.assertEqual(
            rows["probe-loop"][:3], ["failed", "0", "4"], rows["probe-loop"]
        )
        self.assertEqual(rows["probe-loop"][3], "trapped", rows["probe-loop"])

    def test_6_svc_status_reports_one_unit(self):
        out = _svc(self, self.session, "status cli@tty1")
        self.assertRegex(out, r"cli@tty1: (running|stable), pid [1-9][0-9]*")
        self.assertIn("restart=always class=required tty=1", out)
        out = _svc(self, self.session, "status no-such-unit")
        self.assertIn("svc: no unit named no-such-unit", out)

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
    the orderly shutdown sequence, with a line naming the unit and a dump of
    the unit table."""

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
            log = _syslog_wait(session, "[pm] shutdown: begin", 30)
            self.assertTrue(
                log,
                "the orderly shutdown sequence did not run.\n"
                f"--- syslog ---\n{session.syslog_tail(4096)}\n",
            )
            self.assertIn("; halting", log)
            self.assertIn("[supervisor] unit table:", log)
            self.assertIn(
                "[supervisor]   probe-crit failed pid=0 restarts=0 last=trapped", log
            )
            self.assertRegex(
                log, r"\[supervisor\]   cli@tty1 (running|stable) pid=[1-9]"
            )
            self.assertLess(
                log.index("[supervisor] unit table:"),
                log.index("[pm] shutdown: begin"),
                "the table must be dumped before the shutdown sequence",
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


class SupervisorCriticalRebootTests(unittest.TestCase):
    """supervisor.on_critical=reboot: the same named line and dump, the orderly
    shutdown sequence, and then the machine reboots instead of powering off."""

    def test_a_critical_death_reboots(self):
        cfg = default_config()
        cfg.kernel_options = ("supervisor.on_critical=reboot",)
        session = QemuSession(cfg, timeout_s=120, echo=True)
        session.start()
        try:
            session.require_prompt()
            _submit(
                self,
                session,
                f"probe-crit {PROBE} class=critical -- crash-once probe-crit",
            )
            # The firmware banner of the next boot is the reboot; the harness
            # reports it as GuestRestarted.
            with self.assertRaises(GuestRestarted):
                session.expect(b"never printed by any boot", timeout_s=90)
            log = session.syslog_tail(1 << 20)
            self.assertIn(
                "[supervisor] critical unit probe-crit died (trapped",
                log,
                f"the critical unit's death was not named.\n--- syslog ---\n{log[-4096:]}\n",
            )
            self.assertIn("; rebooting", log)
            self.assertIn("[supervisor] unit table:", log)
            self.assertIn(
                "[pm] shutdown: begin", log, "the reboot skipped the orderly shutdown"
            )
        finally:
            session.force_stop()
            session.close()


class SupervisorDeathPanicTests(unittest.TestCase):
    """The supervisor is a critical process: its own death is a kernel panic
    naming it. supervisor.fault=trap makes it trap on the first unit sysinit
    hands it."""

    def test_the_supervisors_death_panics(self):
        cfg = default_config()
        cfg.kernel_options = ("supervisor.fault=trap",)
        # The only test in this file that MEANS to panic the guest.
        session = QemuSession(cfg, timeout_s=180, echo=False, panic_is_fatal=False)
        session.start()
        try:
            # No require_prompt: the boot dies at sysinit's first `service`.
            self.assertTrue(
                session.expect(b"END KERNEL PANIC", timeout_s=150),
                f"the supervisor's death did not panic.\n--- tail ---\n{session.tail()}\n",
            )
            self.assertIn(b"reason : critical_process_ended", session.buf)
            log = session.syslog_tail(1 << 20)
            self.assertIn("[supervisor] supervisor.fault=trap: trapping", log)
            self.assertRegex(
                log,
                r"\[kernel\] critical process supervisor \(pid \d+\) ended \(reason 2,",
                f"the panic does not name the supervisor.\n--- syslog ---\n{log[-4096:]}\n",
            )
        finally:
            session.force_stop()
            session.close()


if __name__ == "__main__":
    unittest.main()
