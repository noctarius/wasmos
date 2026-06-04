/* kernel_selftest_runtime.h - Kernel self-test entry points invoked from init.
 * Spawn a suite of baseline kernel tests as child processes and wait for their results. */
#ifndef WASMOS_KERNEL_SELFTEST_RUNTIME_H
#define WASMOS_KERNEL_SELFTEST_RUNTIME_H

#include <stdint.h>

/* Spawn baseline kernel tests under init_pid.
 * If preempt_test_enabled, also runs preemption stress tests. Returns 0 on pass. */
int kernel_selftest_spawn_baseline(uint32_t init_pid, uint8_t preempt_test_enabled);

#endif
