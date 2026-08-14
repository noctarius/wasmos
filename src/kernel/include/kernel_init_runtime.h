/* kernel_init_runtime.h - Kernel init process (PID 1) state and entry.
 *
 * The init process drives early system bring-up: spawning fs-manager, fs-init,
 * and device-manager in sequence, then requesting sysinit from /boot.
 * init_state_t is the cooperative state machine persisted across yields. */
#ifndef WASMOS_KERNEL_INIT_RUNTIME_H
#define WASMOS_KERNEL_INIT_RUNTIME_H

#include <stdint.h>
#include "boot.h"
#include "process.h"

/* Cooperative state machine state for the init bring-up sequence. */
typedef struct {
    const boot_info_t* boot_info;
    uint8_t started;
    /* Flags tracking PM test-hook injections (only set in ring3-test mode). */
    uint8_t pm_wait_owner_test_injected;
    uint8_t pm_kill_owner_test_injected;
    uint8_t pm_status_owner_test_injected;
    uint8_t pm_spawn_owner_test_injected;
    uint8_t phase;        /* current bring-up phase index */
    uint8_t pending_kind; /* type of the in-flight spawn request */
    /* Endpoint init receives PM replies on; IPC_ENDPOINT_NONE until it is created. */
    uint32_t reply_endpoint;
    uint32_t request_id; /* next request id; incremented per spawn request */
    /* Boot-module indices of the packages this sequence spawns, looked up by name as each
     * phase is reached.  0xFFFFFFFF means "not resolved / no such module in this image",
     * and the corresponding phase is skipped rather than failed; the index is set back to
     * that value once its spawn has been issued. */
    uint32_t native_min_index;
    uint32_t native_smoke_index;
    uint32_t smoke_index;
    uint32_t fs_manager_index;
    uint32_t fs_init_index;
    uint32_t device_manager_index;
    uint32_t dm_pid; /* pid device-manager was spawned as; 0 before that phase completes */
} init_state_t;

/* Reset *state to the start of the bring-up sequence and bind it to boot_info, which is
 * retained by pointer and must outlive the state.  A NULL state is ignored.  Does not
 * destroy the reply endpoint a previous run created. */
void kernel_init_state_reset(init_state_t* state, const boot_info_t* boot_info);

/* Kernel process entry point for the init state machine.  Advances at most one phase per
 * dispatch and returns PROCESS_RUN_YIELDED to be re-run, or PROCESS_RUN_EXITED with the
 * process's exit status set when a phase fails unrecoverably.  `arg` is the init_state_t
 * the process was spawned with. */
process_run_result_t kernel_init_entry(process_t* process, void* arg);

/* Return non-zero if the ring-3 smoke test suite should run this boot.  Reads a flag the
 * kernel sets during early bring-up, so the answer is stable and the call is cheap. */
uint8_t kernel_ring3_smoke_enabled(void);

#endif
