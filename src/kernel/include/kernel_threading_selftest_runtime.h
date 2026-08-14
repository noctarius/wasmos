/* kernel_threading_selftest_runtime.h - Kernel threading self-test declarations. */
#ifndef WASMOS_KERNEL_THREADING_SELFTEST_RUNTIME_H
#define WASMOS_KERNEL_THREADING_SELFTEST_RUNTIME_H

#include <stdint.h>

/* Spawn the kernel threading self-tests under init_pid: an internal create/exit smoke, a
 * join-ordering smoke, and a thread IPC stress process.  ring3_thread_lifecycle_smoke_enabled
 * is recorded for the ring-3 lifecycle probe those tests hand off to.  All per-test state
 * lives in module globals, so there is one instance per kernel and a second call restarts
 * it.  Returns 0 when every process was spawned, -1 on the first failure.  Pass/fail is
 * reported through the log, not this return value. */
int kernel_threading_selftest_spawn(uint32_t init_pid,
                                    uint8_t ring3_thread_lifecycle_smoke_enabled);

#endif
