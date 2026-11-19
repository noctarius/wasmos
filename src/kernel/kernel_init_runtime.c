#include "kernel_init_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "kpanic.h"
#include "memory.h"
#include "paging.h"
#include "process_manager.h"
#include "process_manager_internal.h"
#include "serial.h"
#include "wasmos_app.h"
#include "string.h"
#include "timer.h"
#include "xfer_buffer.h"

static const uint8_t g_skip_wasm_boot = 0;

/* Terminal idle wait for init (see the final park in kernel_init_entry).  init
 * has no work left after boot handoff; a bounded park lets the CPU reach
 * idle/hlt while keeping a low-rate heartbeat.  Returning through the no-op
 * process_block_on_ipc() instead would spin the scheduler. */
#define KERNEL_INIT_IDLE_WAIT_MS 100u

/* How many times init re-asks PM to spawn sysinit by path before giving up.
 *
 * sysinit ships on the ESP rather than in the initfs and is spawned only once
 * fs-manager has reported /boot mounted, so a healthy boot needs exactly one
 * attempt; a retry means the read failed with the volume mounted, and this
 * bound only decides how long the system waits to find out. Before it existed
 * the retry was unbounded, and a boot that never got /boot retried 17,081 times
 * over 14 minutes, logging each one, pegging a CPU and reaching no prompt
 * (docs/TASKS.md `[BUG][P0]`). */
#define INIT_SYSINIT_MAX_ATTEMPTS 512u

/* How long init waits, from device-manager's spawn, for fs-manager to report
 * /boot mounted. Enumeration, the boot volume's driver and its registration
 * all fall inside it; a loaded CI runner is entitled to be slow, so it is
 * generous. Past it the boot panics naming the missing mount, because sysinit
 * -- and so every service -- lives on that volume. */
#define INIT_BOOT_MOUNT_TIMEOUT_MS 60000u
/* The longest init parks between looks at its deadline while it waits. */
#define INIT_WAIT_SLICE_MS 100u
/* init's tag on its "fs.vfs" name subscription (SVC_IPC_NAME_EVENT arg1). */
#define INIT_FSVFS_COOKIE 0x1B007u

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
    state->supervisor_index = 0xFFFFFFFFu;
    state->fs_manager_index = 0xFFFFFFFFu;
    state->fs_tmpfs_index = 0xFFFFFFFFu;
    state->fs_init_index = 0xFFFFFFFFu;
    state->device_manager_index = 0xFFFFFFFFu;
    state->sysinit_attempts = 0;
    state->boot_mount_deadline = 0;
    state->fsmgr_endpoint = IPC_ENDPOINT_NONE;
    memset(&state->name_sub_buf, 0, sizeof(state->name_sub_buf));
    memset(&state->mount_wait_buf, 0, sizeof(state->mount_wait_buf));
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

/* ------------------------------------------------------------ /boot wait */

/* The wait for /boot cannot finish, so neither can the boot: sysinit and every
 * service live on that volume. A named panic, like init_no_sysinit, rather than
 * a kernel parked with nothing on top. */
__attribute__((noreturn)) static void init_boot_mount_failed(const char* why) {
    klog_printf("[init] waiting for /boot: %s\n"
                "[init] sysinit lives on /boot, so this system has no userspace and no "
                "prompt is coming.\n",
                why);
    kpanic("init_no_boot_mount", 0, 0);
}

/* Takes the next message for init's reply endpoint. Returns 1 with *out
 * filled; 0 when nothing came within INIT_WAIT_SLICE_MS, and the caller yields
 * and asks again. Past the /boot deadline it panics. */
static int init_take(process_t* process, init_state_t* state, ipc_message_t* out) {
    if (ipc_recv_for(process->context_id, state->reply_endpoint, out) == IPC_OK) {
        return 1;
    }
    if ((int64_t)(timer_ticks() - state->boot_mount_deadline) >= 0) {
        init_boot_mount_failed("fs-manager did not report /boot mounted in time");
    }
    (void)ipc_endpoint_wait_for(process->context_id, state->reply_endpoint, INIT_WAIT_SLICE_MS);
    return 0;
}

/* Acquires a transfer buffer init owns and returns its kernel mapping, or NULL
 * with nothing held. */
static uint8_t* init_buffer(process_t* process, uint32_t size, xfer_buffer_owner_t* out) {
    uint64_t phys;
    memset(out, 0, sizeof(*out));
    if (xfer_buffer_acquire(BUFFER_KIND_TRANSFER, process->context_id, size, out) !=
        WASMOS_ERR_NONE) {
        return 0;
    }
    phys = xfer_buffer_object_phys(&out->buffer);
    if (phys == 0u) {
        (void)xfer_buffer_release_owned(out);
        return 0;
    }
    return ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
}

static int init_send(process_t* process, uint32_t endpoint, ipc_message_t* msg) {
    msg->destination = endpoint;
    return ipc_send_from(process->context_id, endpoint, msg) == IPC_OK ? 0 : -1;
}

/* SVC_IPC_SUBSCRIBE_NAME_REQ for "fs.vfs", bind events to init's reply
 * endpoint. The PM reads the descriptor from init's buffer while handling it;
 * init releases the buffer on the answer. */
static int init_send_name_subscribe(process_t* process, init_state_t* state) {
    svc_name_subscribe_desc_t* desc;
    ipc_message_t msg;
    uint8_t* p = init_buffer(process, (uint32_t)sizeof(*desc), &state->name_sub_buf);
    if (!p) {
        return -1;
    }
    desc = (svc_name_subscribe_desc_t*)p;
    memset(desc, 0, sizeof(*desc));
    desc->version = WASMOS_SVC_NAME_SUBSCRIBE_DESC_VERSION;
    desc->notify_endpoint = state->reply_endpoint;
    desc->cookie = INIT_FSVFS_COOKIE;
    memcpy(desc->name, "fs.vfs", sizeof("fs.vfs"));
    msg.type = SVC_IPC_SUBSCRIBE_NAME_REQ;
    msg.source = state->reply_endpoint;
    msg.request_id = state->request_id;
    msg.arg0 = state->name_sub_buf.buffer.buffer_id;
    msg.arg1 = 0;
    msg.arg2 = (uint32_t)sizeof(*desc);
    msg.arg3 = 0;
    if (init_send(process, process_manager_endpoint(), &msg) != 0) {
        (void)xfer_buffer_release_owned(&state->name_sub_buf);
        return -1;
    }
    return 0;
}

/* SVC_IPC_LOOKUP_REQ for "fs.vfs": the binding that may predate the
 * subscription. */
static int init_send_lookup_fsvfs(process_t* process, init_state_t* state) {
    uint32_t name[4];
    ipc_message_t msg;
    pm_pack_name_args("fs.vfs", name);
    msg.type = SVC_IPC_LOOKUP_REQ;
    msg.source = state->reply_endpoint;
    msg.request_id = state->request_id;
    msg.arg0 = name[0];
    msg.arg1 = name[1];
    msg.arg2 = name[2];
    msg.arg3 = name[3];
    return init_send(process, process_manager_endpoint(), &msg);
}

/* Drops the "fs.vfs" subscription once fs-manager is found. The answer is not
 * waited for; whatever of it arrives later is a straggler the phases skip. */
static void init_send_name_unsubscribe(process_t* process, init_state_t* state) {
    ipc_message_t msg;
    msg.type = SVC_IPC_UNSUBSCRIBE_NAME_REQ;
    msg.source = state->reply_endpoint;
    msg.request_id = 0;
    msg.arg0 = state->reply_endpoint;
    msg.arg1 = INIT_FSVFS_COOKIE;
    msg.arg2 = 0;
    msg.arg3 = 0;
    (void)init_send(process, process_manager_endpoint(), &msg);
}

/* FSMGR_IPC_MOUNT_WAIT_REQ for `path`, in a buffer init owns and lends READ to
 * fs-manager's context. The reply is deferred until the mount exists; init
 * releases the buffer then, which revokes the loan. */
static int init_send_mount_wait(process_t* process, init_state_t* state, const char* path) {
    wasmos_fsmgr_mount_wait_desc_t* desc;
    xfer_buffer_borrow_t borrow;
    uint32_t fsmgr_context = 0;
    uint32_t len = (uint32_t)strlen(path);
    ipc_message_t msg;
    uint8_t* p;

    if (len == 0u || len > FSMGR_MOUNT_WAIT_PATH_MAX ||
        ipc_endpoint_owner(state->fsmgr_endpoint, &fsmgr_context) != IPC_OK) {
        return -1;
    }
    p = init_buffer(process, (uint32_t)sizeof(*desc), &state->mount_wait_buf);
    if (!p) {
        return -1;
    }
    desc = (wasmos_fsmgr_mount_wait_desc_t*)p;
    memset(desc, 0, sizeof(*desc));
    desc->version = FSMGR_MOUNT_WAIT_DESC_VERSION;
    desc->path_len = len;
    memcpy(desc->path, path, len);
    msg.type = FSMGR_IPC_MOUNT_WAIT_REQ;
    msg.source = state->reply_endpoint;
    msg.request_id = state->request_id;
    msg.arg0 = state->mount_wait_buf.buffer.buffer_id;
    msg.arg1 = 0;
    msg.arg2 = (uint32_t)sizeof(*desc);
    msg.arg3 = 0;
    if (xfer_buffer_borrow(&state->mount_wait_buf, fsmgr_context, BUFFER_BORROW_READ, &borrow) !=
            WASMOS_ERR_NONE ||
        init_send(process, state->fsmgr_endpoint, &msg) != 0) {
        (void)xfer_buffer_release_owned(&state->mount_wait_buf);
        return -1;
    }
    return 0;
}

/* Scheduler entry point for the init process, driving the userspace boot chain
 * as a resumable state machine.
 *
 * arg is the init_state_t prepared by kernel_init_state_reset, borrowed and
 * mutated across dispatches; `phase` is where the sequence stands.  Each call
 * advances at most one step and yields, so the whole boot chain — spawning the
 * process manager, resolving boot modules, spawning the supervisor, fs-manager,
 * fs-tmpfs, fs-init and device-manager, then sysinit by path once fs-manager
 * reports /boot mounted — plays out over many dispatches.  Steps that depend on a service that is
 * not up yet simply yield and retry on the next dispatch.
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
        state->supervisor_index = boot_module_index_by_app_name(state->boot_info, "supervisor");
        state->fs_manager_index = boot_module_index_by_app_name(state->boot_info, "fs-manager");
        state->fs_tmpfs_index = boot_module_index_by_app_name(state->boot_info, "fs-tmpfs");
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
        /* Every service sysinit starts is handed to the supervisor, so a boot
         * without it would bring up no services at all. */
        if (state->supervisor_index == 0xFFFFFFFFu) {
            klog_write("[init] supervisor module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (state->fs_manager_index == 0xFFFFFFFFu) {
            klog_write("[init] fs-manager module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        /* The root filesystem is not optional: with nothing mounted at "/", a
         * path that names no other mount resolves to nothing. */
        if (state->fs_tmpfs_index == 0xFFFFFFFFu) {
            klog_write("[init] fs-tmpfs module not found\n");
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
        } else if (state->supervisor_index != 0xFFFFFFFFu) {
            /* First after the process manager: the supervisor must exist before
             * anything it supervises, and it needs nothing but the PM. */
            trace_write("[init] spawn supervisor\n");
            if (init_send_spawn_index(process, state, state->supervisor_index, 8) != 0) {
                klog_write("[init] supervisor spawn request failed\n");
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
        } else if (state->fs_tmpfs_index != 0xFFFFFFFFu) {
            /* Before fs-init and before device-manager, because a mount point is
             * a directory in THIS filesystem: the root has to be mounted before
             * anything mounts onto it. */
            trace_write("[init] spawn fs-tmpfs\n");
            if (init_send_spawn_index(process, state, state->fs_tmpfs_index, 7) != 0) {
                klog_write("[init] fs-tmpfs spawn request failed\n");
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
            } else if (state->pending_kind == 7) {
                klog_write("[init] fs-tmpfs spawn failed\n");
            } else if (state->pending_kind == 8) {
                klog_write("[init] supervisor spawn failed\n");
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
        } else if (state->pending_kind == 7) {
            trace_write("[init] fs-tmpfs spawn ok\n");
            state->fs_tmpfs_index = 0xFFFFFFFFu;
        } else if (state->pending_kind == 8) {
            trace_write("[init] supervisor spawn ok\n");
            state->supervisor_index = 0xFFFFFFFFu;
            /* Nobody restarts the supervisor and nobody else halts the system
             * when a critical unit dies, so its own death is a panic. */
            process_set_critical((uint32_t)msg.arg0);
        } else if (state->pending_kind == 5) {
            trace_write("[init] fs-init spawn ok\n");
            state->fs_init_index = 0xFFFFFFFFu;
        } else {
            trace_write("[init] device-manager spawn ok\n");
            state->device_manager_index = 0xFFFFFFFFu;
            state->boot_mount_deadline =
                timer_ticks() + timer_ms_to_ticks(INIT_BOOT_MOUNT_TIMEOUT_MS);
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

    /* Phases 6-9 wait for /boot, the volume sysinit lives on. The milestone is
     * fs-manager's: it owns the mount table, and it answers
     * FSMGR_IPC_MOUNT_WAIT_REQ once a backend serving /boot has registered.
     * Device-manager's READY is not that signal -- it says its rules were
     * loaded, not that any volume is mounted. fs-manager is found by its name,
     * subscribed to before it is looked up so a registration in between is not
     * lost (SVC_IPC_SUBSCRIBE_NAME_REQ). */
    if (state->phase == 6) {
        if (init_send_name_subscribe(process, state) != 0) {
            init_boot_mount_failed("cannot subscribe to the name fs.vfs");
        }
        state->phase = 7;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 7) {
        if (!init_take(process, state, &msg)) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id) {
            return PROCESS_RUN_YIELDED;
        }
        (void)xfer_buffer_release_owned(&state->name_sub_buf);
        if (msg.type != SVC_IPC_SUBSCRIBE_NAME_RESP) {
            init_boot_mount_failed("the subscription to the name fs.vfs was refused");
        }
        state->request_id++;
        if (init_send_lookup_fsvfs(process, state) != 0) {
            init_boot_mount_failed("cannot look up fs.vfs");
        }
        state->phase = 8;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 8) {
        uint32_t endpoint = IPC_ENDPOINT_NONE;
        if (!init_take(process, state, &msg)) {
            return PROCESS_RUN_YIELDED;
        }
        /* The lookup's answer and the bind event can come in either order;
         * whichever names fs-manager first decides. */
        if (msg.type == SVC_IPC_LOOKUP_RESP && msg.request_id == state->request_id &&
            msg.arg0 != (uint32_t)-1) {
            endpoint = msg.arg0;
        } else if (msg.type == SVC_IPC_NAME_EVENT && msg.arg1 == INIT_FSVFS_COOKIE) {
            endpoint = msg.arg2;
        }
        if (endpoint == IPC_ENDPOINT_NONE) {
            return PROCESS_RUN_YIELDED;
        }
        state->fsmgr_endpoint = endpoint;
        state->request_id++;
        init_send_name_unsubscribe(process, state);
        if (init_send_mount_wait(process, state, "/boot") != 0) {
            init_boot_mount_failed("cannot ask fs-manager to wait for /boot");
        }
        state->phase = 9;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 9) {
        if (!init_take(process, state, &msg)) {
            return PROCESS_RUN_YIELDED;
        }
        /* Stragglers of phase 8 -- the other of lookup and bind, the
         * unsubscribe's answer -- carry other ids or none. */
        if (msg.request_id != state->request_id ||
            (msg.type != FSMGR_IPC_MOUNT_WAIT_RESP && msg.type != FS_IPC_ERROR)) {
            return PROCESS_RUN_YIELDED;
        }
        (void)xfer_buffer_release_owned(&state->mount_wait_buf);
        if (msg.type != FSMGR_IPC_MOUNT_WAIT_RESP) {
            init_boot_mount_failed("fs-manager refused the wait for /boot");
        }
        klog_write("[init] /boot is mounted; spawning sysinit\n");
        state->request_id++;
        state->phase = 10;
    }

    if (state->phase == 10) {
        /* sysinit is not a boot-volume prerequisite, so it is NOT in the initfs
         * (which holds only what's needed to mount the boot volume). It ships on
         * the ESP and is spawned by path once /boot is mounted.
         *
         * Bounded, because a boot where that read never succeeds used to retry
         * forever: 17,081 attempts over 14 minutes in one CI capture, a log line
         * each, a pegged CPU and no prompt -- `[BUG][P0]`. Giving up turns that
         * silent hang into a named boot failure. */
        if (state->sysinit_attempts >= INIT_SYSINIT_MAX_ATTEMPTS) {
            /* PANIC rather than park. sysinit is this system's pid 1: it reads
             * sysinit.rc and starts every service, so a boot that cannot spawn
             * it has no userspace and never will. Parking would leave a kernel
             * running with nothing on top -- alive enough to look like a hang,
             * dead enough to be useless -- which is the state this whole bug
             * has been hiding in. Linux calls the same thing "Attempted to kill
             * init". */
            klog_printf("[init] cannot spawn /boot/system/services/sysinit.wap after %u "
                        "attempts\n"
                        "[init] sysinit starts every service, so this system has no userspace "
                        "and no prompt is coming.\n"
                        "[init] /boot is mounted but sysinit could not be read: the last "
                        "[pm] spawn_path line above says why.\n",
                        (unsigned)INIT_SYSINIT_MAX_ATTEMPTS);
            kpanic("init_no_sysinit", (uintptr_t)state->sysinit_attempts, 0);
        }
        state->sysinit_attempts++;
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
        /* A straggler of the /boot wait (a lookup answer, a name bind, the
         * unsubscribe's answer) is not the spawn's reply. */
        if (msg.type != PROC_IPC_RESP && msg.type != PROC_IPC_ERROR) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (msg.type == PROC_IPC_ERROR) {
            state->request_id++;
            state->phase = 10;
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
    /* Anything still queued -- a late answer from the /boot wait, a bind of
     * fs.vfs after a restart -- is dropped first: the wait returns at once on a
     * non-empty endpoint, so a message left there would turn the park into a
     * spin. */
    while (ipc_recv_for(process->context_id, state->reply_endpoint, &msg) == IPC_OK) {
    }
    (void)ipc_endpoint_wait_for(
        process->context_id, state->reply_endpoint, KERNEL_INIT_IDLE_WAIT_MS);
    return PROCESS_RUN_YIELDED;
}
