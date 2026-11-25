"""kpanic's stop reaches a CPU that takes no ordinary interrupt.

A panic stops every other CPU and prints each one's frame, which is how a dump
shows where a wedged CPU was. The CPU that most needs it is the one that takes
no ordinary interrupt: one spinning with interrupts masked, which is what a
deadlocked spinlock holder looks like, or one stuck inside an interrupt
handler before it ended the interrupt, which the interrupt controller holds at
that interrupt's priority. x86_64 stops CPUs with an NMI, which neither holds
off. The boot option debug.kpanic_stop_test=<mode> builds each situation on
purpose -- mode 1 masks interrupts on CPU 1, mode 2 spins inside CPU 1's timer
interrupt -- and then the boot CPU panics.

Regression: 2026-11-25-aarch64-stop-masked-cpu -- on aarch64 the stop was an
ordinary SGI, which a CPU with interrupts masked never takes, so the dump showed
CPU 1 as "<no NMI capture>" -- exactly the CPU a hang investigation needs.

Regression: 2026-11-25-aarch64-stop-priority -- the stop SGI, once an FIQ, still
had the priority every IRQ has, so a CPU stuck in an IRQ handler before its end
of interrupt ran at that priority and the GIC never signalled the stop.
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

NOT_RUN = re.compile(rb"\[kpanic-stop-test\] needs [^\r\n]*")
CPU1 = re.compile(rb"--- CPU 1 captured=(\d)")


class KpanicStopTest(unittest.TestCase):
    def _panic_with_cpu1_spinning(self, mode: int, where: str) -> None:
        cfg = default_config()
        cfg.smp_count = max(cfg.smp_count, 2)
        cfg.kernel_options = (f"debug.kpanic_stop_test={mode}",)
        session = QemuSession(cfg, timeout_s=120, echo=False, panic_is_fatal=False)
        session.start()
        try:
            # Both lines are written while the boot is still single-threaded on
            # the console: the decline as early kernel log, the dump directly.
            ended = re.compile(rb"END KERNEL PANIC|" + NOT_RUN.pattern)
            self.assertTrue(
                session.expect(ended, timeout_s=120),
                f"the boot neither panicked nor declined the test.\n"
                f"--- tail ---\n{session.tail(4096)}\n",
            )
            # The [kpanic-stop-test] lines are kernel log, which may also or
            # only reach the kernel-log port.
            log = session.buf + session.syslog_tail(1 << 20).encode()
            not_run = NOT_RUN.search(log)
            if not_run:
                self.skipTest(not_run.group(0).decode())
            spinning = re.search(
                rb"\[kpanic-stop-test\] cpu 1 spinning \(mode %d\): (\d)" % mode, log
            )
            self.assertTrue(
                spinning and spinning.group(1) == b"1",
                f"CPU 1 never reported spinning {where}.\n"
                f"--- tail ---\n{session.tail(4096)}\n",
            )
            cpu1 = CPU1.search(session.buf)
            self.assertIsNotNone(
                cpu1,
                f"the dump has no CPU 1 block.\n--- tail ---\n{session.tail(4096)}\n",
            )
            self.assertEqual(
                cpu1.group(1),
                b"1",
                f"kpanic's stop did not reach CPU 1, which spun {where}.\n"
                f"--- tail ---\n{session.tail(4096)}\n",
            )
        finally:
            session.close()

    def test_a_cpu_spinning_with_interrupts_masked_is_captured(self):
        self._panic_with_cpu1_spinning(1, "with interrupts masked")

    def test_a_cpu_spinning_inside_an_interrupt_handler_is_captured(self):
        self._panic_with_cpu1_spinning(2, "inside its timer interrupt")


if __name__ == "__main__":
    unittest.main()
