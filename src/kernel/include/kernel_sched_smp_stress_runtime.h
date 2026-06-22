/* kernel_sched_smp_stress_runtime.h - Standalone SMP scheduler stress test.
 *
 * Separate from the always-on boot selftests: only does anything when the
 * kernel is built with -DWASMOS_SCHED_SMP_STRESS. It hammers cross-CPU
 * block/wake/steal/migration with a ring of token-passing worker threads and
 * checks scheduler invariants, so flaky cross-CPU races (RUNNING-orphans, lost
 * wakeups, stranded-ready threads) surface deterministically as a stalled
 * token rather than as an occasional boot hang. Runtime-independent (pure
 * kernel threads), so it gates scheduler changes under both wasm3+SMP and
 * WARP+SMP. */
#ifndef WASMOS_KERNEL_SCHED_SMP_STRESS_RUNTIME_H
#define WASMOS_KERNEL_SCHED_SMP_STRESS_RUNTIME_H

#include <stdint.h>

/* Spawn the stress coordinator under init_pid. No-op (returns 0) unless the
 * kernel was built with WASMOS_SCHED_SMP_STRESS. Returns 0 on success. */
int kernel_sched_smp_stress_spawn(uint32_t init_pid);

#endif /* WASMOS_KERNEL_SCHED_SMP_STRESS_RUNTIME_H */
