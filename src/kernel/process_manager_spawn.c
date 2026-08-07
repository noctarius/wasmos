/* process_manager_spawn.c - PM SPAWN request handling.
 * Reads a WASMOS-APP payload from the FS or initfs, parses it, sets up the child
 * process address space, grants IPC endpoints and capabilities, then starts the WASM
 * or native driver.  Responds with PROC_IPC_RESP when the child is ready. */
#include "process_manager_internal.h"
#include "klog.h"
#include "serial.h"
#include "process_manager.h"
#include "capability.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "native_driver.h"
#include "string.h"
#include "timer.h"
#include "wasmos_app_meta.h"
#include "wasmos_exec_format.h"
#include "wasmos_spawn_info.h"
#include "sched.h"
#include "string.h"
#include "serial.h"

/* Scheduler band for a spawned app based on its exec-format flags, wiring up the
 * previously-dead priority scheme (every non-idle process used to run at
 * SCHED_PRIO_WASM).  Drivers get SCHED_PRIO_DRIVER so latency-critical device
 * work -- e.g. the serial driver draining the UART RX FIFO on its IRQ before it
 * overruns -- outranks ordinary apps under load.
 *
 * Services get SCHED_PRIO_SERVICE so they respond to app requests promptly
 * instead of competing in the WASM band; the VT input multiplexer in particular
 * must run to process serial input the driver hands it, or input stalls during a
 * spawn storm.  (This became safe once the perpetual device-manager idle-spin
 * and the font/gfx synchronous-call spins were converted to blocking waits;
 * before that, boosted services hogged CPU and slowed apps.)  Plain apps keep
 * the WASM band. */
static uint8_t pm_sched_prio_for_flags(uint32_t flags) {
    if (flags & WASMOS_APP_FLAG_DRIVER) {
        return (uint8_t)SCHED_PRIO_DRIVER;
    }
    if (flags & WASMOS_APP_FLAG_SERVICE) {
        return (uint8_t)SCHED_PRIO_SERVICE;
    }
    return (uint8_t)SCHED_PRIO_WASM;
}

typedef enum {
    PM_SPAWN_INTERNAL_ERR_BAD_ARGS = -1000,
    PM_SPAWN_INTERNAL_ERR_ALLOC = -1001,
    PM_SPAWN_INTERNAL_ERR_BINDING = -1002,
    PM_SPAWN_INTERNAL_ERR_MISSING_ENDPOINT = -1003,
    PM_SPAWN_INTERNAL_ERR_UNSUPPORTED_KIND = -1004,
    PM_SPAWN_INTERNAL_ERR_BAD_PROCESS = -1005,
    PM_SPAWN_INTERNAL_ERR_CAPS_APPLY = -1006,
    PM_SPAWN_INTERNAL_ERR_BAD_USER_PTR = -1007,
    PM_SPAWN_INTERNAL_ERR_USER_COPY = -1008,
    PM_SPAWN_INTERNAL_ERR_RECV = -1009,
    PM_SPAWN_INTERNAL_ERR_SEND = -1010,
    PM_SPAWN_INTERNAL_ERR_BOUNDS = -1011,
    PM_SPAWN_INTERNAL_ERR_BAD_REPLY = -1012,
    PM_SPAWN_INTERNAL_ERR_EMPTY_PATH = -1013
} pm_spawn_internal_error_t;

/* --- PM's per-operation transfer buffers ----------------------------------
 * PM does NOT keep a single shared buffer. Each operation (one spawn, one FS
 * read) acquires its own BUFFER_KIND_TRANSFER object owned by PM, uses it, and
 * releases it when the operation completes. Concurrent operations therefore get
 * distinct objects and buffer_ids, matching the object model (a context owns as
 * many live buffers as it has live operations). PM hands an operation's
 * buffer_id to a provider (e.g. fs-manager) which borrows it to fill it; PM
 * reads the result out of the object it owns and then releases it. */
static int pm_xfer_acquire(uint32_t pm_context_id, uint32_t minimum_size,
                           xfer_buffer_owner_t* out) {
    if (!out || pm_context_id == 0u) {
        return -1;
    }
    if (minimum_size == 0u) {
        minimum_size = 1u;
    }
    return xfer_buffer_acquire(BUFFER_KIND_TRANSFER, pm_context_id, minimum_size, out) ==
                   WASMOS_ERR_NONE
               ? 0
               : -1;
}

/* Kernel VA of the backing of a PM-owned buffer, or 0 if it is not live. */
static uint8_t* pm_xfer_owner_ptr(const xfer_buffer_owner_t* owner) {
    uint64_t phys = 0u;

    if (!owner) {
        return 0;
    }
    phys = xfer_buffer_object_phys(&owner->buffer);
    if (phys == 0u) {
        return 0;
    }
    return ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
}

/* Release a PM-owned per-operation buffer and mark the binding empty so a
 * double release is a no-op. */
static void pm_xfer_release(xfer_buffer_owner_t* owner) {
    if (owner && owner->buffer.buffer_id != 0u) {
        (void)xfer_buffer_release_owned(owner);
        owner->buffer.buffer_id = 0u;
    }
}

/* Kernel VA of a transfer buffer owned by `owner_context` and named by
 * `buffer_id` (as carried over IPC). PM reads a caller-owned buffer directly
 * (it is kernel); describe validates the object exists and is owned by that
 * context. Returns 0 on any failure; fills *out_size when non-NULL. */
const uint8_t* pm_foreign_xfer_ptr(uint32_t buffer_id, uint32_t owner_context, uint32_t* out_size) {
    xfer_buffer_t desc = {0};
    uint64_t phys = 0u;

    if (xfer_buffer_describe(buffer_id, BUFFER_KIND_TRANSFER, owner_context, &desc) !=
        WASMOS_ERR_NONE) {
        return 0;
    }
    phys = xfer_buffer_object_phys(&desc);
    if (phys == 0u) {
        return 0;
    }
    if (out_size) {
        *out_size = desc.size_bytes;
    }
    return ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
}

static void pm_slot_release_owned_blob(pm_app_state_t* slot) {
    uint8_t* owned = 0;

    if (!slot) {
        return;
    }
    owned = slot->owned_blob_storage;
    if (slot->owned_blob_storage_phys != 0 && slot->owned_blob_storage_pages != 0) {
        pfa_free_pages(slot->owned_blob_storage_phys, (uint64_t)slot->owned_blob_storage_pages);
    }
    if (slot->blob == owned) {
        slot->blob = 0;
    }
    slot->owned_blob_storage = 0;
    slot->owned_blob_storage_phys = 0;
    slot->owned_blob_storage_pages = 0;
}

static void pm_slot_reset(pm_app_state_t* slot) {
    if (!slot) {
        return;
    }
    pm_slot_release_owned_blob(slot);
    slot->in_use = 0;
    slot->pid = 0;
    slot->flags = 0;
    slot->blob = 0;
    slot->blob_size = 0;
    slot->started = 0;
    slot->entry_argc = 0;
    slot->entry_arg0 = 0;
    slot->entry_arg1 = 0;
    slot->entry_arg2 = 0;
    slot->entry_arg3 = 0;
    slot->spawn_cli_args_len = 0;
    memset(slot->spawn_cli_args, 0, sizeof(slot->spawn_cli_args));
    memset(slot->name, 0, sizeof(slot->name));
}

static int pm_slot_alloc_owned_blob(pm_app_state_t* slot, const uint8_t* blob, uint32_t blob_size) {
    const uint64_t page_size = 4096u;
    uint64_t pages = 0;
    uint64_t phys = 0;
    uint8_t* dst = 0;

    if (!slot || !blob || blob_size == 0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }

    pages = ((uint64_t)blob_size + (page_size - 1u)) / page_size;
    phys = pfa_alloc_pages(pages);
    if (phys == 0) {
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }
    dst = ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
    for (uint32_t i = 0; i < blob_size; ++i) {
        dst[i] = blob[i];
    }

    slot->owned_blob_storage = dst;
    slot->owned_blob_storage_phys = phys;
    slot->owned_blob_storage_pages = (uint32_t)pages;
    slot->blob = dst;
    slot->blob_size = blob_size;
    return 0;
}

static const boot_module_t* pm_module_at(uint32_t index) {
    const boot_info_t* info = g_pm.boot_info;
    if (!info || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return 0;
    }
    if (index >= info->module_count) {
        return 0;
    }
    const uint8_t* mods = (const uint8_t*)info->modules;
    return (const boot_module_t*)(mods + index * info->module_entry_size);
}

uint32_t pm_find_module_index_by_name(const char* name) {
    const boot_info_t* info = g_pm.boot_info;
    if (!info || !name || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0xFFFFFFFFu;
    }

    for (uint32_t i = 0; i < info->module_count; ++i) {
        const boot_module_t* mod = pm_module_at(i);
        if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 || mod->size == 0 ||
            mod->size > 0xFFFFFFFFULL) {
            continue;
        }
        wasmos_app_desc_t desc;
        if (wasmos_app_parse(ptr_cast(uint8_t, mod->base), (uint32_t)mod->size, &desc) != 0) {
            continue;
        }
        char temp[64];
        if (str_copy_bytes(temp, sizeof(temp), desc.name, desc.name_len) != 0) {
            continue;
        }
        if (strcmp(temp, name) == 0) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static pm_app_state_t* pm_find_app_slot(void) {
    list_iter_t it;
    pm_app_state_t* slot = (pm_app_state_t*)list_first(&g_pm.apps, &it);
    while (slot) {
        if (!slot->in_use) {
            return slot;
        }
        slot = (pm_app_state_t*)list_next(&it);
    }
    return (pm_app_state_t*)list_alloc(&g_pm.apps);
}

/* The packed-arg spawn opcodes each describe exactly one I/O window; the
 * descriptor path (pm_handle_spawn_caps_v2) may carry several. Centralised so
 * the "no io.port capability means no windows at all" rule cannot drift between
 * the five call sites. Returns 0, or BAD_CAPS for an inverted window. */
static int pm_caps_set_io_window(pm_spawn_caps_t* caps, uint16_t first, uint16_t last) {
    caps->io_range_count = 0;
    if ((caps->cap_flags & DEVMGR_CAP_IO_PORT) == 0) {
        return 0;
    }
    if (first > last) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    caps->io_ranges[0].first = first;
    caps->io_ranges[0].last = last;
    caps->io_range_count = 1;
    return 0;
}

/* The legacy per-app entry-arg binding mechanism (proc.endpoint / module.count /
 * cli.tty.alloc / block.endpoint / ...) has been retired in favour of the
 * spawn-info contract (see wasmos_spawn_info.h + pm_app_entry). Every startup
 * value the child needs now travels in its spawn-info buffer, and service
 * endpoints are resolved via svc_lookup. The 4-slot wasm entry signature is
 * kept but always receives zeros. */
static int pm_apply_entry_bindings(pm_app_state_t* slot, const wasmos_app_desc_t* desc) {
    if (!slot || !desc) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    slot->entry_argc = 4;
    slot->entry_arg0 = 0;
    slot->entry_arg1 = 0;
    slot->entry_arg2 = 0;
    slot->entry_arg3 = 0;
    return 0;
}

static int pm_apply_post_spawn_bindings(pm_app_state_t* slot, uint32_t pid) {
    (void)pid;
    (void)slot;
    return 0;
}

static process_run_result_t pm_app_entry(process_t* process, void* arg) {
    pm_app_state_t* state = (pm_app_state_t*)arg;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_disable();
#endif

    if (!state->started) {
        wasmos_app_desc_t desc;
        if (wasmos_app_parse(state->blob, state->blob_size, &desc) != 0) {
            klog_write("[pm] app parse failed\n");
            process_set_exit_status(process, -1);
            pm_slot_reset(state);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        state->flags = desc.flags;
        /* Build the spawn-info contract buffer (child-owned): a
         * wasmos_spawn_info_t header immediately followed by the args blob. The
         * child retrieves its buffer_id via the wasmos_spawn_info_buffer()
         * hostcall (WASM) or api->spawn_info() (native) and reads proc.endpoint,
         * its controlling TTY, boot-module info, and argv from it. The buffer is
         * owned by the child context, so child exit reclaims it via
         * xfer_buffer_drop_context. */

        xfer_buffer_owner_t si_xfer = {0};
        uint32_t args_len = state->spawn_cli_args_len;
        uint32_t need = (uint32_t)sizeof(wasmos_spawn_info_t) + args_len + 1u;
        uint64_t si_phys = 0;
        uint8_t* si_buf = 0;
        wasmos_spawn_info_t* si = 0;
        uint8_t* args_dst = 0;
        if (xfer_buffer_acquire(BUFFER_KIND_TRANSFER, process->context_id, need, &si_xfer) !=
                WASMOS_ERR_NONE ||
            (si_phys = xfer_buffer_object_phys(&si_xfer.buffer)) == 0u) {
            klog_write("[pm] spawn info alloc failed\n");
            process_set_exit_status(process, -1);
            pm_slot_reset(state);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        si_buf = ptr_cast(uint8_t, (si_phys | KERNEL_HIGHER_HALF_BASE));
        si = (wasmos_spawn_info_t*)si_buf;
        si->magic = WASMOS_SPAWN_INFO_MAGIC;
        si->version = WASMOS_SPAWN_INFO_VERSION;
        si->header_size = (uint32_t)sizeof(wasmos_spawn_info_t);
        si->proc_endpoint = g_pm.proc_endpoint;
        si->tty = (desc.flags & WASMOS_APP_FLAG_WANTS_TTY) ? pm_alloc_cli_tty() : 0u;
        si->module_count = g_pm.module_count;
        si->module_index = g_pm.init_module_index;
        si->args_off = (uint32_t)sizeof(wasmos_spawn_info_t);
        si->args_len = args_len;
        args_dst = si_buf + sizeof(wasmos_spawn_info_t);
        for (uint32_t i = 0; i < args_len; ++i) {
            args_dst[i] = (uint8_t)state->spawn_cli_args[i];
        }
        args_dst[args_len] = '\0';
        process->spawn_info_buffer_id = si_xfer.buffer.buffer_id;

        /* The 4-slot entry-arg calling convention is retired: the child pulls
         * everything from its spawn-info buffer. Pass zeros to satisfy the
         * (unchanged) wasm entry signature. */
        uint32_t init_args[4] = {0u, 0u, 0u, 0u};

#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
        /* Drain pdc to 0 before any long-running module start (native driver
         * initialisation, WARP JIT compilation, AOT loading).  Either path can
         * take seconds; holding pdc=1 through it accumulates watchdog stall ticks
         * until the 512-tick threshold fires.  The paired preempt_enable() at
         * every exit path becomes a safe no-op when pdc is already 0. */
        while (preempt_disable_depth() > 0)
            preempt_enable();
        process_clear_resched();
#endif

        if (wasmos_app_start(&state->app, &desc, process->context_id, init_args,
                             state->entry_argc) != 0) {
            klog_write("[pm] app start failed\n");
            process_set_exit_status(process, -1);
            pm_slot_reset(state);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        state->started = 1;
    }

    int entry_rc = wasmos_app_call_entry(&state->app);
    if (entry_rc != 0) {
        klog_write("[pm] app entry failed\n");
        klog_write("[pm] app entry rc=");
        serial_write_hex64((uint64_t)(int64_t)entry_rc);
        klog_write("\n");
        process_set_exit_status(process, -1);
    } else {
        process_set_exit_status(process, 0);
    }

    wasmos_app_stop(&state->app);
    pm_slot_reset(state);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_enable();
#endif
    return PROCESS_RUN_EXITED;
}

static int pm_spawn_module(uint32_t parent_pid, uint32_t module_index, uint32_t* out_pid) {
    const boot_module_t* mod = pm_module_at(module_index);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 || mod->size == 0 ||
        mod->size > 0xFFFFFFFFULL) {
        klog_write("[pm] spawn_module invalid module\n");
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }

    pm_app_state_t* slot = pm_find_app_slot();
    if (!slot) {
        klog_write("[pm] spawn_module no slot\n");
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }
    pm_slot_reset(slot);

    wasmos_app_desc_t desc;
    wasmos_app_subsystem_info_t subsystem;
    uint8_t require_explicit_ready = 0;
    if (wasmos_app_parse(ptr_cast(uint8_t, mod->base), (uint32_t)mod->size, &desc) != 0) {
        klog_write("[pm] spawn_module parse failed\n");
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    if (wasmos_app_resolve_subsystem(&desc, &subsystem) != 0) {
        klog_write("[pm] spawn_module subsystem resolve failed\n");
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }

    int ready_rc = wasmos_app_requires_explicit_ready(&desc);
    if (ready_rc < 0) {
        klog_write("[pm] spawn_module ready policy failed\n");
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }
    require_explicit_ready = ready_rc ? 1u : 0u;

    if (str_copy_bytes(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        klog_write("[pm] spawn_module name copy failed\n");
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }

    slot->blob = ptr_cast(uint8_t, mod->base);
    slot->blob_size = (uint32_t)mod->size;
    slot->started = 0;
    slot->in_use = 1;
    if (pm_apply_entry_bindings(slot, &desc) != 0) {
        klog_write("[pm] spawn_module bindings failed: ");
        klog_write(slot->name);
        klog_write("\n");
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }

    preempt_disable();
    if ((require_explicit_ready
             ? process_spawn_as_ready_gated_parked(parent_pid, slot->name, pm_app_entry, slot,
                                                   out_pid)
             : process_spawn_as_parked(parent_pid, slot->name, pm_app_entry, slot, out_pid)) != 0) {
        klog_write("[pm] spawn_module process spawn failed: ");
        klog_write(slot->name);
        klog_write("\n");
        preempt_enable();
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }

    slot->pid = *out_pid;
    if (process_set_runtime_lock_required(*out_pid, subsystem.needs_runtime_lock) != 0 ||
        process_set_runtime_tag(*out_pid, subsystem.runtime_tag) != 0) {
        klog_write("[pm] spawn_module runtime flag failed: ");
        klog_write(slot->name);
        klog_write("\n");
        preempt_enable();
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_BAD_PROCESS;
    }
    /* Child is still parked (not enqueued): safe to set its scheduler band. */
    (void)process_set_main_prio(*out_pid, pm_sched_prio_for_flags(desc.flags));
    if (pm_apply_post_spawn_bindings(slot, *out_pid) != 0) {
        klog_write("[pm] spawn_module post bindings failed: ");
        klog_write(slot->name);
        klog_write("\n");
        preempt_enable();
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }
    preempt_enable();
    /* Process is parked: caller must call process_unpark_pid() after all
     * post-spawn setup (capabilities, CWD, etc.) is complete. */
    return 0;
}

static int pm_apply_spawn_caps(uint32_t pid, const pm_spawn_caps_t* caps) {
    process_t* proc = 0;
    if (!caps || !caps->valid) {
        return 0;
    }
    proc = process_get(pid);
    if (!proc || proc->context_id == 0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_PROCESS;
    }
    if (capability_set_spawn_profile(proc->context_id, caps->cap_flags, caps->io_range_count,
                                     caps->io_ranges, caps->irq_mask, caps->dma_direction_flags,
                                     caps->dma_max_bytes, caps->dma_window_count,
                                     caps->dma_windows) != 0) {
        return PM_SPAWN_INTERNAL_ERR_CAPS_APPLY;
    }
    return 0;
}

static int pm_spawn_from_buffer(uint32_t parent_pid, const uint8_t* blob, uint32_t blob_size,
                                const char* spawn_cli_args, uint32_t spawn_cli_args_len,
                                uint32_t* out_pid) {
    if (!blob || blob_size == 0 || !out_pid) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    pm_app_state_t* slot = pm_find_app_slot();
    if (!slot) {
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }
    pm_slot_reset(slot);

    wasmos_app_desc_t desc;
    wasmos_app_subsystem_info_t subsystem;
    uint8_t require_explicit_ready = 0;
    if (wasmos_app_parse(blob, blob_size, &desc) != 0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    if (wasmos_app_resolve_subsystem(&desc, &subsystem) != 0) {
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }
    if ((desc.flags & (WASMOS_APP_FLAG_APP | WASMOS_APP_FLAG_SERVICE | WASMOS_APP_FLAG_DRIVER)) ==
        0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }

    int ready_rc = wasmos_app_requires_explicit_ready(&desc);
    if (ready_rc < 0) {
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }
    require_explicit_ready = ready_rc ? 1u : 0u;

    if (str_copy_bytes(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    if (pm_slot_alloc_owned_blob(slot, blob, blob_size) != 0) {
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }

    slot->started = 0;
    slot->in_use = 1;
    slot->spawn_cli_args_len = 0;
    if (spawn_cli_args && spawn_cli_args_len > 0u) {
        if (spawn_cli_args_len >= sizeof(slot->spawn_cli_args)) {
            pm_slot_reset(slot);
            return PM_SPAWN_INTERNAL_ERR_BOUNDS;
        }
        for (uint32_t i = 0; i < spawn_cli_args_len; ++i) {
            slot->spawn_cli_args[i] = spawn_cli_args[i];
        }
        slot->spawn_cli_args[spawn_cli_args_len] = '\0';
        slot->spawn_cli_args_len = spawn_cli_args_len;
    }
    if (pm_apply_entry_bindings(slot, &desc) != 0) {
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }

    if ((require_explicit_ready
             ? process_spawn_as_ready_gated_parked(parent_pid, slot->name, pm_app_entry, slot,
                                                   out_pid)
             : process_spawn_as_parked(parent_pid, slot->name, pm_app_entry, slot, out_pid)) != 0) {
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }
    slot->pid = *out_pid;
    if (process_set_runtime_lock_required(*out_pid, subsystem.needs_runtime_lock) != 0 ||
        process_set_runtime_tag(*out_pid, subsystem.runtime_tag) != 0) {
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_BAD_PROCESS;
    }
    /* Child is still parked (not enqueued): safe to set its scheduler band. */
    (void)process_set_main_prio(*out_pid, pm_sched_prio_for_flags(desc.flags));
    if (pm_apply_post_spawn_bindings(slot, *out_pid) != 0) {
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_BINDING;
    }
    /* Process is parked: caller must call process_unpark_pid() after all
     * post-spawn setup (capabilities, CWD, etc.) is complete. */
    return 0;
}

static int pm_recv_fs_reply(uint32_t source_context_id, uint32_t source_endpoint,
                            uint32_t request_id, ipc_message_t* out_msg) {
    ipc_message_t msg;
    for (;;) {
        int rc = ipc_recv_blocking_for(source_context_id, source_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            continue; /* spurious wake; retry */
        }
        if (rc != IPC_OK) {
            return PM_SPAWN_INTERNAL_ERR_RECV;
        }
        if (msg.request_id != request_id) {
            continue;
        }
        if (out_msg) {
            *out_msg = msg;
        }
        return 0;
    }
}

static int pm_recv_reply_matching(uint32_t source_context_id, uint32_t source_endpoint,
                                  uint32_t request_id, uint32_t expected_source,
                                  ipc_message_t* out_msg) {
    ipc_message_t msg;

    for (;;) {
        int rc = ipc_recv_blocking_for(source_context_id, source_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            continue;
        }
        if (rc != IPC_OK) {
            return PM_SPAWN_INTERNAL_ERR_RECV;
        }
        if (msg.request_id != request_id || msg.source != expected_source) {
            continue;
        }
        if (out_msg) {
            *out_msg = msg;
        }
        return 0;
    }
}

static int pm_exec_tail_write(uint8_t* buffer, uint32_t buffer_size, uint32_t* io_offset,
                              const void* src, uint32_t len, uint32_t* out_offset, int append_nul) {
    uint32_t offset = 0u;
    const uint8_t* src_bytes = (const uint8_t*)src;

    if (!buffer || !io_offset || (!src && len != 0u)) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    offset = *io_offset;
    if (len > 0u) {
        if (offset >= buffer_size || len > (buffer_size - offset)) {
            return PM_SPAWN_INTERNAL_ERR_BOUNDS;
        }
        for (uint32_t i = 0; i < len; ++i) {
            buffer[offset + i] = src_bytes[i];
        }
        if (out_offset) {
            *out_offset = offset;
        }
        offset += len;
    } else if (out_offset) {
        *out_offset = 0u;
    }
    if (append_nul) {
        if (offset >= buffer_size) {
            return PM_SPAWN_INTERNAL_ERR_BOUNDS;
        }
        buffer[offset++] = '\0';
    }
    *io_offset = offset;
    return 0;
}

static int pm_request_broker_spawn_plan(uint32_t pm_context_id, const xfer_buffer_owner_t* pmbuf,
                                        const wasmos_exec_handler_registry_entry_t* handler,
                                        const char* path, uint32_t path_len, const char* cli_args,
                                        uint32_t args_len, uint32_t spawn_flags, uint32_t blob_size,
                                        wasmos_exec_broker_plan_t* out_plan) {
    uint32_t broker_context_id = 0u;
    uint8_t* pm_fs_buf = 0;
    uint32_t pm_fs_buf_size = 0u;
    uint32_t request_offset = 0u;
    uint32_t request_size = sizeof(wasmos_broker_spawn_plan_request_t);
    uint32_t tail_offset = 0u;
    uint32_t handler_name_offset = 0u;
    uint32_t path_offset = 0u;
    uint32_t args_offset = 0u;
    uint32_t request_id = 0u;
    int broker_has_borrow = 0;
    xfer_buffer_borrow_t broker_borrow = {0};
    ipc_message_t req;
    ipc_message_t reply;
    wasmos_broker_spawn_plan_request_t request;
    const uint8_t* broker_fs_buf = 0;

    if (!handler || !path || path_len == 0u || !out_plan) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }
    if (g_pm.broker_reply_endpoint == IPC_ENDPOINT_NONE ||
        handler->broker_endpoint == IPC_ENDPOINT_NONE ||
        ipc_endpoint_owner(handler->broker_endpoint, &broker_context_id) != IPC_OK ||
        broker_context_id == 0u) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_IPC;
    }
    pm_fs_buf = pm_xfer_owner_ptr(pmbuf);
    pm_fs_buf_size = xfer_buffer_size(BUFFER_KIND_TRANSFER);
    if (!pm_fs_buf || blob_size > pm_fs_buf_size || request_size > (pm_fs_buf_size - blob_size)) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }

    request_offset = blob_size;
    tail_offset = request_offset + request_size;
    if (pm_exec_tail_write(pm_fs_buf, pm_fs_buf_size, &tail_offset, handler->handler_name,
                           (uint32_t)strlen(handler->handler_name), &handler_name_offset, 1) != 0 ||
        pm_exec_tail_write(pm_fs_buf, pm_fs_buf_size, &tail_offset, path, path_len, &path_offset,
                           1) != 0 ||
        pm_exec_tail_write(pm_fs_buf, pm_fs_buf_size, &tail_offset, cli_args, args_len,
                           &args_offset, args_len > 0u ? 1 : 0) != 0) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }

    memset(&request, 0, sizeof(request));
    request.version = WASMOS_BROKER_SPAWN_PLAN_VERSION;
    request.spawn_flags = spawn_flags;
    request.blob_offset = 0u;
    request.blob_size = blob_size;
    request.path_offset = path_offset;
    request.path_len = path_len;
    request.args_offset = args_offset;
    request.args_len = args_len;
    request.handler_name_offset = handler_name_offset;
    request.handler_name_len = (uint32_t)strlen(handler->handler_name);
    memcpy(request.request_tag, handler->request_tag, sizeof(request.request_tag));
    memcpy(request.runtime_tag, handler->runtime_tag, sizeof(request.runtime_tag));
    memcpy(request.broker_name, handler->broker_name, sizeof(request.broker_name));
    memcpy(pm_fs_buf + request_offset, &request, sizeof(request));
    /* Object-model broker contract: PM defines the buffer's lifecycle (it is
     * the consumer of the plan and the only side that can know when the buffer
     * is free), so PM owns it and lends the broker one R|W borrow: READ for the
     * request payload PM staged, WRITE for the plan the broker returns into the
     * same buffer. buffer_id is carried in arg2 so the broker can resolve it.
     * PM revokes the borrow once it has read the plan. */
    if (xfer_buffer_borrow(pmbuf, broker_context_id, BUFFER_BORROW_READ | BUFFER_BORROW_WRITE,
                           &broker_borrow) != WASMOS_ERR_NONE) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_IPC;
    }
    broker_has_borrow = 1;

    request_id = g_pm.fs_request_id++;
    req.type = PROC_BROKER_IPC_SPAWN_PLAN_REQ;
    req.source = g_pm.broker_reply_endpoint;
    req.destination = handler->broker_endpoint;
    req.request_id = request_id;
    req.arg0 = request_offset;
    req.arg1 = tail_offset - request_offset;
    req.arg2 = pmbuf->buffer.buffer_id;
    req.arg3 = 0u;
    if (ipc_send_from(pm_context_id, handler->broker_endpoint, &req) != IPC_OK) {
        klog_write("[dbg-pm-broker] send failed\n");
        goto broker_ipc_fail;
    }
    if (pm_recv_reply_matching(pm_context_id, g_pm.broker_reply_endpoint, request_id,
                               handler->broker_endpoint, &reply) != 0) {
        klog_write("[dbg-pm-broker] recv failed\n");
        goto broker_ipc_fail;
    }
    if (reply.type != PROC_BROKER_IPC_SPAWN_PLAN_RESP) {
        klog_write("[dbg-pm-broker] unexpected reply type=");
        serial_write_hex64((uint64_t)reply.type);
        klog_write(" arg0=");
        serial_write_hex64((uint64_t)reply.arg0);
        klog_write(" arg1=");
        serial_write_hex64((uint64_t)reply.arg1);
        klog_write("\n");
    }
    if (reply.type != PROC_BROKER_IPC_SPAWN_PLAN_RESP || reply.arg1 == 0u ||
        reply.arg0 >= pm_fs_buf_size || reply.arg1 > (pm_fs_buf_size - reply.arg0)) {
        klog_write("[dbg-pm-broker] reply envelope invalid\n");
        goto broker_plan_fail;
    }
    /* The broker wrote its plan into PM's owned buffer (reply.arg0 = offset,
     * reply.arg1 = size); PM reads it back out of the buffer it owns. */
    broker_fs_buf = (const uint8_t*)pm_xfer_owner_ptr(pmbuf);
    if (!broker_fs_buf || wasmos_exec_broker_plan_validate(broker_fs_buf + reply.arg0, reply.arg1,
                                                           handler, out_plan) != 0) {
        klog_write("[dbg-pm-broker] plan validate failed\n");
        goto broker_plan_fail;
    }

    if (broker_has_borrow) {
        (void)xfer_buffer_unborrow(&broker_borrow);
    }
    return 0;

broker_ipc_fail:
    if (broker_has_borrow) {
        (void)xfer_buffer_unborrow(&broker_borrow);
    }
    return WASMOS_ERR_PROC_SPAWN_BROKER_IPC;

broker_plan_fail:
    if (broker_has_borrow) {
        (void)xfer_buffer_unborrow(&broker_borrow);
    }
    return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
}

static int pm_fs_read_blob_for_spawn(uint32_t pm_context_id, const xfer_buffer_owner_t* pmbuf,
                                     const char* path, uint32_t path_len, uint32_t* out_blob_size);

typedef struct {
    char path[256];
    char args[256];
    uint32_t path_len;
    uint32_t args_len;
    uint32_t blob_size;
} pm_resolved_spawn_path_t;

/* Resolves a spawn path into a ready-to-parse blob in the caller-provided,
 * PM-owned per-operation buffer `pmbuf` (acquired and released by the handler).
 * The blob stays live in *pmbuf on success for the handler to spawn from. */
static int pm_resolve_spawn_path(uint32_t pm_context_id, const xfer_buffer_owner_t* pmbuf,
                                 const char* path, uint32_t path_len, const char* cli_args,
                                 uint32_t args_len, uint32_t spawn_flags,
                                 pm_resolved_spawn_path_t* out_resolved) {
    const uint8_t* pm_fs_buf = 0;
    wasmos_exec_format_match_t format_match;
    wasmos_exec_broker_plan_t broker_plan;
    int broker_rc = 0;

    if (!path || !out_resolved || path_len == 0u || path_len >= sizeof(out_resolved->path)) {
        return WASMOS_ERR_PROC_SPAWN_BAD_PATH;
    }
    memset(out_resolved, 0, sizeof(*out_resolved));
    memcpy(out_resolved->path, path, path_len);
    out_resolved->path[path_len] = '\0';
    out_resolved->path_len = path_len;
    if (args_len > 0u) {
        if (!cli_args || args_len >= sizeof(out_resolved->args)) {
            return WASMOS_ERR_PROC_SPAWN_ARGS_TOOBIG;
        }
        memcpy(out_resolved->args, cli_args, args_len);
        out_resolved->args[args_len] = '\0';
        out_resolved->args_len = args_len;
    }

    pm_fs_buf = (const uint8_t*)pm_xfer_owner_ptr(pmbuf);
    if (!pm_fs_buf) {
        return WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF;
    }
    if (pm_fs_read_blob_for_spawn(pm_context_id, pmbuf, out_resolved->path, out_resolved->path_len,
                                  &out_resolved->blob_size) != 0) {
        return WASMOS_ERR_PROC_SPAWN_FS_READ;
    }
    if (wasmos_exec_format_classify(out_resolved->path, pm_fs_buf, out_resolved->blob_size,
                                    &format_match) != 0) {
        return WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
    }
    if (format_match.kind != WASMOS_EXEC_FORMAT_BROKER) {
        return 0;
    }
    broker_rc = pm_request_broker_spawn_plan(
        pm_context_id, pmbuf, format_match.handler, out_resolved->path, out_resolved->path_len,
        out_resolved->args_len > 0u ? out_resolved->args : 0, out_resolved->args_len, spawn_flags,
        out_resolved->blob_size, &broker_plan);
    if (broker_rc != 0) {
        return broker_rc;
    }
    if (!broker_plan.host_path || broker_plan.host_path_len == 0u ||
        broker_plan.host_path_len >= sizeof(out_resolved->path) ||
        broker_plan.host_args_len >= sizeof(out_resolved->args)) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }
    memset(out_resolved->path, 0, sizeof(out_resolved->path));
    memcpy(out_resolved->path, broker_plan.host_path, broker_plan.host_path_len);
    out_resolved->path[broker_plan.host_path_len] = '\0';
    out_resolved->path_len = broker_plan.host_path_len;
    memset(out_resolved->args, 0, sizeof(out_resolved->args));
    /* Broker plans describe the final host invocation; once delegation
     * happens, broker-supplied host args replace the original guest argv
     * string rather than being concatenated implicitly in PM. */
    if (broker_plan.host_args_len > 0u) {
        memcpy(out_resolved->args, broker_plan.host_args, broker_plan.host_args_len);
        out_resolved->args[broker_plan.host_args_len] = '\0';
    }
    out_resolved->args_len = broker_plan.host_args_len;

    if (pm_fs_read_blob_for_spawn(pm_context_id, pmbuf, out_resolved->path, out_resolved->path_len,
                                  &out_resolved->blob_size) != 0) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }
    if (wasmos_exec_format_classify(out_resolved->path, pm_fs_buf, out_resolved->blob_size,
                                    &format_match) != 0 ||
        format_match.kind != WASMOS_EXEC_FORMAT_WAP) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }
    return 0;
}

static int pm_inherit_child_cwd(uint32_t pm_context_id, uint32_t parent_context_id,
                                uint32_t child_pid) {
    process_t* child = 0;
    ipc_message_t req;
    uint32_t req_id = 0;

    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || g_pm.fs_ctrl_endpoint == IPC_ENDPOINT_NONE ||
        parent_context_id == 0 || child_pid == 0) {
        return 0;
    }
    child = process_get(child_pid);
    if (!child || child->context_id == 0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_PROCESS;
    }

    req_id = g_pm.fs_request_id++;
    req.type = FSMGR_IPC_CLONE_CWD_REQ;
    req.source = g_pm.fs_ctrl_endpoint;
    req.destination = g_pm.fs_endpoint;
    req.request_id = req_id;
    req.arg0 = parent_context_id;
    req.arg1 = child->context_id;
    req.arg2 = 0;
    req.arg3 = 0;
    if (ipc_send_from(pm_context_id, g_pm.fs_endpoint, &req) != IPC_OK) {
        return PM_SPAWN_INTERNAL_ERR_SEND;
    }
    return 0;
}

static int pm_fs_read_blob_for_spawn(uint32_t pm_context_id, const xfer_buffer_owner_t* pmbuf,
                                     const char* path, uint32_t path_len, uint32_t* out_blob_size) {
    uint8_t* pm_fs_buf = 0;
    uint32_t max = 0;
    uint32_t req_id = 0;
    ipc_message_t reply;

    if (!path || path_len == 0 || !out_blob_size) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    pm_fs_buf = pm_xfer_owner_ptr(pmbuf);
    max = xfer_buffer_size(BUFFER_KIND_TRANSFER);
    if (!pm_fs_buf || max == 0 || path_len >= max) {
        return PM_SPAWN_INTERNAL_ERR_BOUNDS;
    }
    for (uint32_t i = 0; i < path_len; ++i) {
        pm_fs_buf[i] = (uint8_t)path[i];
    }
    if (pm_fs_buf[0] == '\0') {
        klog_write("[pm] spawn_path path empty\n");
        return PM_SPAWN_INTERNAL_ERR_EMPTY_PATH;
    }

    /* READ_PATH request: arg0 = path length (path bytes are already at offset 0
     * of PM's buffer), arg1 = client buffer capacity (0 = full transfer size),
     * arg2 = PM's buffer_id, arg3 = the grant handle. PM owns the buffer and,
     * owner-push, grants fs-manager R|W over it (fs-manager reborrows to the
     * backend, which writes the blob back, then unborrows so PM can release).
     * PM's buffer is full transfer size, so arg1 = 0 lets the backend fill it. */
    uint32_t fsmgr_ctx = 0;
    xfer_buffer_borrow_t fs_grant;
    if (ipc_endpoint_owner(g_pm.fs_endpoint, &fsmgr_ctx) != IPC_OK || fsmgr_ctx == 0) {
        return PM_SPAWN_INTERNAL_ERR_SEND;
    }
    if (xfer_buffer_borrow(pmbuf, fsmgr_ctx, BUFFER_BORROW_READ | BUFFER_BORROW_WRITE, &fs_grant) !=
        WASMOS_ERR_NONE) {
        return PM_SPAWN_INTERNAL_ERR_SEND;
    }
    req_id = g_pm.fs_request_id++;
    ipc_message_t req = {.type = FS_IPC_READ_PATH_REQ,
                         .source = g_pm.fs_reply_endpoint,
                         .destination = g_pm.fs_endpoint,
                         .request_id = req_id,
                         .arg0 = (int32_t)path_len,
                         .arg1 = 0,
                         .arg2 = (int32_t)pmbuf->buffer.buffer_id,
                         .arg3 = (int32_t)fs_grant.borrow_id};
    if (ipc_send_from(pm_context_id, g_pm.fs_endpoint, &req) != IPC_OK) {
        (void)xfer_buffer_unborrow(&fs_grant);
        return PM_SPAWN_INTERNAL_ERR_SEND;
    }

    int recv_rc = pm_recv_fs_reply(pm_context_id, g_pm.fs_reply_endpoint, req_id, &reply);
    /* Drop PM's grant now (not at buffer release): pmbuf is reused across
     * multiple reads in one spawn (e.g. broker: guest blob then host
     * executor), and fs-manager is the grant's borrower so it never drops
     * it. Leaving it active would make the next read's re-grant fail
     * ALREADY_BORROWED. PM created the grant, so it holds the handle. */
    (void)xfer_buffer_unborrow(&fs_grant);
    if (recv_rc != 0 || reply.type != FS_IPC_RESP || reply.arg0 <= 0 ||
        (uint32_t)reply.arg0 > max) {
        klog_write("[pm] spawn_path fs read failed: ");
        for (uint32_t i = 0; i < path_len; ++i) {
            char c[2];
            c[0] = path[i];
            c[1] = '\0';
            klog_write(c);
        }
        klog_write("\n");
        return PM_SPAWN_INTERNAL_ERR_BAD_REPLY;
    }

    *out_blob_size = (uint32_t)reply.arg0;
    return 0;
}

int pm_handle_spawn_sync(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t child_pid = 0;

    if (g_pm.spawn.in_use) {
        return WASMOS_ERR_PROC_PM_BUSY;
    }
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;

    if (pm_spawn_module(parent_pid, (uint32_t)msg->arg0, &child_pid) != 0) {
        return WASMOS_ERR_PROC_PM_SPAWN_FAILED;
    }
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, child_pid);

    uint32_t timeout_ms = (uint32_t)msg->arg1;
    g_pm.spawn.in_use = 1;
    g_pm.spawn.is_sync = 1;
    g_pm.spawn.reply_endpoint = msg->source;
    g_pm.spawn.request_id = msg->request_id;
    g_pm.spawn.parent_pid = parent_pid;
    g_pm.spawn.parent_context_id = owner_context;
    g_pm.spawn.sync_child_pid = child_pid;
    g_pm.spawn.app_flags = 0;
    g_pm.spawn.sync_timeout_ticks =
        (timeout_ms > 0) ? (timer_ticks() + timer_ms_to_ticks(timeout_ms)) : 0;
    process_unpark_pid(child_pid);
    return 0;
}

int pm_handle_spawn_caps_sync(uint32_t pm_context_id, const ipc_message_t* msg) {
    pm_spawn_caps_t caps = {0};
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t child_pid = 0;

    if (g_pm.spawn.in_use) {
        return WASMOS_ERR_PROC_PM_BUSY;
    }
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;

    caps.valid = 1;
    caps.cap_flags = (uint32_t)msg->arg1;
    caps.irq_mask = (uint16_t)((uint32_t)msg->arg3 & 0xFFFFu);
    uint32_t timeout_ms = (uint32_t)msg->arg3 >> 16;
    if (pm_caps_set_io_window(&caps, (uint16_t)((uint32_t)msg->arg2 & 0xFFFFu),
                              (uint16_t)(((uint32_t)msg->arg2 >> 16) & 0xFFFFu)) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        caps.dma_direction_flags = WASMOS_DMA_DIR_BIDIR;
        caps.dma_max_bytes = 4096u;
        caps.dma_window_count = 1;
        caps.dma_windows[0].base = 0;
        caps.dma_windows[0].length = 0x80000000ull;
    }

    if (pm_spawn_module(parent_pid, (uint32_t)msg->arg0, &child_pid) != 0) {
        return WASMOS_ERR_PROC_PM_SPAWN_FAILED;
    }
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, child_pid);
    if (pm_apply_spawn_caps(child_pid, &caps) != 0) {
        (void)process_kill(child_pid, -1);
        return WASMOS_ERR_PROC_PM_CAPS_APPLY;
    }
    /* Index caps-sync spawns come from the device manager (ready-gated, never
     * exit-waited); auto-reap on exit so one-shot enumerators (pci-bus/acpi-bus,
     * spawned by rule-index+caps) don't linger as zombies.  Harmless for
     * persistent drivers.  Same fixed-policy rationale as the path caps-sync. */
    (void)process_set_auto_reap(child_pid, 1);

    g_pm.spawn.in_use = 1;
    g_pm.spawn.is_sync = 1;
    g_pm.spawn.reply_endpoint = msg->source;
    g_pm.spawn.request_id = msg->request_id;
    g_pm.spawn.parent_pid = parent_pid;
    g_pm.spawn.parent_context_id = owner_context;
    g_pm.spawn.sync_child_pid = child_pid;
    g_pm.spawn.app_flags = 0;
    g_pm.spawn.sync_timeout_ticks =
        (timeout_ms > 0) ? (timer_ticks() + timer_ms_to_ticks(timeout_ms)) : 0;
    process_unpark_pid(child_pid);
    return 0;
}

int pm_handle_spawn_path_sync(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t spawn_req_flags = (uint32_t)msg->arg0;
    uint32_t path_len = (uint32_t)msg->arg1 & 0xFFFu;
    uint32_t caller_buffer_id = (uint32_t)msg->arg1 >> 12;
    uint32_t timeout_ms = (uint32_t)msg->arg3;
    const uint8_t* caller_fs_buf = 0;
    char path[256];
    pm_resolved_spawn_path_t resolved;
    uint32_t child_pid = 0;
    xfer_buffer_owner_t pmbuf = {0};

    if (g_pm.spawn.in_use) {
        return WASMOS_ERR_PROC_PM_BUSY;
    }
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;
    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || path_len == 0 || path_len >= sizeof(path)) {
        return WASMOS_ERR_PROC_PM_BAD_PATH;
    }
    caller_fs_buf = pm_foreign_xfer_ptr(caller_buffer_id, owner_context, 0);
    if (!caller_fs_buf || path_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    for (uint32_t i = 0; i < path_len; ++i) {
        path[i] = (char)caller_fs_buf[i];
    }
    path[path_len] = '\0';
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) != 0) {
        return WASMOS_ERR_PROC_PM_NO_PM_FSBUF;
    }
    if (pm_resolve_spawn_path(pm_context_id, &pmbuf, path, path_len, 0, 0, spawn_req_flags,
                              &resolved) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_PATH_RESOLVE;
    }
    if (pm_spawn_from_buffer(parent_pid, (const uint8_t*)pm_xfer_owner_ptr(&pmbuf),
                             resolved.blob_size, resolved.args_len > 0u ? resolved.args : 0,
                             resolved.args_len, &child_pid) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_SPAWN_FAILED;
    }
    pm_xfer_release(&pmbuf);
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, child_pid);
    /* Sync spawns complete on the child's READY, not its exit; a one-shot child
     * (e.g. pci-bus/acpi-bus enumerators) exits afterwards.  If the caller asked
     * to auto-reap (PROC_SPAWN_PATH_FLAG_AUTOREAP in arg0), free its slot on that
     * exit — reaping fires post-exit so it cannot race the ready reply. */
    if ((spawn_req_flags & PROC_SPAWN_PATH_FLAG_AUTOREAP) != 0) {
        (void)process_set_auto_reap(child_pid, 1);
    }

    g_pm.spawn.in_use = 1;
    g_pm.spawn.is_sync = 1;
    g_pm.spawn.reply_endpoint = msg->source;
    g_pm.spawn.request_id = msg->request_id;
    g_pm.spawn.parent_pid = parent_pid;
    g_pm.spawn.parent_context_id = owner_context;
    g_pm.spawn.sync_child_pid = child_pid;
    g_pm.spawn.app_flags = 0;
    g_pm.spawn.sync_timeout_ticks =
        (timeout_ms > 0) ? (timer_ticks() + timer_ms_to_ticks(timeout_ms)) : 0;
    process_unpark_pid(child_pid);
    return 0;
}

int pm_handle_spawn_path_caps_sync(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t caps_arg0 = (uint32_t)msg->arg0;
    uint32_t caps_arg2 = (uint32_t)msg->arg2;
    uint32_t path_len = (uint32_t)msg->arg1 & 0xFFFu;
    uint32_t caller_buffer_id = (uint32_t)msg->arg1 >> 12;
    uint32_t timeout_ms = (uint32_t)msg->arg3;
    pm_spawn_caps_t caps = {0};
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    const uint8_t* caller_fs_buf = 0;
    char path[256];
    char cli_args[256];
    uint32_t cli_args_len = 0;
    pm_resolved_spawn_path_t resolved;
    uint32_t child_pid = 0;
    xfer_buffer_owner_t pmbuf = {0};

    caps.valid = 1;
    caps.cap_flags = caps_arg0 & 0xFFFFu;
    caps.irq_mask = (uint16_t)(caps_arg0 >> 16);
    caps.dma_direction_flags = 0;
    caps.dma_max_bytes = 0;
    caps.dma_window_count = 0;

    if (g_pm.spawn.in_use) {
        return WASMOS_ERR_PROC_PM_BUSY;
    }
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;
    if (pm_caps_set_io_window(&caps, (uint16_t)(caps_arg2 & 0xFFFFu),
                              (uint16_t)(caps_arg2 >> 16)) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        /* Keep the path+caps sync ABI aligned with the compact spawn-caps
         * handlers: callers that only set the DMA bit still get the default
         * low-memory BIDIR window expected by existing in-tree drivers. */
        caps.dma_direction_flags = WASMOS_DMA_DIR_BIDIR;
        caps.dma_max_bytes = 4096u;
        caps.dma_window_count = 1;
        caps.dma_windows[0].base = 0;
        caps.dma_windows[0].length = 0x80000000ull;
    }
    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || path_len == 0 || path_len >= sizeof(path)) {
        return WASMOS_ERR_PROC_PM_BAD_PATH;
    }
    caller_fs_buf = pm_foreign_xfer_ptr(caller_buffer_id, owner_context, 0);
    if (!caller_fs_buf || path_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    for (uint32_t i = 0; i < path_len; ++i) {
        path[i] = (char)caller_fs_buf[i];
    }
    path[path_len] = '\0';
    if (path_len < xfer_buffer_size(BUFFER_KIND_TRANSFER) && caller_fs_buf[path_len] != 0u) {
        for (; path_len + cli_args_len < xfer_buffer_size(BUFFER_KIND_TRANSFER); ++cli_args_len) {
            uint8_t ch = caller_fs_buf[path_len + cli_args_len];
            if (ch == 0u) {
                break;
            }
            if (cli_args_len + 1u >= sizeof(cli_args)) {
                return WASMOS_ERR_PROC_PM_BAD_PATH;
            }
            cli_args[cli_args_len] = (char)ch;
        }
        if (path_len + cli_args_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
            return WASMOS_ERR_PROC_PM_BAD_PATH;
        }
    }
    cli_args[cli_args_len] = '\0';
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) != 0) {
        return WASMOS_ERR_PROC_PM_NO_PM_FSBUF;
    }
    if (pm_resolve_spawn_path(pm_context_id, &pmbuf, path, path_len,
                              cli_args_len > 0u ? cli_args : 0, cli_args_len, 0, &resolved) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_PATH_RESOLVE;
    }
    if (pm_spawn_from_buffer(parent_pid, (const uint8_t*)pm_xfer_owner_ptr(&pmbuf),
                             resolved.blob_size, resolved.args_len > 0u ? resolved.args : 0,
                             resolved.args_len, &child_pid) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_SPAWN_FAILED;
    }
    pm_xfer_release(&pmbuf);
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, child_pid);
    if (pm_apply_spawn_caps(child_pid, &caps) != 0) {
        (void)process_kill(child_pid, -1);
        return WASMOS_ERR_PROC_PM_CAPS_APPLY;
    }
    /* This caps-sync path is used only by the device manager, which sync-spawns
     * drivers waiting for READY (never for exit).  Auto-reap on exit so one-shot
     * enumerators (pci-bus/acpi-bus) don't linger as zombies; harmless for
     * persistent drivers (fires only once they become zombies).  All four IPC
     * args carry caps/path/timeout here, so there is no room for an explicit
     * per-spawn flag — the policy is fixed to this caller's model. */
    (void)process_set_auto_reap(child_pid, 1);

    g_pm.spawn.in_use = 1;
    g_pm.spawn.is_sync = 1;
    g_pm.spawn.reply_endpoint = msg->source;
    g_pm.spawn.request_id = msg->request_id;
    g_pm.spawn.parent_pid = parent_pid;
    g_pm.spawn.parent_context_id = owner_context;
    g_pm.spawn.sync_child_pid = child_pid;
    g_pm.spawn.app_flags = 0;
    g_pm.spawn.sync_timeout_ticks =
        (timeout_ms > 0) ? (timer_ticks() + timer_ms_to_ticks(timeout_ms)) : 0;
    process_unpark_pid(child_pid);
    return 0;
}

static void pm_poll_sync_spawn(uint32_t pm_context_id) {
    ipc_message_t resp;

    if (g_pm.spawn.sync_timeout_ticks > 0 && timer_ticks() >= g_pm.spawn.sync_timeout_ticks) {
        klog_write("[pm] spawn timeout ticks=");
        serial_write_hex64(timer_ticks());
        klog_write(" deadline=");
        serial_write_hex64(g_pm.spawn.sync_timeout_ticks);
        klog_write(" pid=");
        serial_write_hex64((uint64_t)g_pm.spawn.sync_child_pid);
        klog_write("\n");
        g_pm.spawn.in_use = 0;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = PROC_IPC_SPAWN_SYNC;
        resp.arg1 = (uint32_t)-1; /* timeout */
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
        return;
    }

    process_t* child = process_get(g_pm.spawn.sync_child_pid);
    if (!child || child->state == PROCESS_STATE_ZOMBIE || child->exiting) {
        klog_write("[pm] spawn child-dead pid=");
        serial_write_hex64((uint64_t)g_pm.spawn.sync_child_pid);
        klog_write(" state=");
        serial_write_hex64(child ? (uint64_t)child->state : 0xFFFFFFFFull);
        klog_write("\n");
        g_pm.spawn.in_use = 0;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = PROC_IPC_SPAWN_SYNC;
        resp.arg1 = (uint32_t)-2; /* child died before ready */
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
        return;
    }

    if (!child->ready) {
        return;
    }

    g_pm.spawn.in_use = 0;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = g_pm.spawn.reply_endpoint;
    resp.request_id = g_pm.spawn.request_id;
    resp.arg0 = g_pm.spawn.sync_child_pid;
    resp.arg1 = g_pm.spawn.app_flags;
    resp.arg2 = 0;
    resp.arg3 = 0;
    ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
}

/* Poll for completion of an in-flight spawn request (called from PM's IPC
 * dispatch loop each tick). Idle (in_use == 0): nothing pending. Otherwise the
 * only in-flight spawns are synchronous (FS read + inline spawn), driven by
 * pm_poll_sync_spawn. The former name-based async path (FS_IPC_READ_APP round
 * trip) was removed with spawn-by-name; spawns now read blobs inline in the
 * path handlers using a PM-owned per-operation buffer. */
void pm_poll_spawn(uint32_t pm_context_id) {
    if (!g_pm.spawn.in_use) {
        return;
    }
    pm_poll_sync_spawn(pm_context_id);
}

int pm_handle_notify_ready(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* sender = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    sender = process_find_by_context(owner_context);
    if (!sender) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    process_notify_ready(sender);

    if (g_pm.spawn.in_use && g_pm.spawn.is_sync && g_pm.spawn.sync_child_pid == sender->pid) {
        ipc_message_t resp;
        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = g_pm.spawn.sync_child_pid;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        g_pm.spawn.in_use = 0;
        if (ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp) != IPC_OK) {
            return WASMOS_ERR_PROC_PM_REPLY_SEND;
        }
    }

    /* Ack the sender so wasmos_sys_notify_ready() can unblock.  This must
     * happen after the sync-spawn response above to avoid a window where the
     * child resumes and destroys its endpoint before the parent is unblocked. */
    ipc_message_t ack;
    ack.type = PROC_IPC_RESP;
    ack.source = g_pm.proc_endpoint;
    ack.destination = msg->source;
    ack.request_id = msg->request_id;
    ack.arg0 = 0;
    ack.arg1 = 0;
    ack.arg2 = 0;
    ack.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &ack) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

void pm_check_waits(uint32_t pm_context_id) {
    list_iter_t it;
    pm_wait_state_t* waiter = (pm_wait_state_t*)list_first(&g_pm.waits, &it);
    while (waiter) {
        uint32_t reply_owner_context = 0;
        if (!waiter->in_use) {
            waiter = (pm_wait_state_t*)list_next(&it);
            continue;
        }
        if (ipc_endpoint_owner(waiter->reply_endpoint, &reply_owner_context) != IPC_OK ||
            reply_owner_context != waiter->owner_context_id) {
            if (!g_pm_wait_owner_deny_logged) {
                g_pm_wait_owner_deny_logged = 1;
                klog_write("[test] pm wait reply owner deny ok\n");
            }
            waiter->in_use = 0;
            waiter = (pm_wait_state_t*)list_next(&it);
            continue;
        }
        int32_t exit_status = 0;
        int rc = process_get_exit_status(waiter->pid, &exit_status);
        if (rc != 0) {
            waiter = (pm_wait_state_t*)list_next(&it);
            continue;
        }

        ipc_message_t resp;
        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = waiter->reply_endpoint;
        resp.request_id = waiter->request_id;
        resp.arg0 = waiter->pid;
        resp.arg1 = (uint32_t)exit_status;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, waiter->reply_endpoint, &resp);
        waiter->in_use = 0;
        /* Status delivered — reap the zombie so its process slot is freed. */
        process_reap_zombie_pid(waiter->pid);
        waiter = (pm_wait_state_t*)list_next(&it);
    }
}

void pm_reap_apps(process_t* owner) {
    if (!owner) {
        return;
    }
    list_iter_t it;
    pm_app_state_t* app = (pm_app_state_t*)list_first(&g_pm.apps, &it);
    while (app) {
        if (!app->in_use || app->pid == 0) {
            app = (pm_app_state_t*)list_next(&it);
            continue;
        }
        int32_t exit_status = 0;
        if (process_get_exit_status(app->pid, &exit_status) != 0) {
            app = (pm_app_state_t*)list_next(&it);
            continue;
        }
        if (process_wait(owner, app->pid, &exit_status) != 0) {
            app = (pm_app_state_t*)list_next(&it);
            continue;
        }
        wasmos_app_stop(&app->app);
        pm_slot_reset(app);
        app = (pm_app_state_t*)list_next(&it);
    }
}

int pm_handle_spawn(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        if (!g_pm_spawn_owner_deny_logged) {
            g_pm_spawn_owner_deny_logged = 1;
            klog_write("[test] pm spawn owner deny ok\n");
        }
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;

    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return WASMOS_ERR_PROC_PM_INVALID_MODULE;
    }
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);
    process_unpark_pid(pid);

    /* Fire-and-forget one-shots carry PROC_SPAWN_PATH_FLAG_AUTOREAP in arg1 for
     * this index-based spawn; auto-reap them on exit so they don't linger as
     * zombies.  Only fires post-exit, and this path has no PROC_IPC_WAIT waiter. */
    if (((uint32_t)msg->arg1 & PROC_SPAWN_PATH_FLAG_AUTOREAP) != 0) {
        (void)process_set_auto_reap(pid, 1);
    }

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_spawn_caps(uint32_t pm_context_id, const ipc_message_t* msg) {
    pm_spawn_caps_t caps = {0};
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;
    caps.valid = 1;
    caps.cap_flags = msg->arg1;
    caps.irq_mask = (uint16_t)((uint32_t)msg->arg3 & 0xFFFFu);
    caps.dma_direction_flags = 0;
    caps.dma_max_bytes = 0;
    caps.dma_window_count = 0;
    if (pm_caps_set_io_window(&caps, (uint16_t)((uint32_t)msg->arg2 & 0xFFFFu),
                              (uint16_t)(((uint32_t)msg->arg2 >> 16) & 0xFFFFu)) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        /* Legacy fallback for in-tree storage DMA rollout: when callers use the
         * compact spawn-caps request with DMA bit set, apply a conservative
         * default DMA policy equivalent to the current device-manager profile. */
        caps.dma_direction_flags = WASMOS_DMA_DIR_BIDIR;
        caps.dma_max_bytes = 4096u;
        caps.dma_window_count = 1;
        caps.dma_windows[0].base = 0;
        caps.dma_windows[0].length = 0x80000000ull;
    }
    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return WASMOS_ERR_PROC_PM_INVALID_MODULE;
    }
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);
    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        (void)process_kill(pid, -1);
        return WASMOS_ERR_PROC_PM_CAPS_APPLY;
    }
    process_unpark_pid(pid);
    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_spawn_caps_v2(uint32_t pm_context_id, const ipc_message_t* msg) {
    pm_spawn_caps_t caps = {0};
    wasmos_spawn_caps_v2_t in_caps;
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    uint32_t known_cap_mask =
        DEVMGR_CAP_IO_PORT | DEVMGR_CAP_MMIO_MAP | DEVMGR_CAP_IRQ | DEVMGR_CAP_DMA;
    uint32_t payload_size = 0;
    uint32_t expected_size = 0;
    const uint8_t* caps_payload = 0;
    uint32_t caps_buffer_size = 0;
    uint64_t win_end = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;
    /* arg1 names a transfer buffer, not a raw pointer. pm_resolve_user_ptr
     * cannot see WARP's linear memory -- it looks for a MEM_REGION_WASM_LINEAR
     * region, which reserved-VA linmem slots are not, and silently falls back to
     * treating the offset as an absolute user VA. That read unrelated memory
     * (stack poison, once stacks were poisoned), which is why this path never
     * worked: it had no users to expose it. The xfer-buffer object resolves via
     * the backing pages and is what every working descriptor path uses. */
    payload_size = (uint32_t)msg->arg2;
    if (msg->arg1 == 0 || payload_size < (uint32_t)sizeof(in_caps)) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    caps_payload = pm_foreign_xfer_ptr((uint32_t)msg->arg1, owner_context, &caps_buffer_size);
    if (!caps_payload || payload_size > caps_buffer_size) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    __builtin_memcpy(&in_caps, caps_payload, sizeof(in_caps));

    if ((in_caps.cap_flags & ~known_cap_mask) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((in_caps.cap_flags & DEVMGR_CAP_IO_PORT) != 0 && in_caps.io_range_count == 0 &&
        in_caps.io_port_min > in_caps.io_port_max) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if (in_caps.io_range_count > WASMOS_IO_RANGE_LIMIT) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }

    caps.valid = 1;
    caps.cap_flags = in_caps.cap_flags;
    caps.irq_mask = in_caps.irq_mask;
    caps.dma_direction_flags = 0;
    caps.dma_max_bytes = 0;
    caps.dma_window_count = 0;

    /* A non-zero io_range_count supersedes the single io_port_min/max window;
     * the ranges follow the fixed header, ahead of the DMA windows. */
    if (in_caps.io_range_count > 0) {
        expected_size = WASMOS_SPAWN_CAPS_V2_SIZE(in_caps.io_range_count, 0);
        if (payload_size < expected_size) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        __builtin_memcpy(caps.io_ranges, caps_payload + sizeof(in_caps),
                         (size_t)in_caps.io_range_count * sizeof(wasmos_io_range_t));
        caps.io_range_count =
            ((caps.cap_flags & DEVMGR_CAP_IO_PORT) != 0) ? (uint32_t)in_caps.io_range_count : 0u;
        for (uint32_t i = 0; i < caps.io_range_count; ++i) {
            if (caps.io_ranges[i].first > caps.io_ranges[i].last) {
                return WASMOS_ERR_PROC_PM_BAD_CAPS;
            }
        }
    } else if (pm_caps_set_io_window(&caps, in_caps.io_port_min, in_caps.io_port_max) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        if ((in_caps.dma.direction_flags & ~WASMOS_DMA_DIR_BIDIR) != 0 ||
            in_caps.dma.direction_flags == 0 || in_caps.dma.max_bytes == 0 ||
            in_caps.dma.window_count == 0 || in_caps.dma.window_count > PM_DMA_WINDOW_LIMIT) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        expected_size = WASMOS_SPAWN_CAPS_V2_SIZE(in_caps.io_range_count, in_caps.dma.window_count);
        if (payload_size != expected_size) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        __builtin_memcpy(caps.dma_windows,
                         caps_payload + sizeof(in_caps) +
                             (size_t)in_caps.io_range_count * sizeof(wasmos_io_range_t),
                         (size_t)in_caps.dma.window_count * sizeof(wasmos_dma_window_t));
        for (uint32_t i = 0; i < in_caps.dma.window_count; ++i) {
            if (caps.dma_windows[i].length == 0) {
                return WASMOS_ERR_PROC_PM_BAD_CAPS;
            }
            win_end = caps.dma_windows[i].base + caps.dma_windows[i].length;
            if (win_end < caps.dma_windows[i].base) {
                return WASMOS_ERR_PROC_PM_BAD_CAPS;
            }
        }
        caps.dma_direction_flags = in_caps.dma.direction_flags;
        caps.dma_max_bytes = in_caps.dma.max_bytes;
        caps.dma_window_count = in_caps.dma.window_count;
    } else if (payload_size != (uint32_t)sizeof(in_caps)) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }

    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return WASMOS_ERR_PROC_PM_INVALID_MODULE;
    }
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);
    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        (void)process_kill(pid, -1);
        return WASMOS_ERR_PROC_PM_CAPS_APPLY;
    }
    process_unpark_pid(pid);
    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_spawn_path(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t path_len = (uint32_t)msg->arg1 & 0xFFFu;
    uint32_t caller_buffer_id = (uint32_t)msg->arg1 >> 12;
    uint32_t args_len = (uint32_t)msg->arg2;
    const uint8_t* caller_fs_buf = 0;
    char path[256];
    char cli_args[256];
    pm_resolved_spawn_path_t resolved;
    uint32_t pid = 0;
    uint32_t spawn_req_flags = (uint32_t)msg->arg0;
    xfer_buffer_owner_t pmbuf = {0};

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_SPAWN_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_SPAWN_NO_CALLER;
    }
    parent_pid = caller->pid;
    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || path_len == 0 || path_len >= sizeof(path)) {
        return WASMOS_ERR_PROC_SPAWN_BAD_PATH;
    }
    caller_fs_buf = pm_foreign_xfer_ptr(caller_buffer_id, owner_context, 0);
    if (!caller_fs_buf || path_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return WASMOS_ERR_PROC_SPAWN_CALLER_FSBUF;
    }
    for (uint32_t i = 0; i < path_len; ++i) {
        path[i] = (char)caller_fs_buf[i];
    }
    path[path_len] = '\0';
    if (args_len > 0u) {
        uint32_t fs_buf_size = xfer_buffer_size(BUFFER_KIND_TRANSFER);
        uint32_t args_off = path_len + 1u;
        if (args_len >= sizeof(cli_args) || args_off >= fs_buf_size ||
            args_len > (fs_buf_size - args_off)) {
            return WASMOS_ERR_PROC_SPAWN_ARGS_TOOBIG;
        }
        for (uint32_t i = 0; i < args_len; ++i) {
            cli_args[i] = (char)caller_fs_buf[args_off + i];
        }
        cli_args[args_len] = '\0';
    } else {
        cli_args[0] = '\0';
    }
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) != 0) {
        return WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF;
    }

    int resolve_rc =
        pm_resolve_spawn_path(pm_context_id, &pmbuf, path, path_len, args_len > 0u ? cli_args : 0,
                              args_len, spawn_req_flags, &resolved);
    if (resolve_rc != 0) {
        pm_xfer_release(&pmbuf);
        return resolve_rc;
    }

    wasmos_app_desc_t desc;
    uint32_t app_flags = 0;
    int parse_rc =
        wasmos_app_parse((const uint8_t*)pm_xfer_owner_ptr(&pmbuf), resolved.blob_size, &desc);
    if (parse_rc == 0) {
        app_flags = desc.flags;
    }
    int ready_policy = 0;
    if (parse_rc == 0) {
        ready_policy = wasmos_app_requires_explicit_ready(&desc);
        if (ready_policy < 0) {
            pm_xfer_release(&pmbuf);
            return WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
        }
    }
    int needs_ready = ready_policy != 0 && !(spawn_req_flags & PROC_SPAWN_PATH_FLAG_DETACH);
    if (needs_ready && g_pm.spawn.in_use) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_BUSY;
    }
    if (pm_spawn_from_buffer(parent_pid, (const uint8_t*)pm_xfer_owner_ptr(&pmbuf),
                             resolved.blob_size, resolved.args_len > 0u ? resolved.args : 0,
                             resolved.args_len, &pid) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
    }
    pm_xfer_release(&pmbuf);
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);

    /* Auto-reap fire-and-forget one-shots on exit so their g_processes[] slot is
     * freed (PROC_SPAWN_PATH_FLAG_AUTOREAP); DETACH implies the same (no waiter).
     * Reaping only fires post-exit, so this is orthogonal to the ready-gating
     * below and applies to both the ready-gated and immediate-reply paths.  NOT
     * for apps the caller will PROC_IPC_WAIT on — process_try_auto_reap cannot
     * see PM IPC waiters and would race the status reply. */
    if ((spawn_req_flags & (PROC_SPAWN_PATH_FLAG_DETACH | PROC_SPAWN_PATH_FLAG_AUTOREAP)) != 0) {
        (void)process_set_auto_reap(pid, 1);
    }

    if (needs_ready) {
        g_pm.spawn.in_use = 1;
        g_pm.spawn.is_sync = 1;
        g_pm.spawn.reply_endpoint = msg->source;
        g_pm.spawn.request_id = msg->request_id;
        g_pm.spawn.parent_pid = parent_pid;
        g_pm.spawn.parent_context_id = owner_context;
        g_pm.spawn.sync_child_pid = pid;
        g_pm.spawn.app_flags = app_flags;
        g_pm.spawn.sync_timeout_ticks = 0;
        process_unpark_pid(pid);
        return 0;
    }

    process_unpark_pid(pid);
    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = app_flags;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_spawn_path_caps(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t path_len = (uint32_t)msg->arg1 & 0xFFFu;
    uint32_t caller_buffer_id = (uint32_t)msg->arg1 >> 12;
    const uint8_t* caller_fs_buf = 0;
    char path[256];
    char cli_args[256];
    uint32_t cli_args_len = 0;
    uint32_t pid = 0;
    pm_spawn_caps_t caps = {0};
    pm_resolved_spawn_path_t resolved;
    xfer_buffer_owner_t pmbuf = {0};

    /* Unpack caps from IPC args:
     * arg0 = (irq_mask<<16) | (cap_flags & 0xFFFF)
     * arg2 = (io_port_max<<16) | io_port_min */
    uint32_t caps_arg0 = (uint32_t)msg->arg0;
    uint32_t caps_arg2 = (uint32_t)msg->arg2;
    caps.valid = 1;
    caps.cap_flags = caps_arg0 & 0xFFFFu;
    caps.irq_mask = (uint16_t)(caps_arg0 >> 16);
    caps.dma_direction_flags = 0;
    caps.dma_max_bytes = 0;
    caps.dma_window_count = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    parent_pid = caller->pid;
    if (pm_caps_set_io_window(&caps, (uint16_t)(caps_arg2 & 0xFFFFu),
                              (uint16_t)(caps_arg2 >> 16)) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        caps.dma_direction_flags = WASMOS_DMA_DIR_BIDIR;
        caps.dma_max_bytes = 4096u;
        caps.dma_window_count = 1;
        caps.dma_windows[0].base = 0;
        caps.dma_windows[0].length = 0x80000000ull;
    }
    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || path_len == 0 || path_len >= sizeof(path)) {
        return WASMOS_ERR_PROC_PM_BAD_PATH;
    }
    caller_fs_buf = pm_foreign_xfer_ptr(caller_buffer_id, owner_context, 0);
    if (!caller_fs_buf || path_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    for (uint32_t i = 0; i < path_len; ++i) {
        path[i] = (char)caller_fs_buf[i];
    }
    path[path_len] = '\0';
    if (path_len < xfer_buffer_size(BUFFER_KIND_TRANSFER) && caller_fs_buf[path_len] != 0u) {
        for (; path_len + cli_args_len < xfer_buffer_size(BUFFER_KIND_TRANSFER); ++cli_args_len) {
            uint8_t ch = caller_fs_buf[path_len + cli_args_len];
            if (ch == 0u) {
                break;
            }
            if (cli_args_len + 1u >= sizeof(cli_args)) {
                return WASMOS_ERR_PROC_PM_BAD_PATH;
            }
            cli_args[cli_args_len] = (char)ch;
        }
        if (path_len + cli_args_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
            return WASMOS_ERR_PROC_PM_BAD_PATH;
        }
    }
    cli_args[cli_args_len] = '\0';
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) != 0) {
        return WASMOS_ERR_PROC_PM_NO_PM_FSBUF;
    }
    if (pm_resolve_spawn_path(pm_context_id, &pmbuf, path, path_len,
                              cli_args_len > 0u ? cli_args : 0, cli_args_len, 0, &resolved) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_PATH_RESOLVE;
    }
    if (pm_spawn_from_buffer(parent_pid, (const uint8_t*)pm_xfer_owner_ptr(&pmbuf),
                             resolved.blob_size, resolved.args_len > 0u ? resolved.args : 0,
                             resolved.args_len, &pid) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_SPAWN_FAILED;
    }
    pm_xfer_release(&pmbuf);
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);
    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        (void)process_kill(pid, -1);
        return WASMOS_ERR_PROC_PM_CAPS_APPLY;
    }
    process_unpark_pid(pid);

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

/* Module metadata as a descriptor. Same lookup as pm_handle_module_meta, but the
 * answer lands in a buffer the caller owns and has lent WRITE, because the
 * declared region list is variable-length and the packed form's four response
 * words are full. */
int pm_handle_module_meta_desc(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    wasmos_app_desc_t desc;
    wasmos_module_meta_desc_t out;
    const wasmos_app_driver_match_t* match = 0;
    uint32_t match_index = (uint32_t)msg->arg1;
    uint32_t buffer_size = 0;
    uint32_t offset = (uint32_t)msg->arg3;
    const uint8_t* buf = 0;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    if (wasmos_app_module_desc(g_pm.boot_info, (uint32_t)msg->arg0, &desc) != 0) {
        return WASMOS_ERR_PROC_PM_META_LOOKUP;
    }
    if ((desc.flags & WASMOS_APP_FLAG_DRIVER) == 0) {
        return WASMOS_ERR_PROC_PM_META_NOT_DRIVER;
    }
    if (desc.driver_match_count == 0 || match_index >= desc.driver_match_count) {
        return WASMOS_ERR_PROC_PM_META_BAD_INDEX;
    }
    match = &desc.driver_matches[match_index];

    buf = pm_foreign_xfer_ptr((uint32_t)msg->arg2, owner_context, &buffer_size);
    if (!buf || (uint64_t)offset + sizeof(out) > (uint64_t)buffer_size) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }

    __builtin_memset(&out, 0, sizeof(out));
    out.version = WASMOS_MODULE_META_DESC_VERSION;
    out.class_code = match->class_code;
    out.subclass = match->subclass;
    out.prog_if = match->prog_if;
    out.storage_bootstrap = ((desc.flags & WASMOS_APP_FLAG_STORAGE_BOOTSTRAP) != 0) ? 1u : 0u;
    out.vendor_id = match->vendor_id;
    out.device_id = match->device_id;
    out.io_port_min = match->io_port_min;
    out.io_port_max = match->io_port_max;
    out.cap_flags = wasmos_app_driver_cap_flags(&desc);
    out.match_count = desc.driver_match_count;
    out.region_count = desc.region_count;
    for (uint32_t i = 0; i < desc.region_count && i < WASMOS_MODULE_META_MAX_REGIONS; ++i) {
        out.regions[i].kind = desc.regions[i].kind;
        out.regions[i].bar_index = desc.regions[i].bar_index;
        out.regions[i].first = desc.regions[i].first;
        out.regions[i].last = desc.regions[i].last;
    }
    __builtin_memcpy((uint8_t*)(uintptr_t)buf + offset, &out, sizeof(out));

    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = (int32_t)sizeof(out);
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_module_meta(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    wasmos_app_desc_t desc;
    uint32_t match_index = msg->arg1;
    uint32_t match_count = 0;
    wasmos_app_driver_match_t* match = 0;
    uint32_t cap_flags = 0;
    uint32_t packed_match = 0;
    uint32_t packed_vendor_device = 0;
    uint32_t packed_caps = 0;
    uint32_t packed_io = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    if (wasmos_app_module_desc(g_pm.boot_info, msg->arg0, &desc) != 0) {
        return WASMOS_ERR_PROC_PM_META_LOOKUP;
    }
    if ((desc.flags & WASMOS_APP_FLAG_DRIVER) == 0) {
        return WASMOS_ERR_PROC_PM_META_NOT_DRIVER;
    }
    match_count = desc.driver_match_count;
    if (match_count == 0 || match_index >= match_count) {
        return WASMOS_ERR_PROC_PM_META_BAD_INDEX;
    }
    match = &desc.driver_matches[match_index];
    cap_flags = wasmos_app_driver_cap_flags(&desc);

    packed_match = ((uint32_t)match->class_code << 24) | ((uint32_t)match->subclass << 16) |
                   ((uint32_t)match->prog_if << 8) |
                   ((((desc.flags & WASMOS_APP_FLAG_STORAGE_BOOTSTRAP) != 0) ? 1u : 0u) |
                    ((match_count & 0x7Fu) << 1));
    packed_vendor_device = ((uint32_t)match->vendor_id << 16) | (uint32_t)match->device_id;
    packed_caps = (uint32_t)cap_flags;
    packed_io = ((uint32_t)match->io_port_max << 16) | ((uint32_t)match->io_port_min & 0xFFFFu);

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = packed_io;
    resp.arg1 = packed_match;
    resp.arg2 = packed_vendor_device;
    resp.arg3 = packed_caps;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

int pm_handle_module_meta_path(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    const uint8_t* caller_fs_buf = 0;
    char path[96];
    uint32_t path_ptr = (uint32_t)msg->arg0;
    uint32_t path_len = (uint32_t)msg->arg1 & 0xFFFu;
    uint32_t caller_buffer_id = (uint32_t)msg->arg1 >> 12;
    uint32_t source = (uint32_t)msg->arg2;
    wasmos_app_desc_t desc;
    uint32_t module_index = 0xFFFFFFFFu;
    uint32_t cap_flags = 0;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    if (path_len == 0 || path_len >= sizeof(path)) {
        return WASMOS_ERR_PROC_PM_BAD_PATH;
    }
    if (path_ptr == 0) {
        caller_fs_buf = pm_foreign_xfer_ptr(caller_buffer_id, owner_context, 0);
        if (!caller_fs_buf || path_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
            return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
        }
        for (uint32_t i = 0; i < path_len; ++i) {
            path[i] = (char)caller_fs_buf[i];
        }
    } else {
        if (mm_copy_from_user(owner_context, path, (uint64_t)path_ptr, path_len) != 0) {
            return WASMOS_ERR_PROC_PM_USER_COPY;
        }
    }
    path[path_len] = '\0';
    if (source == PROC_MODULE_SOURCE_INITFS) {
        if (wasmos_app_module_desc_by_initfs_path(g_pm.boot_info, path, &module_index, &desc) !=
            0) {
            return WASMOS_ERR_PROC_PM_META_LOOKUP;
        }
    } else if (source == PROC_MODULE_SOURCE_FS) {
        return WASMOS_ERR_PROC_PM_META_BAD_SOURCE;
    } else {
        return WASMOS_ERR_PROC_PM_META_BAD_SOURCE;
    }

    cap_flags = wasmos_app_driver_cap_flags(&desc);
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = module_index;
    resp.arg1 = desc.flags;
    resp.arg2 = cap_flags;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}
