#ifndef WASMOS_KERNEL_INIT_RUNTIME_H
#define WASMOS_KERNEL_INIT_RUNTIME_H

#include <stdint.h>
#include "boot.h"
#include "process.h"

typedef struct {
    const boot_info_t *boot_info;
    uint8_t started;
    uint8_t pm_wait_owner_test_injected;
    uint8_t pm_kill_owner_test_injected;
    uint8_t pm_status_owner_test_injected;
    uint8_t pm_spawn_owner_test_injected;
    uint8_t phase;
    uint8_t pending_kind;
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t native_min_index;
    uint32_t native_smoke_index;
    uint32_t smoke_index;
    uint32_t fs_manager_index;
    uint32_t fs_init_index;
    uint32_t device_manager_index;
    uint8_t wasm3_probe_done;
} init_state_t;

void kernel_init_state_reset(init_state_t *state, const boot_info_t *boot_info);
process_run_result_t kernel_init_entry(process_t *process, void *arg);
uint8_t kernel_ring3_smoke_enabled(void);

#endif
