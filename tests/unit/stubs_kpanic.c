/* kpanic for the host tests.
 *
 * The kernel's kpanic dumps CPU state and halts, which a host process cannot do
 * and would not want to: a test that reaches it has hit a condition the kernel
 * calls fatal, and the useful outcome is a failed test with the reason on
 * stdout rather than a hung runner. Aborting gives both -- the harness sees a
 * non-zero exit and the message is already printed.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "kpanic.h"

/* Print `reason` with its two context words and abort. `a` and `b` are whatever
 * the panicking call site chose to carry -- their meaning is per call site, not
 * fixed. A NULL reason prints "(null)" rather than faulting. Never returns,
 * matching the real declaration, and the process exits by SIGABRT so the suite
 * fails.
 *
 * Only the message reaches a test. The real kpanic first NMI-IPIs every other
 * CPU and dumps each one's register context and backtrace, none of which a host
 * process has; the concurrent-panic race it arbitrates does not arise here
 * either, so two threads panicking at once interleave their output. */
__attribute__((noreturn)) void kpanic(const char* reason, uint64_t a, uint64_t b) {
    printf("  [KPANIC] %s (a=%llu b=%llu)\n",
           reason ? reason : "(null)",
           (unsigned long long)a,
           (unsigned long long)b);
    fflush(0);
    abort();
}

/* The unlocked serial writer the kernel's diagnostics use.  ipc.c's IPC trace
 * dump reaches it, so a test that links ipc.c needs it defined; a test that
 * never triggers a diagnostic never sees the output.  Printed rather than
 * discarded, because when one does fire during a test its content is the point.
 *
 * WEAK so a harness with its own capturing definition -- test_sched_runqueue
 * asserts on what the scheduler reported -- wins the link instead of colliding
 * with this one. */
__attribute__((weak, format(printf, 1, 2))) void serial_printf_unlocked(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* The locking console writer, which the scheduler's tripwires use -- an
 * interleaved diagnostic corrupts whatever another CPU is printing, so only
 * fault handlers take the unlocked form. There is no lock to take on the host;
 * a test linking sched_thread.c needs the symbol, and a tripwire that fires
 * during a test is worth seeing.
 *
 * WEAK for the same reason as above: test_sched_runqueue captures both writers
 * to assert which one a report came through. */
__attribute__((weak, format(printf, 1, 2))) void serial_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* Discards the interrupted CPU context a caller would record before panicking.
 * There is no per-CPU panic slot to fill on the host and nothing reads one back,
 * so the register values a target dump would show are unavailable to a test. */
void kpanic_capture_origin(uint64_t rip, uint64_t rsp, uint64_t rbp, uint64_t rflags, uint64_t cs) {
    (void)rip;
    (void)rsp;
    (void)rbp;
    (void)rflags;
    (void)cs;
}
