#!/usr/bin/env python3
"""`QemuSession.boot_diagnosis` must name the cause, not the consequence.

The harness waits for a prompt, so "no prompt" is the terminal symptom of every
boot failure and names none of them. `boot_diagnosis` exists to say which one it
was, and the risk it carries is the one it was written to remove: a sentence
that sounds like a diagnosis and is not.

These run on the host with a stub process, because every input that matters is
a serial log the harness already holds, and producing them from a real guest
means arranging a panic, a reset and a wedge on demand.

Nothing here calls `default_config()`. It resolves OVMF and the ESP out of the
environment or a CMakeCache and RAISES when neither is there, which is exactly
the state the host-tools battery runs in -- it exists to cover what needs no
QEMU. `boot_diagnosis` reads a buffer and a process handle and nothing else, so
a config that names no real file is the honest input.

Regression: 2026-10-18-stall-reported-as-qemu-exit -- `expect()` dumps stall
state on timeout and NMIs every vCPU to do it, which can kill QEMU. The exit
check ran before the stall check, so a wedged guest the harness had just
provoked into dying was reported as "qemu exited with code 0 before the prompt"
-- an exit the harness caused, named as the reason the boot failed. Three CI
triages started from that sentence and had to be re-done from the raw log.
"""

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from scripts.qemu_test_framework import GuestPanicked, QemuConfig, QemuSession
from tests.test_virtio_blk import _why_the_command_produced_nothing


class _StubProc:
    """Just enough of subprocess.Popen for boot_diagnosis: it asks poll() and
    returncode and nothing else."""

    def __init__(self, returncode=None):
        self.returncode = returncode

    def poll(self):
        return self.returncode


def _session(
    buf: bytes, returncode=None, restarted: bool = False, stall_dumps: int = 0
) -> QemuSession:
    """A session that was never started, carrying `buf` as its serial log.

    The paths are placeholders and are never opened: QemuSession.__init__ only
    stores its config, and nothing below starts a guest.
    """
    session = QemuSession(QemuConfig(ovmf_code="", ovmf_vars="", esp_dir=""))
    session.buf = buf
    session.proc = _StubProc(returncode)
    session._restarted = restarted
    # How many times expect() gave up and dumped. This is the ONLY record that
    # the harness provoked the guest: the dump is printed, never appended to
    # session.buf, which holds serial bytes and nothing else.
    session._stall_dumps = stall_dumps
    return session


# What the GUEST prints because the dump NMIs it. These reach session.buf over
# serial, unprefixed; the harness's own "[stall-dump] ..." lines are printed to
# the console and never appear here, which is why a dump is detected by the
# counter rather than by a marker in the buffer.
INDUCED_GUEST_OUTPUT = (
    b"[diag] tid=45 pid=36 fs-fat st=blocked rsn=event\n"
    b"[nmi] unexpected NMI cpu=0 rip=ffffffff80201f72\n"
)


class BootDiagnosisTest(unittest.TestCase):
    def test_a_live_wedge_is_a_stall_naming_the_last_guest_line(self) -> None:
        s = _session(b"[fat] FAT16 detected\n[fat] fs.backend registered\n")
        d = s.boot_diagnosis()
        self.assertIn("stall", d)
        self.assertIn("fs.backend registered", d)

    def test_a_guest_that_said_nothing_is_not_a_stall(self) -> None:
        self.assertIn("never reached the kernel", _session(b"   ").boot_diagnosis())

    def test_a_reboot_outranks_everything_else(self) -> None:
        s = _session(b"wamos> \nBdsDxe: loading Boot\n", returncode=0, restarted=True)
        self.assertIn("REBOOTED", s.boot_diagnosis())

    def test_a_panic_outranks_an_exit(self) -> None:
        """A guest that panicked and then died did not die of dying."""
        s = _session(
            b"KERNEL PANIC\nreason : cpu_exception\npanicking_cpu=0\n"
            b"END KERNEL PANIC\n",
            returncode=1,
        )
        d = s.boot_diagnosis()
        self.assertIn("panicked", d)
        self.assertIn("cpu_exception", d)

    def test_an_exit_the_harness_did_not_provoke_is_reported_as_one(self) -> None:
        """The case the exit branch is actually for: no stall dump ran, so the
        guest went away on its own and that IS the cause."""
        s = _session(b"[kernel] booting\n", returncode=0)
        self.assertIn("qemu exited", s.boot_diagnosis())

    def test_an_exit_during_the_stall_dump_is_not_the_cause(self) -> None:
        """The regression. The dump had already run, so the guest was alive and
        wedged when the harness looked -- and the exit is the harness's own NMI
        sampling killing it, which is a consequence of diagnosing, not a cause.

        Asserted as `stall` first and the last GUEST line second, because a
        diagnosis that merely mentioned both would still have led a reader to
        the exit.
        """
        s = _session(
            b"[fat] fs.backend registered\n" + INDUCED_GUEST_OUTPUT,
            returncode=0,
            stall_dumps=1,
        )
        d = s.boot_diagnosis()
        self.assertIn("stall", d)
        self.assertIn("fs.backend registered", d)
        self.assertNotIn("before the prompt", d)

    def test_an_exit_during_the_dump_is_still_mentioned(self) -> None:
        """Not the cause is not the same as not worth saying: a reader chasing
        a truncated log needs to know the process is gone and why."""
        s = _session(
            b"[fat] fs.backend registered\n" + INDUCED_GUEST_OUTPUT,
            returncode=0,
            stall_dumps=1,
        )
        self.assertIn("during the stall dump", s.boot_diagnosis())

    def test_the_dumps_own_lines_are_never_the_last_guest_output(self) -> None:
        """`[diag]` and `[nmi]` are the guest answering the sampler, so they are
        newer than the stall and would report the diagnosis back as the symptom.
        """
        s = _session(
            b"[fat] fs.backend registered\n" + INDUCED_GUEST_OUTPUT, stall_dumps=1
        )
        d = s.boot_diagnosis()
        self.assertIn("fs.backend registered", d)
        self.assertNotIn("[diag]", d)
        self.assertNotIn("[nmi]", d)


class GuestPanicDetectionTest(unittest.TestCase):
    """A panicked guest ends the session instead of failing the next assertion.

    Regression: 2026-10-18-panic-reported-as-a-flaky-test -- a boot that
    panicked `init_no_sysinit`, meaning no userspace at all, was reported as
    `FAIL: test_dhcp_lease_brings_interface_up` and written off as a flaky
    networking test. The DHCP lease was never the issue. Every assertion after
    a panic is made against a dead machine, so the first one to notice gets
    blamed for it.
    """

    PANIC = (
        b"================= KERNEL PANIC =================\n"
        b"reason : init_no_sysinit\n"
        b"cpus=4  panicking_cpu=0\n"
        b"=============== END KERNEL PANIC ===============\n"
    )

    def _session(self, buf: bytes, fatal: bool = True) -> QemuSession:
        session = QemuSession(
            QemuConfig(ovmf_code="", ovmf_vars="", esp_dir=""), panic_is_fatal=fatal
        )
        session.buf = buf
        # Stopping the VM would discard the halted machine, which is the whole
        # point of not doing it; a call here fails the test rather than passing
        # silently.
        session.force_stop = self.fail  # type: ignore[method-assign]
        return session

    def test_a_complete_panic_ends_the_session(self) -> None:
        s = self._session(b"[init] something\n" + self.PANIC)
        with self.assertRaises(GuestPanicked) as caught:
            s._note_panic()
        self.assertIn("init_no_sysinit", str(caught.exception))

    def test_a_partial_dump_does_not_fire_yet(self) -> None:
        """Anchored on the CLOSING banner, so the message carries the whole
        block rather than whatever had arrived when the first line did."""
        s = self._session(b"KERNEL PANIC\nreason : init_no_sysinit\n")
        s._note_panic()

    def test_it_fires_once(self) -> None:
        """So teardown can still run; a second raise from the read pump during
        cleanup would mask the first."""
        s = self._session(self.PANIC)
        with self.assertRaises(GuestPanicked):
            s._note_panic()
        s._note_panic()

    def test_a_test_that_induces_a_panic_can_opt_out(self) -> None:
        s = self._session(self.PANIC, fatal=False)
        s._note_panic()

    def test_a_healthy_session_is_untouched(self) -> None:
        s = self._session(b"[fat] fs.backend registered mount=/boot\nwamos> \n")
        s._note_panic()


if __name__ == "__main__":
    unittest.main()


class CommandResolutionDiagnosisTest(unittest.TestCase):
    """`no such command found` must not be reported as an unmounted /boot.

    Regression: 2026-10-19-blkinfo-resolution-blamed-on-boot -- the diagnostic
    appended to every failed expectation in test_virtio_blk.py asserted that
    `/boot` is not mounted whenever the CLI could not resolve a command. CI run
    39154123903 showed that claim being wrong in the most direct way available:
    nine lines of `[blkinfo] ...` output, and then the very next `blkinfo` in
    the SAME session answered `no such command found`. The same boot had logged
    `[fat] fs.backend registered mount=/boot` and loaded a keymap from
    `/boot/system/keymaps/`.

    The CLI cannot tell the difference. `cli_resolve_exec_path` probes each PATH
    entry with `fopen()`, and `fopen` turns `open()`'s packed reason into a bare
    NULL, so a transient FS failure and a missing file produce the same message.
    A diagnosis built on top of it must not invent the distinction either.
    """

    def test_a_command_that_ran_earlier_is_not_an_unmounted_boot(self) -> None:
        session = _session(
            b"[blkinfo] id=block:ata:0 driver=ata unit=0 sectors=1032192\n"
            b"/ wamos> blkinfo\n"
        )
        mark = len(session.buf)
        session.buf += b"no such command found: blkinfo\n"
        text = _why_the_command_produced_nothing(session, mark, "blkinfo")
        self.assertNotIn("/boot is not mounted", text)
        self.assertIn("earlier in this same session", text)

    def test_a_command_never_seen_does_not_name_a_cause_it_cannot_know(self) -> None:
        session = _session(b"/ wamos> blkinfo\n")
        mark = len(session.buf)
        session.buf += b"no such command found: blkinfo\n"
        text = _why_the_command_produced_nothing(session, mark, "blkinfo")
        self.assertNotIn("/boot is not mounted", text)
        self.assertIn("no such command found", text)

    def test_output_before_the_mark_is_what_counts_as_having_run(self) -> None:
        """Only output the command produced BEFORE this attempt proves it was
        resolvable; the echo of the failing command itself must not count."""
        session = _session(b"/ wamos> blkinfo --write block:virtio-blk:40 1\n")
        mark = len(session.buf)
        session.buf += b"no such command found: blkinfo\n"
        text = _why_the_command_produced_nothing(session, mark, "blkinfo")
        self.assertNotIn("earlier in this same session", text)

    def test_a_failure_with_no_resolution_error_adds_nothing(self) -> None:
        session = _session(b"[blkinfo] id=block:ata:0 driver=ata unit=0\n")
        mark = len(session.buf)
        session.buf += b"[blkinfo] id=block:ata:0 lba=0 data=00\n"
        self.assertEqual(
            _why_the_command_produced_nothing(session, mark, "blkinfo"), ""
        )
