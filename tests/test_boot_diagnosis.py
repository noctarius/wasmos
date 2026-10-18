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

from scripts.qemu_test_framework import QemuConfig, QemuSession


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


if __name__ == "__main__":
    unittest.main()
