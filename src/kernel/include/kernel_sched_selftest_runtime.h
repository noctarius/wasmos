#ifndef WASMOS_KERNEL_SCHED_SELFTEST_RUNTIME_H
#define WASMOS_KERNEL_SCHED_SELFTEST_RUNTIME_H

/*
 * Returns 0 if all scheduler unit tests pass, non-zero on any failure.
 * Safe to call early in kernel init before any process is spawned.
 * No-op (returns 0) when WASMOS_SCHED_THREADABLE is not defined.
 */
int kernel_sched_selftest_run(void);

#endif
