/* kernel_ring3_suite_runtime.h - Aggregated ring-3 test suite entry point. */
#ifndef WASMOS_KERNEL_RING3_SUITE_RUNTIME_H
#define WASMOS_KERNEL_RING3_SUITE_RUNTIME_H

#include <stdint.h>

/* Spawn the whole ring-3 test suite under init_pid: the shmem smoke process, the native
 * probe, optionally the thread-lifecycle probe, every fault probe, and the fault-policy
 * watcher that waits on them for ring3_fault_churn_rounds further spawn/fault cycles.
 * Returns 0 once everything is spawned and -1 as soon as any spawn fails, leaving the
 * processes spawned before that point running.  Spawning only starts the suite; the
 * verdicts are reported through the log as each probe dies. */
int kernel_ring3_spawn_suite(uint32_t init_pid, uint8_t ring3_thread_lifecycle_smoke_enabled,
                             uint8_t ring3_fault_churn_rounds);

#endif
