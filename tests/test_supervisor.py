import os
import re
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
        # require_prompt already waited for the milestone; it is said once,
        # and it is the only thing the supervisor says on a clean boot.
        self.assertRegex(
            log, r"\[supervisor\] boot complete at \d+ ms: 3 required units up"
        )
        self.assertEqual(log.count("[supervisor] boot complete"), 1)
        # Kernel init spawns sysinit on fs-manager's word that /boot is
        # mounted, so the one spawn reads it: no failed read, no retry.
        #
        # Regression: 2026-11-19-fsmgr-mount-reply-pulls-pm -- with sysinit no
        # longer waiting for device-manager, its spawns overlap the mounts
        # device-manager still makes, and a read timed out after 15 s: on a
        # mount driver's spawn reply fs-manager asked the PM, synchronously,
        # for the backend class, while the PM was waiting for fs-manager to
        # answer its read of the next module. Each waited on the other until
        # the PM gave up, and gfx_smoke never started.
        self.assertEqual(log.count("[init] /boot is mounted; spawning sysinit"), 1)
        self.assertNotIn("[pm] spawn_path fs read failed", log)
        for line in log.splitlines():
            if line.startswith("[supervisor] boot complete"):
                continue
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
        # cli's READY names no loop endpoint, so it is not probed.
        self.assertIn("liveness: not probed", out)
        out = _svc(self, self.session, "status no-such-unit")
        self.assertIn("svc: no unit named no-such-unit", out)

    def test_7_a_hung_unit_is_killed_and_restarted(self):
        # A hang never ends the process, so no exit tells the supervisor;
        # only the unanswered probes do.
        #
        # Regression: 2026-11-19-pm-kill-judged-by-the-pm -- the supervisor
        # logged the kill and the unit never ended: the PM checked that the
        # sender of PROC_IPC_KILL was the parent, then called process_kill,
        # which checked the PM's own pid instead, and the PM is nobody's
        # parent. Every supervisor kill (missed READY, missed probes) was a
        # no-op.
        _submit(
            self, self.session, f"probe-hang {PROBE} probe=1 -- hang-once probe-hang"
        )
        up = "[sv-probe] probe-hang up after a restart"
        log = _syslog_wait(self.session, up, 60)
        self.assertTrue(
            log,
            "the hung probe was not killed and restarted.\n"
            f"--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        steps = [
            "[sv-probe] probe-hang hanging in a nested receive",
            "[supervisor] probe-hang missed 3 liveness probes; killing pid",
            "[supervisor] probe-hang ended (killed",
            up,
        ]
        at = [log.find(step) for step in steps]
        self.assertNotIn(-1, at, f"a step is missing: {dict(zip(steps, at))}")
        self.assertEqual(
            at, sorted(at), f"the steps are out of order: {dict(zip(steps, at))}"
        )
        out = _svc(self, self.session, "status probe-hang")
        self.assertRegex(out, r"probe-hang: (running|stable), pid [1-9][0-9]*")
        self.assertIn("restarts: 1", out)
        self.assertIn("last end: killed", out)
        self.assertIn("liveness: every 1000 ms", out)

    def test_8_a_busy_unit_that_returns_to_its_loop_is_not_killed(self):
        _submit(self, self.session, f"probe-busy {PROBE} probe=1 -- busy probe-busy")
        log = _syslog_wait(self.session, "[sv-probe] probe-busy done being busy", 60)
        self.assertTrue(
            log,
            "the busy probe never finished.\n"
            f"--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        self.assertNotIn(
            "[supervisor] probe-busy",
            log,
            "the supervisor acted on a unit that kept answering",
        )
        out = _svc(self, self.session, "status probe-busy")
        self.assertIn("restarts: 0", out)
        answered = re.search(
            r"liveness: every 1000 ms, (\d+) answered, 0 missed in a row", out
        )
        self.assertTrue(answered, f"svc status shows no clean probe record:\n{out}")
        self.assertGreaterEqual(
            int(answered.group(1)), 5, f"too few probes answered while busy:\n{out}"
        )

    def test_9_a_killed_driver_is_restarted_and_serves_again(self):
        # device-manager hands rtc to the supervisor (default.rules,
        # ENV{RESTART}="on-failure"), so a kill on request is a failure the
        # supervisor restarts it from, and date reaches the new instance.
        unit = "rtc@acpi:0070"
        before = _list_rows(self, self.session)
        self.assertIn(unit, before, f"rtc is not a supervised unit: {sorted(before)}")
        self.assertIn(before[unit][0], ("running", "stable"))
        old_pid = before[unit][1]
        restarts = int(before[unit][2])

        out = _svc(self, self.session, f"kill {unit}")
        self.assertIn(f"svc: killing {unit} (pid {old_pid})", out)
        requested = f"[supervisor] {unit} kill requested; killing pid {old_pid}"
        ended = f"[supervisor] {unit} ended (killed"
        log = _syslog_wait(self.session, ended, 30)
        self.assertTrue(
            log,
            f"{unit} did not end.\n"
            f"--- syslog ---\n{self.session.syslog_tail(4096)}\n",
        )
        self.assertIn(requested, log)
        self.assertLess(log.find(requested), log.find(ended))
        # The kill is a failure, so the on-failure unit is restarted.
        self.assertRegex(
            log,
            re.escape(ended) + rf", status -?\d+\); restart {restarts + 1} in ",
        )

        deadline = time.time() + 30
        row = []
        while time.time() < deadline:
            row = _list_rows(self, self.session).get(unit, [])
            if row and row[0] in ("running", "stable") and row[1] != old_pid:
                break
            time.sleep(0.5)
        self.assertTrue(
            row and row[0] in ("running", "stable") and row[1] != old_pid,
            f"{unit} did not come back as a new instance: {row}",
        )
        self.assertEqual(int(row[2]), restarts + 1)

        out = _svc(self, self.session, "status " + unit)
        self.assertIn("last end: killed", out)
        start = self.session.send_command("date", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=30),
            f"no prompt after date.\n--- tail ---\n{self.session.tail()}\n",
        )
        out = self.session.buf[start:].decode("utf-8", errors="replace")
        self.assertRegex(
            out,
            r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}",
            f"date did not reach the restarted rtc:\n{out}",
        )

        out = _svc(self, self.session, "kill no-such-unit")
        self.assertIn("svc: no unit of that name", out)

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
