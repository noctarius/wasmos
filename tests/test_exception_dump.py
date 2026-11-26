"""The x86_64 exception dump reports the faulting context, not the stub's.

A CPU exception the kernel cannot absorb ends in x86_exception_panic_frame,
which prints the vector, error code, rip and cs from the frame its ISR stub
built, and hands rip and rsp to kpanic for the panicking CPU's block. That dump
is the first thing read after a hang or a fault, so a field that names the
wrong thing sends the reader somewhere else.

The boot option debug.exception_test=<vector> raises an exception in ring 0
from a stack of its own, after logging the rip and rsp the dump must then
report: vector 1 is a real single-step #DB, vector 5 is raised with `int`,
whose frame is that of any vector without an error code.

Regression: 2026-11-26-x86-db-stub-no-push-regs -- the #DB stub's panic path
skipped PUSH_REGS, so the dump read the CPU frame as the register block and
printed whatever lay beyond it.

Regression: 2026-11-26-x86-decl-exc-no-error-slot -- DECL_EXC pushed no zero
error code for vectors without one, so the dump printed rip as "err" and cs as
"rip".

Regression: 2026-11-26-x86-kernel-fault-rsp -- a ring-0 fault reported the
address of the exception frame as rsp instead of the interrupted stack pointer,
which long mode always saves in the frame.
"""

import os
import re
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config

NOT_RUN = re.compile(rb"\[exception-test\] [^\r\n]*not run")
EXPECT = re.compile(
    rb"\[exception-test\] vector=(\d+) rip=([0-9a-f]{16}) rsp=([0-9a-f]{16})"
)
PANICKING = re.compile(rb"panicking_cpu=(\d+)")


def _field(buf: bytes, name: str) -> bytes:
    m = re.search(rb"\[cpu\] " + name.encode() + rb"=([0-9a-f]{16})", buf)
    return m.group(1) if m else b"<missing>"


class ExceptionDumpTest(unittest.TestCase):
    def _raise(self, vector: int) -> None:
        cfg = default_config()
        # One CPU: the exception dump is printed before kpanic stops the other
        # CPUs, so on SMP their log lines can split its fields mid-line.
        # Stopping them is test_kpanic_stop's subject, not this one's.
        cfg.smp_count = 1
        cfg.kernel_options = (f"debug.exception_test={vector}",)
        session = QemuSession(cfg, timeout_s=120, echo=False, panic_is_fatal=False)
        session.start()
        try:
            ended = re.compile(rb"END KERNEL PANIC|" + NOT_RUN.pattern)
            self.assertTrue(
                session.expect(ended, timeout_s=120),
                f"the boot neither panicked nor declined the test.\n"
                f"--- tail ---\n{session.tail(4096)}\n",
            )
            log = session.buf + session.syslog_tail(1 << 20).encode()
            not_run = NOT_RUN.search(log)
            if not_run:
                self.skipTest(not_run.group(0).decode())
            buf = session.buf
            tail = f"--- tail ---\n{session.tail(8192)}\n"
            expect = EXPECT.search(buf)
            self.assertIsNotNone(expect, f"the trigger logged no expectation.\n{tail}")
            self.assertEqual(int(expect.group(1)), vector)
            rip, rsp = expect.group(2), expect.group(3)

            self.assertEqual(_field(buf, "exception vector"), b"%016x" % vector, tail)
            self.assertEqual(
                _field(buf, "err"), b"0" * 16, f"err is not the zero slot.\n{tail}"
            )
            self.assertEqual(
                _field(buf, "rip"), rip, f"rip is not the faulting one.\n{tail}"
            )
            self.assertEqual(
                _field(buf, "cs"),
                b"%016x" % 0x08,
                f"cs is not the kernel selector.\n{tail}",
            )

            panicking = PANICKING.search(buf)
            self.assertIsNotNone(panicking, f"the dump names no panicking CPU.\n{tail}")
            block = re.search(
                rb"--- CPU "
                + panicking.group(1)
                + rb" captured=\d[^\n]*\n\s+rip=([0-9a-f]{16}) rsp=([0-9a-f]{16})",
                buf,
            )
            self.assertIsNotNone(
                block, f"the panicking CPU has no captured frame.\n{tail}"
            )
            self.assertEqual(
                block.group(1), rip, f"kpanic's rip is not the faulting one.\n{tail}"
            )
            self.assertEqual(
                block.group(2),
                rsp,
                f"kpanic's rsp is not the interrupted stack.\n{tail}",
            )
        finally:
            session.close()

    def test_a_kernel_debug_trap_is_dumped_with_its_own_frame(self):
        self._raise(1)

    def test_a_vector_without_an_error_code_is_dumped_with_its_own_frame(self):
        self._raise(5)


if __name__ == "__main__":
    unittest.main()
