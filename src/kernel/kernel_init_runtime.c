#include "kernel_init_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "process_manager.h"
#include "serial.h"
#include "wasmos_app.h"
#include "string.h"

static const uint8_t g_skip_wasm_boot = 0;

/* Terminal idle wait for init (see the final park in kernel_init_entry).  init
 * has no work left after boot handoff; a bounded park lets the CPU reach
 * idle/hlt while keeping a low-rate heartbeat.  Returning through the no-op
 * process_block_on_ipc() instead would spin the scheduler. */
#define KERNEL_INIT_IDLE_WAIT_MS 100u

/* Puts an init_state_t into its pre-boot state: phase 0, nothing started, no
 * request in flight, and every boot-module index set to the 0xFFFFFFFF "not
 * found" sentinel rather than 0, which is a valid index.
 *
 * boot_info is stored as a BORROWED pointer and must outlive the init process —
 * kmain passes the kernel-owned shadow, not the firmware's original.  A NULL
 * state is ignored.  Every field is written explicitly, so the structure need
 * not be zeroed first. */
void kernel_init_state_reset(init_state_t* state, const boot_info_t* boot_info) {
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
    state->fs_manager_index = 0xFFFFFFFFu;
    state->fs_init_index = 0xFFFFFFFFu;
    state->device_manager_index = 0xFFFFFFFFu;
    state->dm_pid = 0;
}

static uint32_t boot_module_index_by_app_name(const boot_info_t* info, const char* name) {
    if (!info || !name || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0xFFFFFFFFu;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return 0xFFFFFFFFu;
    }
    const uint8_t* mods = (const uint8_t*)info->modules;
    for (uint32_t i = 0; i < info->module_count; ++i) {
        const boot_module_t* mod = (const boot_module_t*)(mods + i * info->module_entry_size);
        if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 || mod->size == 0 ||
            mod->size > 0xFFFFFFFFULL) {
            continue;
        }
        wasmos_app_desc_t desc;
        if (wasmos_app_parse(ptr_cast(uint8_t, mod->base), (uint32_t)mod->size, &desc) != 0) {
            continue;
        }
        if (str_eq_bytes(desc.name, desc.name_len, name)) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static int init_send_spawn_index(process_t* process, init_state_t* state, uint32_t module_index,
                                 uint8_t pending_kind) {
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
    /* Reap these children on exit so one-shot boot tests (native-call-min/smoke,
     * init-smoke) don't linger as zombies.  Harmless for long-lived services:
     * auto-reap only fires once a process becomes a zombie. */
    msg.arg1 = PROC_SPAWN_PATH_FLAG_AUTOREAP;
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

/* Spawn a process by filesystem path. init owns a transfer buffer holding the
 * path (staged at offset 0) that PM borrows/reads while handling the message;
 * the spawn-path protocol carries arg1 = (buffer_id<<12)|path_len. */
static int init_send_spawn_path(process_t* process, init_state_t* state, const char* path) {
    uint32_t proc_ep;
    ipc_message_t msg;
    int send_rc;
    xfer_buffer_owner_t buf = {0};
    uint64_t phys = 0u;
    uint8_t* p = 0;
    uint32_t path_len;

    if (!process || !state || !path) {
        return -1;
    }
    proc_ep = process_manager_endpoint();
    if (proc_ep == IPC_ENDPOINT_NONE) {
        return 1;
    }
    path_len = (uint32_t)strlen(path);
    if (path_len == 0u || path_len > 0xFFFu) {
        return -1;
    }
    if (xfer_buffer_acquire(BUFFER_KIND_TRANSFER, process->context_id, path_len, &buf) !=
        WASMOS_ERR_NONE) {
        return -1;
    }
    phys = xfer_buffer_object_phys(&buf.buffer);
    if (phys == 0u) {
        (void)xfer_buffer_release_owned(&buf);
        return -1;
    }
    p = ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
    memcpy(p, path, path_len);
    msg.type = PROC_IPC_SPAWN_PATH;
    msg.source = state->reply_endpoint;
    msg.destination = proc_ep;
    msg.request_id = state->request_id;
    msg.arg0 = 0; /* no spawn flags */
    msg.arg1 = (buf.buffer.buffer_id << 12) | (path_len & 0xFFFu);
    msg.arg2 = 0; /* args_len */
    msg.arg3 = 0;
    send_rc = ipc_send_from(process->context_id, proc_ep, &msg);
    if (send_rc != IPC_OK) {
        (void)xfer_buffer_release_owned(&buf);
        return -1;
    }
    /* PM reads the path synchronously while handling this message, so the buffer
     * is safe to drop once the spawn reply is observed. init owns the buffer and
     * it is reclaimed by xfer_buffer_drop_context on init exit.
     * TODO(xfer-stage2): track `buf` in init_state and release it on the reply. */
    state->pending_kind = 5;
    state->phase = 4;
    return 0;
}

/* Scheduler entry point for the init process, driving the userspace boot chain
 * as a resumable state machine.
 *
 * arg is the init_state_t prepared by kernel_init_state_reset, borrowed and
 * mutated across dispatches; `phase` is where the sequence stands.  Each call
 * advances at most one step and yields, so the whole boot chain — spawning the
 * process manager, resolving boot modules, spawning fs-manager, fs-init and
 * device-manager, then sysinit by path once a filesystem exists — plays out over
 * many dispatches.  Steps that depend on a service that is not up yet simply
 * yield and retry on the next dispatch.
 *
 * Returns PROCESS_RUN_YIELDED for "call me again", PROCESS_RUN_IDLE for a NULL
 * process, state or boot_info, and PROCESS_RUN_EXITED with a non-zero exit
 * status when the process manager cannot be spawned or a reply arrives with a
 * mismatched request id.
 *
 * Once the chain completes, init has no work left and parks on its reply
 * endpoint with a bounded wait rather than returning PROCESS_RUN_BLOCKED, which
 * would not actually park it.  It stays alive as the parent of the boot
 * processes. */
process_run_result_t kernel_init_entry(process_t* process, void* arg) {
    init_state_t* state = (init_state_t*)arg;
    uint32_t pm_pid = 0;
    ipc_message_t msg;

    if (!process || !state || !state->boot_info) {
        return PROCESS_RUN_IDLE;
    }

    if (!state->started) {
        state->native_min_index =
            boot_module_index_by_app_name(state->boot_info, "native-call-min");
        state->native_smoke_index =
            boot_module_index_by_app_name(state->boot_info, "native-call-smoke");
        state->smoke_index = boot_module_index_by_app_name(state->boot_info, "init-smoke");
        state->fs_manager_index = boot_module_index_by_app_name(state->boot_info, "fs-manager");
        state->fs_init_index = boot_module_index_by_app_name(state->boot_info, "fs-init");
        state->device_manager_index =
            boot_module_index_by_app_name(state->boot_info, "device-manager");
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
        if (process_spawn_as(process->pid, "process-manager", process_manager_entry, 0, &pm_pid) !=
            0) {
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
        if (state->fs_manager_index == 0xFFFFFFFFu) {
            klog_write("[init] fs-manager module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (state->fs_init_index == 0xFFFFFFFFu) {
            klog_write("[init] fs-init module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (state->device_manager_index == 0xFFFFFFFFu) {
            klog_write("[init] device-manager module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
    }

    if (g_skip_wasm_boot) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }

    if (state->phase == 0) {
        /* Every boot app is started as its own process, never run inline here:
         * a WASM app leaves through the proc_exit hostcall rather than by
         * returning, so an app entry called in init's context would terminate
         * init and stop system bring-up. */
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
        if (state->reply_endpoint == IPC_ENDPOINT_NONE) {
            if (ipc_endpoint_create(process->context_id, &state->reply_endpoint) != IPC_OK) {
                klog_write("[init] reply endpoint create failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
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
        } else if (state->fs_manager_index != 0xFFFFFFFFu) {
            trace_write("[init] spawn fs-manager\n");
            if (init_send_spawn_index(process, state, state->fs_manager_index, 4) != 0) {
                klog_write("[init] fs-manager spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        } else if (state->fs_init_index != 0xFFFFFFFFu) {
            trace_write("[init] spawn fs-init\n");
            if (init_send_spawn_index(process, state, state->fs_init_index, 5) != 0) {
                klog_write("[init] fs-init spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        } else {
            trace_write("[init] spawn device-manager\n");
            if (init_send_spawn_index(process, state, state->device_manager_index, 6) != 0) {
                klog_write("[init] device-manager spawn request failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            state->pending_kind = 6;
        }
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 1) {
        int recv_rc = ipc_recv_blocking_for(process->context_id, state->reply_endpoint, &msg);
        /* ipc_recv_blocking_for only returns IPC_EMPTY on spurious wake; caller loops */
        if (recv_rc == IPC_EMPTY) {
            return PROCESS_RUN_YIELDED; /* retry on next dispatch */
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (msg.type == PROC_IPC_ERROR) {
            uint32_t op = msg.arg0;
            uint32_t err = msg.arg1;
            if (op == PROC_IPC_SPAWN && (err == (uint32_t)-1 || err == (uint32_t)-2)) {
                /* PM spawn can transiently fail while slots/services churn
                 * during strict ring3 threading smoke; retry same phase.
                 * FIXME: -1/-2 are bare codes on an IPC boundary, so this
                 * retry is matched by magic number instead of by a packed
                 * WASMOS_ERR_PROC_* from abi/errors.yaml. */
                state->request_id++;
                state->pending_kind = 0;
                state->phase = 0;
                return PROCESS_RUN_YIELDED;
            }
            if (state->pending_kind == 1) {
                klog_write("[init] native-call-min spawn failed\n");
            } else if (state->pending_kind == 2) {
                klog_write("[init] native-call-smoke spawn failed\n");
            } else if (state->pending_kind == 3) {
                klog_write("[init] init-smoke spawn failed\n");
            } else if (state->pending_kind == 4) {
                klog_write("[init] fs-manager spawn failed\n");
            } else if (state->pending_kind == 5) {
                /* FIXME: pending_kind 5 reaching phase 1 is the fs-init spawn
                 * (init_send_spawn_path reuses 5 for sysinit, but that reply is
                 * handled in phase 4), so this line names the wrong app. */
                klog_write("[init] sysinit spawn failed\n");
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
        } else if (state->pending_kind == 4) {
            trace_write("[init] fs-manager spawn ok\n");
            state->fs_manager_index = 0xFFFFFFFFu;
        } else if (state->pending_kind == 5) {
            trace_write("[init] fs-init spawn ok\n");
            state->fs_init_index = 0xFFFFFFFFu;
        } else {
            trace_write("[init] device-manager spawn ok\n");
            state->device_manager_index = 0xFFFFFFFFu;
            state->dm_pid = (uint32_t)msg.arg0;
            /* DM may have already set ready=1 implicitly (first IPC block).
             * Reset it and require explicit notify_ready before init proceeds. */
            process_t* dm = process_get(state->dm_pid);
            if (dm) {
                dm->ready = 0;
                process_set_require_explicit_ready(dm);
            }
            state->request_id++;
            state->pending_kind = 0;
            state->phase = 6;
            return PROCESS_RUN_YIELDED;
        }
        state->request_id++;
        state->pending_kind = 0;
        state->phase = 0;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 6) {
        process_t* dm = state->dm_pid ? process_get(state->dm_pid) : 0;
        if (!dm || !dm->ready) {
            return PROCESS_RUN_YIELDED;
        }
        trace_write("[init] device-manager ready\n");
        /* sysinit is not a boot-volume prerequisite, so it is NOT in the initfs
         * (which holds only what's needed to mount the boot volume). It ships on
         * the ESP and is spawned by path from the "boot" FAT mount once device
         * enumeration has brought fs-fat up; the spawn retries until then. */
        if (init_send_spawn_path(process, state, "/boot/system/services/sysinit.wap") != 0) {
            return PROCESS_RUN_YIELDED;
        }
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 4) {
        int recv_rc = ipc_recv_blocking_for(process->context_id, state->reply_endpoint, &msg);
        /* ipc_recv_blocking_for only returns IPC_EMPTY on spurious wake; caller loops */
        if (recv_rc == IPC_EMPTY) {
            return PROCESS_RUN_YIELDED; /* retry on next dispatch */
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

    /* Boot handoff complete; init has no further work.  Park (bounded) on the
     * reply endpoint so the thread yields the CPU to idle/hlt.  Returning
     * PROCESS_RUN_BLOCKED instead does NOT park: process_block_on_ipc() is a
     * no-op stub, so the thread stays RUNNING and on no wait_list, and the
     * scheduler treats PROCESS_RUN_BLOCKED from a still-RUNNING thread as a
     * voluntary yield and re-dispatches init every scheduling round (see
     * process.c) — a spin that keeps the run queue non-empty, so the idle
     * thread's sti;hlt never runs and the host vCPU sits at full load.  The
     * timeout keeps a low-rate heartbeat. */
    (void)ipc_endpoint_wait_for(process->context_id, state->reply_endpoint,
                                KERNEL_INIT_IDLE_WAIT_MS);
    return PROCESS_RUN_YIELDED;
}
