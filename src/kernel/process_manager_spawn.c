/* process_manager_spawn.c - PM SPAWN request handling.
 * Reads a WASMOS-APP payload from the FS or initfs, parses it, sets up the child
 * process address space, grants IPC endpoints and capabilities, then starts the WASM
 * or native driver.  Responds with PROC_IPC_RESP when the child is ready. */
#include "process_manager_internal.h"
#include "klog.h"
#include "kmem.h"
#include "serial.h"
#include "process_manager.h"
#include "capability.h"
#include "device_window.h"
#include "pci_config.h"
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

/* Scheduler band for a spawned app based on its exec-format flags.  Drivers get
 * SCHED_PRIO_DRIVER so latency-critical device work -- e.g. the serial driver
 * draining the UART RX FIFO on its IRQ before it overruns -- outranks ordinary
 * apps under load.
 *
 * Services get SCHED_PRIO_SERVICE so they respond to app requests promptly
 * instead of competing in the WASM band; the VT input multiplexer in particular
 * must run to process serial input the driver hands it, or input stalls during a
 * spawn storm.  Boosting a band above the apps is only safe while every service
 * in it blocks at idle: the anti-starvation streak in cpu_sched_pick_next keeps
 * a spinning service from locking apps out entirely, but it concedes only one
 * slot in SCHED_ANTISTARVATION_STREAK, so the apps still crawl.  Plain apps keep
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
    PM_SPAWN_INTERNAL_ERR_EMPTY_PATH = -1013,
    PM_SPAWN_INTERNAL_ERR_UNTRUSTED = -1014
} pm_spawn_internal_error_t;

/* --- PM's per-operation transfer buffers ----------------------------------
 * PM does NOT keep a single shared buffer. Each operation (one spawn, one FS
 * read) acquires its own BUFFER_KIND_TRANSFER object owned by PM, uses it, and
 * releases it when the operation completes. Concurrent operations therefore get
 * distinct objects and buffer_ids, matching the object model (a context owns as
 * many live buffers as it has live operations). PM hands an operation's
 * buffer_id to a provider (e.g. fs-manager) which borrows it to fill it; PM
 * reads the result out of the object it owns and then releases it.
 *
 * Returns WASMOS_ERR_NONE or xfer_buffer_acquire's packed code. A failure is
 * logged with that code, because a spawn reports it to the requester only as
 * WASMOS_ERR_PROC_*_NO_PM_FSBUF. */
static int pm_xfer_acquire(uint32_t pm_context_id, uint32_t minimum_size,
                           xfer_buffer_owner_t* out) {
    if (minimum_size == 0u) {
        minimum_size = 1u;
    }
    int rc = xfer_buffer_acquire(BUFFER_KIND_TRANSFER, pm_context_id, minimum_size, out);
    if (rc != WASMOS_ERR_NONE) {
        klog_printf("[pm] no %u-byte transfer buffer: %s (%llu KiB free)\n",
                    minimum_size,
                    wasmos_strerror((wasmos_error_code_t)rc),
                    (unsigned long long)(pfa_free_bytes() / 1024u));
    }
    return rc;
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
    slot->requested_tty = 0;
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
        /* Says WHAT was asked for, not only that it failed: a genuine shortage
         * and a request for a nonsensical size both surface here as ALLOC, and
         * they are fixed at opposite ends of the system. */
        klog_printf("[pm] blob alloc failed: %llu page(s) for %u bytes\n",
                    (unsigned long long)pages,
                    (unsigned)blob_size);
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

/* Index into the boot module table of the WASMOS-APP whose embedded name matches
 * `name` exactly, or 0xFFFFFFFF when there is none — which is the sentinel
 * g_pm.init_module_index carries and every caller tests against, NOT an index.
 *
 * Linear over every module, re-parsing each header on every call; only used at
 * init and on module-index spawns, so that cost is not on a hot path.  Modules
 * that are not WASMOS-APPs, are empty, or fail to parse are skipped rather than
 * ending the search, and a name longer than 63 bytes never matches (it does not
 * fit the comparison buffer). */
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

/* Startup values reach the child through the spawn-info contract (see
 * wasmos_spawn_info.h and the buffer pm_app_entry builds), and service endpoints
 * through svc_lookup -- not through entry arguments.  Guest entry points
 * therefore take no arguments and argc is 0; the entry_arg* slots remain only
 * as the zero-filled storage the manifest still carries. */
static int pm_apply_entry_bindings(pm_app_state_t* slot, const wasmos_app_desc_t* desc) {
    if (!slot || !desc) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    slot->entry_argc = 0;
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
            process_set_exit(process, -1, PROC_EXIT_REASON_START_FAILED);
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
            process_set_exit(process, -1, PROC_EXIT_REASON_START_FAILED);
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
        if ((desc.flags & WASMOS_APP_FLAG_WANTS_TTY) == 0u) {
            si->tty = 0u;
        } else if (state->requested_tty != 0u) {
            si->tty = state->requested_tty - 1u;
        } else {
            si->tty = pm_alloc_cli_tty();
        }
        /* A wants_tty child owns its allocated slot.  Any other child keeps the
         * slot it inherited from the requester at spawn (process.c), so an app
         * run from a shell lands on that shell's slot and a service started by
         * init stays kernel log. */
        if ((desc.flags & WASMOS_APP_FLAG_WANTS_TTY) != 0u) {
            process->ctty = (uint8_t)si->tty;
        }
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

        /* Zeros: the child pulls everything from its spawn-info buffer.  See
         * pm_apply_entry_bindings. */
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

        /* TODO: a native module runs its whole entry inside the start hook, so
         * a native driver that returns a non-zero status after running is
         * reported as START_FAILED here instead of EXITED with its status. */
        if (wasmos_app_start(&state->app,
                             &desc,
                             process->context_id,
                             state->requester_cap_mask,
                             init_args,
                             state->entry_argc) != 0) {
            klog_write("[pm] app start failed\n");
            process_set_exit(process, -1, PROC_EXIT_REASON_START_FAILED);
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
        process_set_exit(process, -1, PROC_EXIT_REASON_TRAPPED);
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

/* Marks a process this manager just created, still parked, as started from a
 * package image (see pm_requester_cap_mask). */
static void pm_mark_from_image(uint32_t pid) {
    process_t* created = process_get(pid);
    if (created) {
        created->from_image = 1;
    }
}

/* What the process asking for a spawn holds. Each spawn handler captures it on
 * entry, before any filesystem read, and the child's manifest capabilities are
 * decided against that value (capability_manifest_grant_check) when the child
 * starts, by which time the requester may have exited.
 *
 * A process this manager created from an image (process_t.from_image, set at
 * creation and kept until the slot is reused) holds what its manifest was
 * granted while it runs, and nothing once it is exiting, a zombie or reaped.
 * Any other live process is the kernel's own -- pid 0, the init process that
 * spawns the supervisor, a self-test -- and holds every capability, as context
 * 0 does. The creation mark, not the runtime tag, is the test, because only
 * this manager sets it while a subsystem broker could register a tag of its
 * choosing. A requester without a process holds nothing. */
static uint32_t pm_requester_cap_mask(uint32_t parent_pid) {
    if (parent_pid == 0u) {
        return capability_mask_of(IPC_CONTEXT_KERNEL);
    }
    process_t* requester = process_get(parent_pid);
    if (!requester) {
        return 0u;
    }
    if (!requester->from_image) {
        return capability_mask_of(IPC_CONTEXT_KERNEL);
    }
    if (__atomic_load_n(&requester->exiting, __ATOMIC_ACQUIRE) != 0u ||
        requester->context_id == IPC_CONTEXT_KERNEL) {
        return 0u;
    }
    return capability_mask_of(requester->context_id);
}

static int pm_spawn_module(uint32_t parent_pid, uint32_t requester_cap_mask, uint32_t module_index,
                           uint32_t* out_pid) {
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
    if (wasmos_app_admit(&desc) != 0) {
        return PM_SPAWN_INTERNAL_ERR_UNTRUSTED;
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
    slot->requester_cap_mask = requester_cap_mask;
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
             ? process_spawn_as_ready_gated_parked(
                   parent_pid, slot->name, pm_app_entry, slot, out_pid)
             : process_spawn_as_parked(parent_pid, slot->name, pm_app_entry, slot, out_pid)) != 0) {
        klog_write("[pm] spawn_module process spawn failed: ");
        klog_write(slot->name);
        klog_write("\n");
        preempt_enable();
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }

    slot->pid = *out_pid;
    pm_mark_from_image(*out_pid);
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

/* Installs caps as the child's spawn profile. Each memory-mapped window is
 * mapped into the kernel first (device_window_map), since the io_region_* host
 * calls reach it through that mapping; a window that cannot be mapped refuses
 * the spawn like any other grant the kernel cannot honour. */
static int pm_apply_spawn_caps(uint32_t pid, const pm_spawn_caps_t* caps) {
    process_t* proc = 0;
    capability_region_t regions[WASMOS_REGION_LIMIT];
    if (!caps || !caps->valid) {
        return 0;
    }
    proc = process_get(pid);
    if (!proc || proc->context_id == 0 || caps->region_count > WASMOS_REGION_LIMIT) {
        return PM_SPAWN_INTERNAL_ERR_BAD_PROCESS;
    }
    for (uint32_t i = 0; i < caps->region_count; ++i) {
        regions[i].kind = caps->regions[i].kind;
        regions[i].length = caps->regions[i].length;
        regions[i].base = caps->regions[i].base;
        regions[i].kva = 0;
        if (caps->regions[i].kind == WASMOS_REGION_GRANT_MMIO &&
            device_window_map(caps->regions[i].base, caps->regions[i].length, &regions[i].kva) !=
                0) {
            return PM_SPAWN_INTERNAL_ERR_CAPS_APPLY;
        }
    }
    if (capability_set_spawn_profile(proc->context_id,
                                     caps->cap_flags,
                                     caps->region_count,
                                     regions,
                                     caps->irq_count,
                                     caps->irqs,
                                     caps->dma_direction_flags,
                                     caps->dma_max_bytes,
                                     caps->dma_window_count,
                                     caps->dma_windows) != 0) {
        return PM_SPAWN_INTERNAL_ERR_CAPS_APPLY;
    }
    if ((caps->cap_flags & DEVMGR_CAP_PCI_FUNCTION) != 0 &&
        pci_function_attach(proc->context_id, caps->pci_function) != 0) {
        return PM_SPAWN_INTERNAL_ERR_CAPS_APPLY;
    }
    return 0;
}

/* requested_tty is the PROC_SPAWN_PATH_TTY encoding: 0 leaves the controlling tty
 * to pm_alloc_cli_tty's round-robin, non-zero pins it to (requested_tty - 1). */
static int pm_spawn_from_buffer(uint32_t parent_pid, uint32_t requester_cap_mask,
                                const uint8_t* blob, uint32_t blob_size, const char* spawn_cli_args,
                                uint32_t spawn_cli_args_len, uint32_t requested_tty,
                                uint32_t* out_pid) {
    if (!blob || blob_size == 0 || !out_pid) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    pm_app_state_t* slot = pm_find_app_slot();
    if (!slot) {
        /* No idle slot and the kmem-backed apps list could not grow. Slots are
         * reused only once their app's process is reaped, so a list that keeps
         * growing means processes are being kept alive. */
        klog_write("[pm] app slot alloc failed; spawn refused\n");
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }
    pm_slot_reset(slot);

    wasmos_app_desc_t desc;
    wasmos_app_subsystem_info_t subsystem;
    uint8_t require_explicit_ready = 0;
    if (wasmos_app_parse(blob, blob_size, &desc) != 0) {
        return PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
    }
    if (wasmos_app_admit(&desc) != 0) {
        return PM_SPAWN_INTERNAL_ERR_UNTRUSTED;
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
        klog_write("[pm] blob storage alloc failed\n");
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }

    slot->started = 0;
    slot->requester_cap_mask = requester_cap_mask;
    slot->in_use = 1;
    slot->requested_tty = requested_tty;
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
             ? process_spawn_as_ready_gated_parked(
                   parent_pid, slot->name, pm_app_entry, slot, out_pid)
             : process_spawn_as_parked(parent_pid, slot->name, pm_app_entry, slot, out_pid)) != 0) {
        pm_slot_reset(slot);
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
    }
    slot->pid = *out_pid;
    pm_mark_from_image(*out_pid);
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

/* How long a spawn's filesystem read may take before the PM gives up on it.
 *
 * Generous: a cold read through a filesystem driver, a partition proxy and a
 * disk backend, on a machine still bringing itself up. It is a DEADLINE, not a
 * latency budget -- reaching it means the answer is never coming. */
#define PM_FS_REPLY_TIMEOUT_MS 15000u
/* One sleep's length. Only a backstop: a reply wakes the sleeper immediately, so
 * this decides how promptly the DEADLINE is noticed when no reply ever comes,
 * not how promptly a reply is seen. Long enough that a silent wait costs a
 * handful of wakes rather than a poll. */
#define PM_FS_REPLY_SLICE_MS 250u

/* Wait for the FS reply to `request_id`, bounded.
 *
 * The bound is the point. This blocked without one, so a filesystem that
 * accepted a read and then died -- a driver whose mount failed after the rule
 * spawned it -- parked the PROCESS MANAGER for the rest of the boot. Nothing
 * else spawns, so the system stops without a fault to look at: every process
 * blocked, fs-manager idle with an empty queue, and no clue which request was
 * outstanding.
 *
 * On the deadline the spawn fails with a reason its caller can report, which is
 * recoverable in a way a hung PM is not.
 *
 * A reply for another request is discarded, as before: ids are the PM's own and
 * an unmatched one is a stale reply to a request already abandoned. Each such
 * message restarts the wait, which is correct -- the deadline covers the reply
 * being ABSENT, and a message arriving proves the peer is alive. */
static int pm_recv_fs_reply(uint32_t source_context_id, uint32_t source_endpoint,
                            uint32_t request_id, ipc_message_t* out_msg) {
    ipc_message_t msg;
    /* The deadline is kept HERE, in ticks, because ipc_endpoint_wait_for cannot
     * report one: it returns IPC_OK whether it was woken or its timeout expired,
     * so its timeout bounds one sleep and says nothing about the whole wait. */
    const uint64_t deadline = timer_ticks() + timer_ms_to_ticks(PM_FS_REPLY_TIMEOUT_MS);
    for (;;) {
        int rc = ipc_recv_for(source_context_id, source_endpoint, &msg);
        if (rc == IPC_OK) {
            if (msg.request_id != request_id) {
                continue;
            }
            if (out_msg) {
                *out_msg = msg;
            }
            return 0;
        }
        if (rc != IPC_EMPTY) {
            return PM_SPAWN_INTERNAL_ERR_RECV;
        }
        if (timer_ticks() >= deadline) {
            klog_write("[pm] fs reply timed out; the filesystem never answered\n");
            return PM_SPAWN_INTERNAL_ERR_RECV;
        }
        /* Sleep rather than re-poll. The slice bounds one sleep so the deadline
         * above is re-tested even if no message ever arrives to wake us. */
        (void)ipc_endpoint_wait_for(source_context_id, source_endpoint, PM_FS_REPLY_SLICE_MS);
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
    if (pm_exec_tail_write(pm_fs_buf,
                           pm_fs_buf_size,
                           &tail_offset,
                           handler->handler_name,
                           (uint32_t)strlen(handler->handler_name),
                           &handler_name_offset,
                           1) != 0 ||
        pm_exec_tail_write(
            pm_fs_buf, pm_fs_buf_size, &tail_offset, path, path_len, &path_offset, 1) != 0 ||
        pm_exec_tail_write(pm_fs_buf,
                           pm_fs_buf_size,
                           &tail_offset,
                           cli_args,
                           args_len,
                           &args_offset,
                           args_len > 0u ? 1 : 0) != 0) {
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
    if (xfer_buffer_borrow(
            pmbuf, broker_context_id, BUFFER_BORROW_READ | BUFFER_BORROW_WRITE, &broker_borrow) !=
        WASMOS_ERR_NONE) {
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
    if (pm_recv_reply_matching(pm_context_id,
                               g_pm.broker_reply_endpoint,
                               request_id,
                               handler->broker_endpoint,
                               &reply) != 0) {
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
    if (!broker_fs_buf || wasmos_exec_broker_plan_validate(
                              broker_fs_buf + reply.arg0, reply.arg1, handler, out_plan) != 0) {
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
    if (pm_fs_read_blob_for_spawn(pm_context_id,
                                  pmbuf,
                                  out_resolved->path,
                                  out_resolved->path_len,
                                  &out_resolved->blob_size) != 0) {
        return WASMOS_ERR_PROC_SPAWN_FS_READ;
    }
    if (wasmos_exec_format_classify(
            out_resolved->path, pm_fs_buf, out_resolved->blob_size, &format_match) != 0) {
        return WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
    }
    if (format_match.kind != WASMOS_EXEC_FORMAT_BROKER) {
        return 0;
    }
    broker_rc = pm_request_broker_spawn_plan(pm_context_id,
                                             pmbuf,
                                             &format_match.handler,
                                             out_resolved->path,
                                             out_resolved->path_len,
                                             out_resolved->args_len > 0u ? out_resolved->args : 0,
                                             out_resolved->args_len,
                                             spawn_flags,
                                             out_resolved->blob_size,
                                             &broker_plan);
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
    /* Broker plans describe the final host invocation: after delegation, the
     * broker-supplied host args REPLACE the guest argv string; PM never
     * concatenates the two. */
    if (broker_plan.host_args_len > 0u) {
        memcpy(out_resolved->args, broker_plan.host_args, broker_plan.host_args_len);
        out_resolved->args[broker_plan.host_args_len] = '\0';
    }
    out_resolved->args_len = broker_plan.host_args_len;

    if (pm_fs_read_blob_for_spawn(pm_context_id,
                                  pmbuf,
                                  out_resolved->path,
                                  out_resolved->path_len,
                                  &out_resolved->blob_size) != 0) {
        return WASMOS_ERR_PROC_SPAWN_BROKER_PLAN;
    }
    if (wasmos_exec_format_classify(
            out_resolved->path, pm_fs_buf, out_resolved->blob_size, &format_match) != 0 ||
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

    /* The copy makes fs-manager hold state for the child, and the child may never
     * send fs-manager anything itself -- so nothing the child does would ever owe
     * fs-manager a hangup for it. The watch is that hangup. Without it the child
     * starts at the root instead: a directory nobody can release would pin its
     * mount once the child exits. */
    if (ipc_context_watch(child->context_id, g_pm.fs_endpoint) != IPC_OK) {
        klog_printf("[pm] cwd not inherited: no hangup record for context %u\n",
                    (unsigned)child->context_id);
        return PM_SPAWN_INTERNAL_ERR_ALLOC;
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
        /* Four different failures reach this point and only one of them is the
         * filesystem saying no.  The reason is reported because the caller
         * RETRIES: a boot that cannot read a service spawns this line thousands
         * of times, and without the discriminator every one of them is the same
         * unattributable string.  FS_IPC_ERROR carries the backend's packed code
         * in arg0 (send_fs_error in fs_manager.c), which is the only place that
         * code is ever visible -- it is dropped here otherwise. */
        klog_write("[pm] spawn_path fs read failed: ");
        for (uint32_t i = 0; i < path_len; ++i) {
            char c[2];
            c[0] = path[i];
            c[1] = '\0';
            klog_write(c);
        }
        if (recv_rc != 0) {
            klog_write(" (no reply)\n");
        } else if (reply.type == FS_IPC_ERROR) {
            klog_printf(" (fs: %s)\n", wasmos_strerror((wasmos_error_code_t)reply.arg0));
        } else if (reply.type != FS_IPC_RESP) {
            klog_printf(" (unexpected reply type=0x%x)\n", (unsigned)reply.type);
        } else {
            klog_printf(" (blob size %d outside 1..%u)\n", (int)reply.arg0, (unsigned)max);
        }
        return PM_SPAWN_INTERNAL_ERR_BAD_REPLY;
    }

    *out_blob_size = (uint32_t)reply.arg0;
    return 0;
}

/* ---- Spawn request handlers ----------------------------------------------
 * Every pm_handle_* below shares one convention: 0 means the handler has ALREADY
 * sent its own reply (or, for the *_sync forms, has armed g_pm.spawn so the
 * reply is sent later when the child reports ready).  A non-zero return is a
 * packed WASMOS_ERR_* code that the PM run loop converts into a PROC_IPC_ERROR
 * reply carrying it in arg1 — so a handler must never both return non-zero and
 * send a reply itself.  `msg` is borrowed for the call.
 *
 * The identity of the caller is always established the same way: resolve
 * msg->source's owning context, then the process behind that context.  That
 * process becomes the child's parent, which is what makes the child waitable and
 * killable by its spawner and nobody else.
 *
 * The *_sync forms complete on the child's READY rather than immediately.  Only
 * ONE such spawn can be in flight, because g_pm.spawn is a single slot; a second
 * is refused with WASMOS_ERR_PROC_PM_BUSY.  They arm the slot and then unpark
 * the child LAST, so the child cannot report ready before the slot describes who
 * to answer.
 */

/* Spawn by boot-module index (PROC_IPC_SPAWN_SYNC), replying when the child is
 * ready.  arg0 = module index, arg1 = timeout in ms (0 = wait indefinitely). */
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
    uint32_t requester_cap_mask = pm_requester_cap_mask(parent_pid);

    int spawn_rc = pm_spawn_module(parent_pid, requester_cap_mask, (uint32_t)msg->arg0, &child_pid);
    if (spawn_rc != 0) {
        klog_printf("[pm] spawn_module failed rc=%d\n", spawn_rc);
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

/* Spawn from a filesystem path (PROC_IPC_SPAWN_PATH_SYNC), replying when the
 * child is ready.  arg0 = PROC_SPAWN_PATH_FLAG_* request flags,
 * arg1 = (caller_buffer_id << 12) | path_len, arg2 = args_len, arg3 = timeout
 * in ms.  The path bytes live at offset 0 of the caller's transfer buffer named
 * by that id and, when args_len is not 0, args_len bytes of command-line
 * arguments directly after them; PM reads both directly (it is kernel) and
 * requires path_len < 256 and args_len < 256.
 *
 * The blob is read into a PM-OWNED per-operation buffer and the child is built
 * from that, so the caller cannot mutate the image after PM has validated it.
 * That buffer is released before the reply is armed, on both the success and the
 * failure paths. */
int pm_handle_spawn_path_sync(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t spawn_req_flags = (uint32_t)msg->arg0;
    uint32_t path_len = (uint32_t)msg->arg1 & 0xFFFu;
    uint32_t caller_buffer_id = (uint32_t)msg->arg1 >> 12;
    uint32_t args_len = (uint32_t)msg->arg2;
    uint32_t timeout_ms = (uint32_t)msg->arg3;
    const uint8_t* caller_fs_buf = 0;
    char path[256];
    char args[256];
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
    uint32_t requester_cap_mask = pm_requester_cap_mask(parent_pid);
    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || path_len == 0 || path_len >= sizeof(path) ||
        args_len >= sizeof(args)) {
        return WASMOS_ERR_PROC_PM_BAD_PATH;
    }
    caller_fs_buf = pm_foreign_xfer_ptr(caller_buffer_id, owner_context, 0);
    if (!caller_fs_buf || path_len + args_len >= xfer_buffer_size(BUFFER_KIND_TRANSFER)) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    for (uint32_t i = 0; i < path_len; ++i) {
        path[i] = (char)caller_fs_buf[i];
    }
    path[path_len] = '\0';
    for (uint32_t i = 0; i < args_len; ++i) {
        args[i] = (char)caller_fs_buf[path_len + i];
    }
    args[args_len] = '\0';
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) !=
        WASMOS_ERR_NONE) {
        return WASMOS_ERR_PROC_PM_NO_PM_FSBUF;
    }
    if (pm_resolve_spawn_path(pm_context_id,
                              &pmbuf,
                              path,
                              path_len,
                              args_len > 0u ? args : 0,
                              args_len,
                              spawn_req_flags,
                              &resolved) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_PATH_RESOLVE;
    }
    int spawn_rc = pm_spawn_from_buffer(parent_pid,
                                        requester_cap_mask,
                                        (const uint8_t*)pm_xfer_owner_ptr(&pmbuf),
                                        resolved.blob_size,
                                        resolved.args_len > 0u ? resolved.args : 0,
                                        resolved.args_len,
                                        PROC_SPAWN_PATH_TTY_OF(spawn_req_flags),
                                        &child_pid);
    if (spawn_rc != 0) {
        /* The internal reason, which the packed code cannot carry: several
         * distinct failures collapse into SPAWN_FAILED, and discarding which
         * one happened leaves a caller guessing. */
        klog_printf("[pm] spawn failed rc=%d\n", spawn_rc);
        pm_xfer_release(&pmbuf);
        return spawn_rc == PM_SPAWN_INTERNAL_ERR_UNTRUSTED ? WASMOS_ERR_PROC_SPAWN_UNTRUSTED
                                                           : WASMOS_ERR_PROC_PM_SPAWN_FAILED;
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

/* FIXME: the two PROC_IPC_ERROR replies below carry bare -1 (deadline expired)
 * and -2 (child died before READY) in arg1 rather than packed codes from
 * abi/errors.yaml, so a caller cannot decode either against the shared error
 * taxonomy. */
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
    if (!child || child->state == PROCESS_STATE_ZOMBIE ||
        __atomic_load_n(&child->exiting, __ATOMIC_ACQUIRE)) {
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

/* Poll for completion of an in-flight spawn request; called once per PM dispatch
 * from the run loop.  in_use == 0 means nothing is pending.  Only one spawn can
 * be in flight at a time (g_pm.spawn is a single slot; handlers refuse with
 * WASMOS_ERR_PROC_PM_BUSY while it is taken), and every in-flight spawn is a
 * synchronous one awaiting the child's READY, so pm_poll_sync_spawn is the only
 * driver.  The blob read itself is not in flight here: the path handlers read it
 * inline into a PM-owned per-operation buffer before the child is spawned. */
void pm_poll_spawn(uint32_t pm_context_id) {
    if (!g_pm.spawn.in_use) {
        return;
    }
    pm_poll_sync_spawn(pm_context_id);
}

/* A child announcing itself (PROC_IPC_NOTIFY_READY).  Records the liveness
 * endpoint it names (PROC_READY_FLAG_LIVENESS), latches the sender's ready flag,
 * completes an in-flight sync spawn if the sender IS that spawn's child, then
 * acks the sender so its blocking wasmos_sys_notify_ready() returns.  Returns 0,
 * or a packed WASMOS_ERR_PROC_PM_* code.
 *
 * A liveness endpoint the sender does not own is not recorded -- a probe there
 * would ask some other process -- and the ack carries
 * WASMOS_ERR_PROC_PM_BAD_ENDPOINT in arg0; READY is latched regardless.
 *
 * The sender identifies itself implicitly, by the context owning msg->source —
 * there is no pid argument, so one process cannot report another as ready.  The
 * ack is sent LAST, after the parent's spawn reply, so the child cannot resume
 * and tear down its endpoint before that reply has been built.  A notify from a
 * process that is not the pending spawn's child is still latched and acked; it
 * simply does not complete the spawn. */
int pm_handle_notify_ready(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* sender = 0;
    int32_t ack_code = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    sender = process_find_by_context(owner_context);
    if (!sender) {
        return WASMOS_ERR_PROC_PM_NO_CALLER;
    }
    if (((uint32_t)msg->arg1 & PROC_READY_FLAG_LIVENESS) != 0u) {
        uint32_t live_owner = 0;
        if (ipc_endpoint_owner((uint32_t)msg->arg0, &live_owner) == IPC_OK &&
            live_owner == owner_context) {
            sender->live_endpoint = (uint32_t)msg->arg0;
        } else {
            ack_code = WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
        }
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
    ack.arg0 = (uint32_t)ack_code;
    ack.arg1 = 0;
    ack.arg2 = 0;
    ack.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &ack) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

/* Sweeps the PROC_IPC_WAIT list once, replying for every waiter whose target has
 * exited and reaping that target afterwards.  Called once per PM dispatch — no
 * IPC wakes PM for a child's exit, so this poll is what makes waits complete.
 *
 * A waiter whose reply endpoint no longer resolves, or now belongs to a
 * different context than when the wait was registered, is DROPPED without a
 * reply: the endpoint id may have been recycled to another process, and sending
 * there would deliver one process's exit status to another.  A waiter whose
 * target has not exited is simply left for the next sweep, so a wait on a pid
 * that never exits occupies its slot indefinitely. */
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
        uint32_t exit_reason = 0;
        int rc = process_get_exit_info(waiter->pid, &exit_status, &exit_reason);
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
        resp.arg2 = exit_reason;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, waiter->reply_endpoint, &resp);
        waiter->in_use = 0;
        /* Status delivered — reap the zombie so its process slot is freed. */
        process_reap_zombie_pid(waiter->pid);
        waiter = (pm_wait_state_t*)list_next(&it);
    }
}

/* Collects PM's own exited children once per dispatch: for each in-use app slot
 * whose process has exited, stops the wasm instance, frees the slot's owned blob
 * pages and clears the slot.  `owner` is PM's own process_t, needed because
 * process_wait enforces the parent check — so this only reaps children PM
 * itself spawned, never a CLI's.
 *
 * Deliberately non-blocking: process_get_exit_status is tested first, so
 * process_wait is only ever reached on an already-exited child and cannot park
 * PM.  A still-running app is skipped and retried next dispatch. */
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

/* Spawn by boot-module index, replying IMMEDIATELY (PROC_IPC_SPAWN).  arg0 =
 * module index, arg1 = PROC_SPAWN_PATH_FLAG_* (only AUTOREAP is honoured here).
 * The reply carries the new pid in arg0 and is sent as soon as the child is
 * unparked, so a successful reply means "spawned", not "ready" — use the _SYNC
 * form when readiness matters.  Not gated on g_pm.spawn, so any number of these
 * can be issued back to back. */
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
    uint32_t requester_cap_mask = pm_requester_cap_mask(parent_pid);

    if (pm_spawn_module(parent_pid, requester_cap_mask, msg->arg0, &pid) != 0) {
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

/* Spawn by boot-module index with a full capability DESCRIPTOR
 * (PROC_IPC_SPAWN_CAPS_V2).  arg0 = module index, arg1 = the caller's
 * transfer-buffer id holding a wasmos_spawn_caps_v2_t followed by its region,
 * DMA-window and interrupt tails, arg2 = the payload's byte length,
 * arg3 = timeout in ms.
 *
 * With arg3 == 0 the reply is sent as soon as the child is unparked. With a
 * timeout it waits for the child's READY, like the other _SYNC spawns, and the
 * child is auto-reaped on exit: the device manager spawns its one-shot bus
 * enumerators (pci-bus, acpi-bus) this way and reads their inventory only once
 * they are ready.
 *
 * Validation is strict on purpose, since this grants device authority:
 * unknown cap_flags bits, a malformed or over-long grant list (see
 * pm_caps_from_v2), a DMA request with no direction, zero max_bytes or too many
 * windows, and a payload whose length does not match what the counts imply are
 * each refused with WASMOS_ERR_PROC_PM_BAD_CAPS before any process is created. */
int pm_handle_spawn_caps_v2(uint32_t pm_context_id, const ipc_message_t* msg) {
    pm_spawn_caps_t caps = {0};
    uint32_t owner_context = 0;
    process_t* caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    uint32_t payload_size = 0;
    const uint8_t* caps_payload = 0;
    uint32_t caps_buffer_size = 0;
    uint32_t timeout_ms = (uint32_t)msg->arg3;

    if (timeout_ms > 0 && g_pm.spawn.in_use) {
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
    uint32_t requester_cap_mask = pm_requester_cap_mask(parent_pid);
    /* arg1 names a transfer buffer, not a raw user pointer, and must be resolved
     * as one.  A user-VA resolve cannot see WARP's linear memory: it looks for a
     * MEM_REGION_WASM_LINEAR region, which reserved-VA linmem slots are not, and
     * falls back to treating the offset as an absolute user VA -- reading
     * unrelated memory instead of failing.  The xfer-buffer object resolves
     * through its backing pages, which is what every descriptor path here uses. */
    payload_size = (uint32_t)msg->arg2;
    if (msg->arg1 == 0 || payload_size < (uint32_t)sizeof(wasmos_spawn_caps_v2_t)) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    caps_payload = pm_foreign_xfer_ptr((uint32_t)msg->arg1, owner_context, &caps_buffer_size);
    if (!caps_payload || payload_size > caps_buffer_size) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    int caps_rc = pm_caps_from_v2(caps_payload, payload_size, &caps);
    if (caps_rc != 0) {
        return caps_rc;
    }
    if (!pm_caps_grant_allowed(owner_context, &caps)) {
        return WASMOS_ERR_PROC_PM_GRANT_NOT_AUTHORIZED;
    }

    if (pm_spawn_module(parent_pid, requester_cap_mask, msg->arg0, &pid) != 0) {
        return WASMOS_ERR_PROC_PM_INVALID_MODULE;
    }
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);
    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        (void)process_kill(pid, -1);
        return WASMOS_ERR_PROC_PM_CAPS_APPLY;
    }
    if (timeout_ms > 0) {
        (void)process_set_auto_reap(pid, 1);
        g_pm.spawn.in_use = 1;
        g_pm.spawn.is_sync = 1;
        g_pm.spawn.reply_endpoint = msg->source;
        g_pm.spawn.request_id = msg->request_id;
        g_pm.spawn.parent_pid = parent_pid;
        g_pm.spawn.parent_context_id = owner_context;
        g_pm.spawn.sync_child_pid = pid;
        g_pm.spawn.app_flags = 0;
        g_pm.spawn.sync_timeout_ticks = timer_ticks() + timer_ms_to_ticks(timeout_ms);
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
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}

/* Spawn from a filesystem path (PROC_IPC_SPAWN_PATH).  arg0 =
 * PROC_SPAWN_PATH_FLAG_*, arg1 = (caller_buffer_id << 12) | path_len,
 * arg2 = CLI argument byte length.  The caller's transfer buffer holds the path
 * at offset 0 and, when args_len is non-zero, the arguments at path_len + 1.
 *
 * The only handler whose reply timing depends on the IMAGE rather than the
 * opcode: if the parsed app declares that it must announce readiness, and the
 * request did not ask to DETACH, this behaves like the _SYNC forms — it takes
 * the single g_pm.spawn slot (refusing with WASMOS_ERR_PROC_PM_BUSY if taken)
 * and defers the reply.  Otherwise it replies as soon as the child is unparked,
 * with the app's flags in arg1.  A parse failure is not fatal: it just means no
 * flags and no ready gating, so a non-WASMOS-APP image still spawns. */
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
    uint32_t requester_cap_mask = pm_requester_cap_mask(parent_pid);
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
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) !=
        WASMOS_ERR_NONE) {
        return WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF;
    }

    int resolve_rc = pm_resolve_spawn_path(pm_context_id,
                                           &pmbuf,
                                           path,
                                           path_len,
                                           args_len > 0u ? cli_args : 0,
                                           args_len,
                                           spawn_req_flags,
                                           &resolved);
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
    int sfb_rc = pm_spawn_from_buffer(parent_pid,
                                      requester_cap_mask,
                                      (const uint8_t*)pm_xfer_owner_ptr(&pmbuf),
                                      resolved.blob_size,
                                      resolved.args_len > 0u ? resolved.args : 0,
                                      resolved.args_len,
                                      PROC_SPAWN_PATH_TTY_OF(spawn_req_flags),
                                      &pid);
    if (sfb_rc != 0) {
        /* Which step failed, because the code the caller gets cannot say.
         * WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED covers several distinct causes and
         * deliberately names none of them, on the grounds that the kernel
         * reports the one it hit -- but this path reported nothing at all, so a
         * spawn that died on PM_SPAWN_INTERNAL_ERR_ALLOC (no app slot: the
         * kmem-backed apps list could not grow) looked identical to one that
         * died on a malformed app. The sibling pm_spawn_module path has logged
         * its own "no slot" since it was written. */
        klog_printf("[pm] spawn_path: create failed rc=%d kmem_arena=%u/%u n=%u max=%u\n",
                    sfb_rc,
                    (unsigned)kmem_early_arena_used(),
                    (unsigned)kmem_early_arena_bytes(),
                    (unsigned)kmem_early_arena_count(),
                    (unsigned)kmem_early_arena_max_request());
        pm_xfer_release(&pmbuf);
        return sfb_rc == PM_SPAWN_INTERNAL_ERR_UNTRUSTED ? WASMOS_ERR_PROC_SPAWN_UNTRUSTED
                                                         : WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
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

/* The reply carries every record a package can declare, each name with its NUL,
 * so copying needs no truncation. */
_Static_assert(WASMOS_APP_MAX_DEPS == SPAWN_WATCH_DEPS_MAX,
               "a watched spawn reports the whole .wap dependency table");
_Static_assert(WASMOS_APP_DEP_NAME_MAX < SPAWN_WATCH_DEP_NAME_MAX,
               "a dependency name and its NUL fit wasmos_spawn_watch_dep_t.name");

/* Copies the child's [deps] into a watched spawn's reply fields. */
static void pm_watch_desc_fill_deps(wasmos_spawn_watch_desc_t* out, const wasmos_app_desc_t* desc) {
    __builtin_memset(out->deps, 0, sizeof(out->deps));
    for (uint32_t i = 0; i < desc->dep_count; ++i) {
        const wasmos_app_dep_t* dep = &desc->deps[i];
        wasmos_spawn_watch_dep_t* d = &out->deps[i];
        d->strength = dep->strength;
        d->kind = dep->kind;
        d->name_len = (uint8_t)dep->name_len;
        __builtin_memcpy(d->name, dep->name, dep->name_len);
    }
    out->dep_count = desc->dep_count;
}

/* Spawn from a path with the spawner watching (PROC_IPC_SPAWN_WATCHED):
 * arg0 = buffer_id of a transfer buffer the caller owns, arg1 = offset of the
 * wasmos_spawn_watch_desc_t in it, arg2 = size of the region the PM may use.
 * With SPAWN_WATCH_FLAG_MODULE the path is a boot module's app name and the
 * child is built from the module image, which needs no filesystem.
 *
 * Both lifecycle messages are reserved before the child exists and attached
 * before it first runs, so neither can be lost: no reservation, no child
 * (WATCH_NO_SPACE). The child is auto-reaped, which is what delivers
 * CHILD_EXITED as soon as it exits. Never ready-gates and never touches the
 * single g_pm.spawn slot.
 *
 * The reply is sent while the child is still parked, so on any endpoint it
 * precedes the child's CHILD_READY. A reply that cannot be sent kills the
 * parked child; the spawner then sees only its CHILD_EXITED, KILLED.
 *
 * Returns 0 once the reply is sent, or a packed WASMOS_ERR_PROC_SPAWN_* code
 * with no child left behind. */
int pm_handle_spawn_watched(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    uint32_t notify_owner = 0;
    uint32_t buffer_size = 0;
    uint32_t region_off = (uint32_t)msg->arg1;
    uint32_t region_size = (uint32_t)msg->arg2;
    process_t* caller = 0;
    uint8_t* region = 0;
    wasmos_spawn_watch_desc_t req;
    char path[SPAWN_WATCH_PATH_MAX + 1];
    char cli_args[SPAWN_WATCH_ARGS_MAX + 1];
    uint8_t caps_bytes[WASMOS_SPAWN_CAPS_V2_SIZE(
        WASMOS_REGION_LIMIT, PM_DMA_WINDOW_LIMIT, WASMOS_IRQ_GRANT_LIMIT)];
    pm_spawn_caps_t caps = {0};
    ipc_owed_t* on_ready = 0;
    ipc_owed_t* on_exit = 0;
    pm_resolved_spawn_path_t resolved;
    xfer_buffer_owner_t pmbuf = {0};
    wasmos_app_desc_t desc;
    uint32_t pid = 0;
    uint8_t from_module = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return WASMOS_ERR_PROC_SPAWN_BAD_ENDPOINT;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return WASMOS_ERR_PROC_SPAWN_NO_CALLER;
    }
    uint32_t requester_cap_mask = pm_requester_cap_mask(caller->pid);
    region =
        (uint8_t*)(uintptr_t)pm_foreign_xfer_ptr((uint32_t)msg->arg0, owner_context, &buffer_size);
    if (!region || region_size < sizeof(req) ||
        (uint64_t)region_off + region_size > (uint64_t)buffer_size) {
        return WASMOS_ERR_PROC_SPAWN_BAD_DESC;
    }
    region += region_off;
    /* One copy of the request: the caller can write its buffer at any time, so
     * every check below is made on bytes it can no longer change. */
    __builtin_memcpy(&req, region, sizeof(req));
    if (req.version != SPAWN_WATCH_DESC_VERSION || req.path_len == 0 ||
        req.path_len > SPAWN_WATCH_PATH_MAX || req.args_len > SPAWN_WATCH_ARGS_MAX ||
        (uint64_t)req.path_off + req.path_len > region_size ||
        (uint64_t)req.args_off + req.args_len > region_size || req.caps_len > sizeof(caps_bytes) ||
        (uint64_t)req.caps_off + req.caps_len > region_size ||
        (req.flags & ~(uint32_t)SPAWN_WATCH_FLAG_MODULE) != 0u) {
        return WASMOS_ERR_PROC_SPAWN_BAD_DESC;
    }
    /* A boot module is spawned from its in-memory image, so it needs no
     * filesystem: this is how the services the filesystem itself depends on are
     * started. pm_spawn_module takes neither arguments nor a tty pin. */
    from_module = (req.flags & SPAWN_WATCH_FLAG_MODULE) != 0u;
    if (from_module && (req.args_len != 0u || req.tty != 0u)) {
        return WASMOS_ERR_PROC_SPAWN_BAD_DESC;
    }
    if (!from_module && g_pm.fs_endpoint == IPC_ENDPOINT_NONE) {
        return WASMOS_ERR_PROC_SPAWN_BAD_PATH;
    }
    if (req.caps_len > 0u) {
        __builtin_memcpy(caps_bytes, region + req.caps_off, req.caps_len);
        if (pm_caps_from_v2(caps_bytes, req.caps_len, &caps) != 0) {
            return WASMOS_ERR_PROC_SPAWN_BAD_CAPS;
        }
    }
    if (!pm_caps_grant_allowed(owner_context, &caps)) {
        return WASMOS_ERR_PROC_SPAWN_GRANT_NOT_AUTHORIZED;
    }
    __builtin_memcpy(path, region + req.path_off, req.path_len);
    path[req.path_len] = '\0';
    __builtin_memcpy(cli_args, region + req.args_off, req.args_len);
    cli_args[req.args_len] = '\0';

    if (ipc_endpoint_owner(req.notify_endpoint, &notify_owner) != IPC_OK ||
        notify_owner != owner_context) {
        return WASMOS_ERR_PROC_SPAWN_NOTIFY_NOT_OWNED;
    }
    if (ipc_owed_reserve(
            req.notify_endpoint, g_pm.proc_endpoint, PROC_IPC_CHILD_READY, &on_ready) != IPC_OK ||
        ipc_owed_reserve(
            req.notify_endpoint, g_pm.proc_endpoint, PROC_IPC_CHILD_EXITED, &on_exit) != IPC_OK) {
        ipc_owed_cancel(on_ready);
        return WASMOS_ERR_PROC_SPAWN_WATCH_NO_SPACE;
    }

    if (from_module) {
        uint32_t index = wasmos_app_boot_module_index(g_pm.boot_info, path, req.path_len);
        const boot_module_t* mod = pm_module_at(index);
        if (!mod) {
            ipc_owed_cancel(on_ready);
            ipc_owed_cancel(on_exit);
            return WASMOS_ERR_PROC_SPAWN_NO_MODULE;
        }
        /* desc points into the module image, which lives as long as the kernel. */
        if (wasmos_app_parse((const uint8_t*)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) !=
                0 ||
            pm_spawn_module(caller->pid, requester_cap_mask, index, &pid) != 0) {
            ipc_owed_cancel(on_ready);
            ipc_owed_cancel(on_exit);
            return WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
        }
    } else {
        if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) !=
            WASMOS_ERR_NONE) {
            ipc_owed_cancel(on_ready);
            ipc_owed_cancel(on_exit);
            return WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF;
        }
        int resolve_rc = pm_resolve_spawn_path(pm_context_id,
                                               &pmbuf,
                                               path,
                                               req.path_len,
                                               req.args_len > 0u ? cli_args : 0,
                                               req.args_len,
                                               0,
                                               &resolved);
        if (resolve_rc != 0) {
            pm_xfer_release(&pmbuf);
            ipc_owed_cancel(on_ready);
            ipc_owed_cancel(on_exit);
            return resolve_rc;
        }
        const uint8_t* blob = (const uint8_t*)pm_xfer_owner_ptr(&pmbuf);
        int sfb_rc = PM_SPAWN_INTERNAL_ERR_BAD_ARGS;
        if (wasmos_app_parse(blob, resolved.blob_size, &desc) != 0 ||
            (sfb_rc = pm_spawn_from_buffer(caller->pid,
                                           requester_cap_mask,
                                           blob,
                                           resolved.blob_size,
                                           resolved.args_len > 0u ? resolved.args : 0,
                                           resolved.args_len,
                                           req.tty,
                                           &pid)) != 0) {
            pm_xfer_release(&pmbuf);
            ipc_owed_cancel(on_ready);
            ipc_owed_cancel(on_exit);
            return sfb_rc == PM_SPAWN_INTERNAL_ERR_UNTRUSTED ? WASMOS_ERR_PROC_SPAWN_UNTRUSTED
                                                             : WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
        }
    }

    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        pm_xfer_release(&pmbuf);
        ipc_owed_cancel(on_ready);
        ipc_owed_cancel(on_exit);
        (void)process_kill(pid, -1);
        (void)process_set_auto_reap(pid, 1);
        return WASMOS_ERR_PROC_SPAWN_CAPS_APPLY;
    }

    /* The reply fields, while desc still points into the blob. */
    req.pid = pid;
    req.app_flags = desc.flags;
    req.reserved1 = 0;
    pm_watch_desc_fill_deps(&req, &desc);
    pm_xfer_release(&pmbuf);

    if (process_set_watch(pid, on_ready, on_exit) != 0) {
        ipc_owed_cancel(on_ready);
        ipc_owed_cancel(on_exit);
        (void)process_kill(pid, -1);
        (void)process_set_auto_reap(pid, 1);
        return WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
    }
    (void)process_set_auto_reap(pid, 1);
    (void)pm_inherit_child_cwd(pm_context_id, owner_context, pid);
    __builtin_memcpy(region + __builtin_offsetof(wasmos_spawn_watch_desc_t, pid),
                     &req.pid,
                     sizeof(req) - __builtin_offsetof(wasmos_spawn_watch_desc_t, pid));

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = desc.flags;
    resp.arg2 = 0;
    resp.arg3 = 0;
    if (ipc_send_from(pm_context_id, msg->source, &resp) != IPC_OK) {
        (void)process_kill(pid, -1);
        return WASMOS_ERR_PROC_PM_REPLY_SEND;
    }
    process_unpark_pid(pid);
    return 0;
}

/* What a boot module declares about itself: its capability flags, its declared
 * register windows, and whether the early storage path depends on it
 * (PROC_IPC_MODULE_META_DESC).  arg0 = module index.
 *
 * The answer is written into a transfer buffer the caller owns (arg2 = its
 * buffer_id, arg3 = the offset to write at), because the declared region list is
 * variable-length and does not fit in four reply words.
 *
 * Returns 0 once the PROC_IPC_RESP is sent, or a packed WASMOS_ERR_PROC_PM_*
 * code -- META_LOOKUP for an unknown module index, META_NOT_DRIVER for a module
 * without the driver flag, CALLER_FSBUF for a buffer that cannot hold the
 * descriptor at the requested offset. */
/* What a driver package declares about itself, as both metadata requests report
 * it: its DEVMGR capability bits, storage-bootstrap flag, register windows and
 * DMA budget (the dma.buffer capability's flags, in pages). */
static void pm_module_meta_fill(const wasmos_app_desc_t* desc, wasmos_module_meta_desc_t* out) {
    __builtin_memset(out, 0, sizeof(*out));
    out->version = WASMOS_MODULE_META_DESC_VERSION;
    out->storage_bootstrap = ((desc->flags & WASMOS_APP_FLAG_STORAGE_BOOTSTRAP) != 0) ? 1u : 0u;
    out->cap_flags = wasmos_app_driver_cap_flags(desc);
    out->region_count = desc->region_count;
    for (uint32_t i = 0; i < desc->region_count && i < WASMOS_MODULE_META_MAX_REGIONS; ++i) {
        out->regions[i].kind = desc->regions[i].kind;
        out->regions[i].bar_index = desc->regions[i].bar_index;
        out->regions[i].first = desc->regions[i].first;
        out->regions[i].last = desc->regions[i].last;
    }
    for (uint32_t i = 0; i < desc->cap_count; ++i) {
        if (str_eq_bytes(desc->caps[i].name, desc->caps[i].name_len, "dma.buffer")) {
            out->dma_budget_pages = desc->caps[i].flags;
        }
    }
}

int pm_handle_module_meta_desc(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    process_t* caller = 0;
    wasmos_app_desc_t desc;
    wasmos_module_meta_desc_t out;
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

    buf = pm_foreign_xfer_ptr((uint32_t)msg->arg2, owner_context, &buffer_size);
    if (!buf || (uint64_t)offset + sizeof(out) > (uint64_t)buffer_size) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }

    pm_module_meta_fill(&desc, &out);
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

/* Resolves a module by initfs PATH rather than index (PROC_IPC_MODULE_META_PATH)
 * and replies with arg0 = module index, arg1 = app flags, arg2 = driver cap
 * flags.  arg0 = a raw user VA for the path (0 to use a transfer buffer
 * instead), arg1 = (caller_buffer_id << 12) | path_len, arg2 = the source.
 *
 * Only PROC_MODULE_SOURCE_INITFS is implemented; PROC_MODULE_SOURCE_FS and any
 * other value are both refused with WASMOS_ERR_PROC_PM_META_BAD_SOURCE.  Unlike
 * the other meta handlers this does NOT require the module to be a driver — it
 * is the lookup that tells a caller what a path contains.  path_len must be
 * under 96. */
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

/* What the package at a filesystem path declares about itself
 * (PROC_IPC_MODULE_META_FILE): read through fs-manager as a path spawn reads
 * it, parsed by the same package parser, and reported as the
 * wasmos_module_meta_desc_t PROC_IPC_MODULE_META_DESC gives for a boot module.
 * device-manager needs it to resolve the windows a driver on /boot declares,
 * which it cannot do from the boot image. The descriptor and its path live in
 * the caller's buffer, which the caller owns and has lent READ|WRITE. */
int pm_handle_module_meta_file(uint32_t pm_context_id, const ipc_message_t* msg) {
    uint32_t owner_context = 0;
    uint32_t buffer_size = 0;
    uint32_t region_off = (uint32_t)msg->arg1;
    uint32_t region_size = (uint32_t)msg->arg2;
    uint8_t* region = 0;
    wasmos_module_meta_file_desc_t req;
    char path[SPAWN_WATCH_PATH_MAX + 1];
    xfer_buffer_owner_t pmbuf = {0};
    wasmos_app_desc_t desc;
    uint32_t blob_size = 0;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK ||
        !process_find_by_context(owner_context)) {
        return WASMOS_ERR_PROC_PM_BAD_ENDPOINT;
    }
    region =
        (uint8_t*)(uintptr_t)pm_foreign_xfer_ptr((uint32_t)msg->arg0, owner_context, &buffer_size);
    if (!region || (uint64_t)region_off + region_size > (uint64_t)buffer_size) {
        return WASMOS_ERR_PROC_PM_CALLER_FSBUF;
    }
    region += region_off;
    if (region_size < sizeof(req)) {
        return WASMOS_ERR_PROC_PM_META_BAD_DESC;
    }
    __builtin_memcpy(&req, region, sizeof(req));
    if (req.version != MODULE_META_FILE_DESC_VERSION || req.path_len == 0u ||
        req.path_len > SPAWN_WATCH_PATH_MAX || req.path_off < sizeof(req) ||
        (uint64_t)req.path_off + req.path_len > (uint64_t)region_size) {
        return WASMOS_ERR_PROC_PM_META_BAD_DESC;
    }
    __builtin_memcpy(path, region + req.path_off, req.path_len);
    path[req.path_len] = '\0';
    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE) {
        return WASMOS_ERR_PROC_PM_FS_UNAVAILABLE;
    }
    if (pm_xfer_acquire(pm_context_id, xfer_buffer_size(BUFFER_KIND_TRANSFER), &pmbuf) !=
        WASMOS_ERR_NONE) {
        return WASMOS_ERR_PROC_PM_NO_PM_FSBUF;
    }
    if (pm_fs_read_blob_for_spawn(pm_context_id, &pmbuf, path, req.path_len, &blob_size) != 0 ||
        wasmos_app_parse((const uint8_t*)pm_xfer_owner_ptr(&pmbuf), blob_size, &desc) != 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_META_LOOKUP;
    }
    if ((desc.flags & WASMOS_APP_FLAG_DRIVER) == 0) {
        pm_xfer_release(&pmbuf);
        return WASMOS_ERR_PROC_PM_META_NOT_DRIVER;
    }
    /* desc points into the blob, so fill before the buffer goes. */
    pm_module_meta_fill(&desc, &req.meta);
    pm_xfer_release(&pmbuf);
    __builtin_memcpy(region, &req, sizeof(req));

    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = (int32_t)sizeof(req.meta);
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK
               ? 0
               : WASMOS_ERR_PROC_PM_REPLY_SEND;
}
