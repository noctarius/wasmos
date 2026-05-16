#include "kernel_init_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "process_manager.h"
#include "serial.h"
#include "wasmos_app.h"
#include "wasm3_probe.h"
#include "string.h"

static const uint8_t g_skip_wasm_boot = 0;

void
kernel_init_state_reset(init_state_t *state, const boot_info_t *boot_info)
{
    if (!state) {
        return;
    }
    state->boot_info = boot_info;
    state->started = 0;
    state->pm_wait_owner_test_injected = 0;
    state->pm_kill_owner_test_injected = 0;
    state->pm_status_owner_test_injected = 0;
    state->pm_spawn_owner_test_injected = 0;
    state->phase = 0;
    state->pending_kind = 0;
    state->reply_endpoint = IPC_ENDPOINT_NONE;
    state->request_id = 1;
    state->native_min_index = 0xFFFFFFFFu;
    state->native_smoke_index = 0xFFFFFFFFu;
    state->smoke_index = 0xFFFFFFFFu;
    state->device_manager_index = 0xFFFFFFFFu;
    state->wasm3_probe_done = 0;
}

static uint32_t
boot_module_index_by_app_name(const boot_info_t *info, const char *name)
{
    if (!info || !name || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0xFFFFFFFFu;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return 0xFFFFFFFFu;
    }
    const uint8_t *mods = (const uint8_t *)info->modules;
    for (uint32_t i = 0; i < info->module_count; ++i) {
        const boot_module_t *mod = (const boot_module_t *)(mods + i * info->module_entry_size);
        if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
            mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
            continue;
        }
        wasmos_app_desc_t desc;
        if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
            continue;
        }
        if (str_eq_bytes(desc.name, desc.name_len, name)) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static void
pack_name_args(const char *name, uint32_t out[4])
{
    if (!out) {
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = 0;
    }
    if (!name) {
        return;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; name[i] && idx < 16; ++i, ++idx) {
        uint32_t slot = idx / 4;
        uint32_t shift = (idx % 4) * 8;
        out[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
}

static int
init_send_spawn_index(process_t *process, init_state_t *state, uint32_t module_index, uint8_t pending_kind)
{
    uint32_t proc_ep;
    ipc_message_t msg;
    int send_rc;

    if (!process || !state || module_index == 0xFFFFFFFFu) {
        return -1;
    }
    proc_ep = process_manager_endpoint();
    if (proc_ep == IPC_ENDPOINT_NONE) {
        return 1;
    }
    msg.type = PROC_IPC_SPAWN;
    msg.source = state->reply_endpoint;
    msg.destination = proc_ep;
    msg.request_id = state->request_id;
    msg.arg0 = module_index;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    send_rc = ipc_send_from(process->context_id, proc_ep, &msg);
    if (send_rc != IPC_OK) {
        return -1;
    }
    state->pending_kind = pending_kind;
    state->phase = 1;
    return 0;
}

static int
init_send_spawn_name(process_t *process, init_state_t *state, const char *name)
{
    uint32_t proc_ep;
    uint32_t packed[4];
    ipc_message_t msg;
    int send_rc;

    if (!process || !state || !name) {
        return -1;
    }
    proc_ep = process_manager_endpoint();
    if (proc_ep == IPC_ENDPOINT_NONE) {
        return 1;
    }
    pack_name_args(name, packed);
    msg.type = PROC_IPC_SPAWN_NAME;
    msg.source = state->reply_endpoint;
    msg.destination = proc_ep;
    msg.request_id = state->request_id;
    msg.arg0 = packed[0];
    msg.arg1 = packed[1];
    msg.arg2 = packed[2];
    msg.arg3 = packed[3];
    send_rc = ipc_send_from(process->context_id, proc_ep, &msg);
    if (send_rc != IPC_OK) {
        return -1;
    }
    state->pending_kind = 5;
    state->phase = 4;
    return 0;
}

static int
init_send_fs_probe(process_t *process, init_state_t *state)
{
    uint32_t fs_ep;
    ipc_message_t msg;
    int send_rc;
    static uint8_t logged_fs_wait;

    if (!process || !state) {
        return -1;
    }
    fs_ep = process_manager_fs_endpoint();
    if (fs_ep == IPC_ENDPOINT_NONE) {
        if (!logged_fs_wait) {
            klog_write("[init] waiting for fs service endpoint\n");
            logged_fs_wait = 1;
        }
        return 1;
    }
    logged_fs_wait = 0;
    msg.type = FS_IPC_READY_REQ;
    msg.source = state->reply_endpoint;
    msg.destination = fs_ep;
    msg.request_id = state->request_id;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    send_rc = ipc_send_from(process->context_id, fs_ep, &msg);
    if (send_rc != IPC_OK) {
        return -1;
    }
    state->pending_kind = 6;
    state->phase = 3;
    return 0;
}

static int
init_post_fat_devices_ready(void)
{
    return 1;
}

process_run_result_t
kernel_init_entry(process_t *process, void *arg)
{
    init_state_t *state = (init_state_t *)arg;
    uint32_t pm_pid = 0;
    ipc_message_t msg;

    if (!process || !state || !state->boot_info) {
        return PROCESS_RUN_IDLE;
    }

    if (!state->started) {
        state->native_min_index = boot_module_index_by_app_name(state->boot_info, "native-call-min");
        state->native_smoke_index = boot_module_index_by_app_name(state->boot_info, "native-call-smoke");
        state->smoke_index = boot_module_index_by_app_name(state->boot_info, "init-smoke");
        state->device_manager_index = boot_module_index_by_app_name(state->boot_info, "device-manager");
        state->wasm3_probe_done = 0;
        state->reply_endpoint = IPC_ENDPOINT_NONE;
        state->request_id = 1;
        state->pending_kind = 0;
        state->phase = 0;
        if (g_skip_wasm_boot) {
            trace_write("[init] wasm boot bypass enabled\n");
            state->started = 1;
            return PROCESS_RUN_YIELDED;
        }

        process_manager_init(state->boot_info);
        if (process_spawn_as(process->pid, "process-manager", process_manager_entry, 0, &pm_pid) != 0) {
            klog_write("[init] process manager spawn failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        trace_write("[init] process manager pid=");
        trace_do(serial_write_hex64(pm_pid));
        state->started = 1;
        state->pm_wait_owner_test_injected = 0;
        state->pm_kill_owner_test_injected = 0;
        state->pm_status_owner_test_injected = 0;
        state->pm_spawn_owner_test_injected = 0;
        if (state->device_manager_index == 0xFFFFFFFFu) {
            klog_write("[init] device-manager module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
    }

    if (g_skip_wasm_boot) {
        if (!state->wasm3_probe_done && state->native_min_index != 0xFFFFFFFFu) {
            trace_write("[init] wasm3 probe native-call-min\n");
            int wasm3_rc = wasm3_probe_run(state->boot_info, state->native_min_index);
            (void)wasm3_rc;
            trace_write("[init] wasm3 probe rc=");
            trace_do(serial_write_hex64((uint64_t)(uint32_t)wasm3_rc));
            state->wasm3_probe_done = 1;
        }
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }

    if (state->phase == 0) {
        if (!state->wasm3_probe_done && state->native_min_index != 0xFFFFFFFFu) {
            trace_write("[init] wasm3 probe native-call-min\n");
            int wasm3_rc = wasm3_probe_run(state->boot_info, state->native_min_index);
            (void)wasm3_rc;
            trace_write("[init] wasm3 probe rc=");
            trace_do(serial_write_hex64((uint64_t)(uint32_t)wasm3_rc));
            state->wasm3_probe_done = 1;
        }
        uint32_t proc_ep = process_manager_endpoint();
        if (proc_ep == IPC_ENDPOINT_NONE) {
            return PROCESS_RUN_YIELDED;
        }
        if (kernel_ring3_smoke_enabled() && !state->pm_wait_owner_test_injected) {
            process_manager_inject_wait_owner_mismatch_test(process->context_id);
            state->pm_wait_owner_test_injected = 1;
        }
        if (kernel_ring3_smoke_enabled() && !state->pm_kill_owner_test_injected) {
            process_manager_inject_kill_owner_deny_test();
            state->pm_kill_owner_test_injected = 1;
        }
        if (kernel_ring3_smoke_enabled() && !state->pm_status_owner_test_injected) {
            process_manager_inject_status_owner_deny_test();
            state->pm_status_owner_test_injected = 1;
        }
        if (kernel_ring3_smoke_enabled() && !state->pm_spawn_owner_test_injected) {
            process_manager_inject_spawn_owner_deny_test();
            state->pm_spawn_owner_test_injected = 1;
        }
        if (ipc_endpoint_create(process->context_id, &state->reply_endpoint) != IPC_OK) {
            klog_write("[init] reply endpoint create failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (state->native_min_index != 0xFFFFFFFFu) {
            trace_write("[init] spawn native-call-min\n");
            if (init_send_spawn_index(process, state, state->native_min_index, 1) != 0) {
                klog_write("[init] native-call-min spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        } else if (state->native_smoke_index != 0xFFFFFFFFu) {
            trace_write("[init] spawn native-call-smoke\n");
            if (init_send_spawn_index(process, state, state->native_smoke_index, 2) != 0) {
                klog_write("[init] native-call-smoke spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        } else if (state->smoke_index != 0xFFFFFFFFu) {
            trace_write("[init] spawn init-smoke\n");
            if (init_send_spawn_index(process, state, state->smoke_index, 3) != 0) {
                klog_write("[init] init-smoke spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        } else {
            trace_write("[init] spawn device-manager\n");
            if (init_send_spawn_index(process, state, state->device_manager_index, 4) != 0) {
                klog_write("[init] device-manager spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            state->pending_kind = 4;
        }
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 1) {
        int recv_rc = ipc_recv_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id || msg.type == PROC_IPC_ERROR) {
            if (state->pending_kind == 1) {
                klog_write("[init] native-call-min spawn failed\n");
            } else if (state->pending_kind == 2) {
                klog_write("[init] native-call-smoke spawn failed\n");
            } else if (state->pending_kind == 3) {
                klog_write("[init] init-smoke spawn failed\n");
            } else {
                klog_write("[init] device-manager spawn failed\n");
            }
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (state->pending_kind == 1) {
            trace_write("[init] native-call-min spawn ok\n");
            state->native_min_index = 0xFFFFFFFFu;
        } else if (state->pending_kind == 2) {
            trace_write("[init] native-call-smoke spawn ok\n");
            state->native_smoke_index = 0xFFFFFFFFu;
        } else if (state->pending_kind == 3) {
            trace_write("[init] init-smoke spawn ok\n");
            state->smoke_index = 0xFFFFFFFFu;
        } else {
            trace_write("[init] device-manager spawn ok\n");
            state->device_manager_index = 0xFFFFFFFFu;
            state->request_id++;
            state->pending_kind = 0;
            state->phase = 2;
            return PROCESS_RUN_YIELDED;
        }
        state->request_id++;
        state->pending_kind = 0;
        state->phase = 0;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 2) {
        int rc = init_send_fs_probe(process, state);
        if (rc < 0) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (rc > 0) {
            return PROCESS_RUN_YIELDED;
        }
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 3) {
        int recv_rc = ipc_recv_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (msg.type == FS_IPC_ERROR) {
            state->phase = 2;
            return PROCESS_RUN_YIELDED;
        }
        if (msg.type != FS_IPC_RESP || msg.arg0 != 0) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->request_id++;
        state->phase = 6;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 6) {
        if (!init_post_fat_devices_ready()) {
            return PROCESS_RUN_YIELDED;
        }
        trace_write("[init] post-FAT devices ready\n");
        if (init_send_spawn_name(process, state, "sysinit") != 0) {
            return PROCESS_RUN_YIELDED;
        }
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 4) {
        int recv_rc = ipc_recv_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (msg.type == PROC_IPC_ERROR) {
            state->request_id++;
            state->phase = 6;
            return PROCESS_RUN_YIELDED;
        }
        trace_write("[init] sysinit spawn ok\n");
        state->pending_kind = 0;
        state->phase = 5;
    }

    process_block_on_ipc(process);
    return PROCESS_RUN_BLOCKED;
}
