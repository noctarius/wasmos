/* kernel_selftest_runtime.h - Kernel self-test entry points invoked from init.
 * Spawn a suite of baseline kernel tests as child processes and wait for their results. */
#ifndef WASMOS_KERNEL_SELFTEST_RUNTIME_H
#define WASMOS_KERNEL_SELFTEST_RUNTIME_H

#include <stdint.h>

/* Spawn baseline kernel tests under init_pid: a demand-paging fault test and an IPC
 * wait/send pair, plus a preemption busy/observer pair when preempt_test_enabled is
 * non-zero.  Each is auto-reaped so it does not linger as a zombie.  Returns 0 when every
 * process was spawned and -1 on the first spawn failure — it reports that the tests
 * STARTED, not that they passed; the verdicts appear in the log as they run.  Per-test
 * state lives in module globals, so there is one instance per kernel. */
int kernel_selftest_spawn_baseline(uint32_t init_pid, uint8_t preempt_test_enabled);

#endif
