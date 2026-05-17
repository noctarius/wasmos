#include "process_manager_internal.h"
#include "klog.h"
#include "process_manager.h"
#include "capability.h"
#include "memory.h"
#include "native_driver.h"
#include "string.h"
#include "wasm_chardev.h"
#include "wasmos_app_meta.h"

static const boot_module_t *
pm_module_at(uint32_t index)
{
    const boot_info_t *info = g_pm.boot_info;
    if (!info || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return 0;
    }
    if (index >= info->module_count) {
        return 0;
    }
    const uint8_t *mods = (const uint8_t *)info->modules;
    return (const boot_module_t *)(mods + index * info->module_entry_size);
}

uint32_t
pm_find_module_index_by_name(const char *name)
{
    const boot_info_t *info = g_pm.boot_info;
    if (!info || !name || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0xFFFFFFFFu;
    }

    for (uint32_t i = 0; i < info->module_count; ++i) {
        const boot_module_t *mod = pm_module_at(i);
        if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP ||
            mod->base == 0 || mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
            continue;
        }
        wasmos_app_desc_t desc;
        if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
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

static pm_app_state_t *
pm_find_app_slot(void)
{
    for (uint32_t i = 0; i < PM_MAX_MANAGED_APPS; ++i) {
        if (!g_pm.apps[i].in_use) {
            return &g_pm.apps[i];
        }
    }
    return 0;
}

typedef enum {
    PM_ARG_NONE = 0,
    PM_ARG_PROC_ENDPOINT,
    PM_ARG_MODULE_COUNT,
    PM_ARG_INIT_MODULE_INDEX,
    PM_ARG_BLOCK_ENDPOINT,
    PM_ARG_CLI_TTY_ALLOC,
    PM_ARG_CHARDEV_ENDPOINT,
    PM_ARG_CONST_NEG1
} pm_arg_kind_t;

static pm_arg_kind_t
pm_arg_kind_from_binding(const uint8_t *name, uint32_t name_len)
{
    if (str_eq_bytes(name, name_len, "none")) return PM_ARG_NONE;
    if (str_eq_bytes(name, name_len, "proc.endpoint")) return PM_ARG_PROC_ENDPOINT;
    if (str_eq_bytes(name, name_len, "module.count")) return PM_ARG_MODULE_COUNT;
    if (str_eq_bytes(name, name_len, "init.module.index")) return PM_ARG_INIT_MODULE_INDEX;
    if (str_eq_bytes(name, name_len, "block.endpoint")) return PM_ARG_BLOCK_ENDPOINT;
    if (str_eq_bytes(name, name_len, "cli.tty.alloc")) return PM_ARG_CLI_TTY_ALLOC;
    if (str_eq_bytes(name, name_len, "chardev.endpoint")) return PM_ARG_CHARDEV_ENDPOINT;
    if (str_eq_bytes(name, name_len, "const.neg1")) return PM_ARG_CONST_NEG1;
    return PM_ARG_NONE;
}

static int
pm_resolve_pre_spawn_arg(pm_arg_kind_t kind, uint32_t *out_value)
{
    if (!out_value) {
        return -1;
    }
    switch (kind) {
    case PM_ARG_NONE:
        *out_value = 0;
        return 0;
    case PM_ARG_PROC_ENDPOINT:
        if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE) return -1;
        *out_value = g_pm.proc_endpoint;
        return 0;
    case PM_ARG_MODULE_COUNT:
        *out_value = g_pm.module_count;
        return 0;
    case PM_ARG_INIT_MODULE_INDEX:
        *out_value = g_pm.init_module_index;
        return 0;
    case PM_ARG_BLOCK_ENDPOINT:
        if (g_pm.block_endpoint == IPC_ENDPOINT_NONE) return -1;
        *out_value = g_pm.block_endpoint;
        return 0;
    case PM_ARG_CLI_TTY_ALLOC:
        *out_value = pm_alloc_cli_tty();
        return 0;
    case PM_ARG_CHARDEV_ENDPOINT: {
        uint32_t ep = IPC_ENDPOINT_NONE;
        if (wasm_chardev_endpoint(&ep) != 0 || ep == IPC_ENDPOINT_NONE) return -1;
        *out_value = ep;
        return 0;
    }
    case PM_ARG_CONST_NEG1:
        *out_value = (uint32_t)-1;
        return 0;
    default:
        return -1;
    }
}

static int
pm_apply_entry_bindings(pm_app_state_t *slot, const wasmos_app_desc_t *desc)
{
    if (!slot || !desc) {
        return -1;
    }
    slot->entry_argc = 4;
    slot->entry_arg0 = 0;
    slot->entry_arg1 = 0;
    slot->entry_arg2 = 0;
    slot->entry_arg3 = 0;
    for (uint32_t i = 0; i < desc->entry_arg_binding_count && i < 4; ++i) {
        pm_arg_kind_t kind = pm_arg_kind_from_binding(desc->entry_arg_bindings[i].name,
                                                      desc->entry_arg_bindings[i].name_len);
        uint32_t value = 0;
        if (pm_resolve_pre_spawn_arg(kind, &value) != 0) {
            return -1;
        }
        if (i == 0) slot->entry_arg0 = value;
        else if (i == 1) slot->entry_arg1 = value;
        else if (i == 2) slot->entry_arg2 = value;
        else slot->entry_arg3 = value;
    }
    return 0;
}

static int
pm_apply_post_spawn_bindings(pm_app_state_t *slot, uint32_t pid)
{
    (void)pid;
    (void)slot;
    return 0;
}

static process_run_result_t
pm_app_entry(process_t *process, void *arg)
{
    pm_app_state_t *state = (pm_app_state_t *)arg;

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
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        state->flags = desc.flags;
        uint32_t init_args[4] = {
            state->entry_arg0,
            state->entry_arg1,
            state->entry_arg2,
            state->entry_arg3
        };

        if (desc.flags & WASMOS_APP_FLAG_NATIVE) {
            int native_rc = native_driver_start(process->context_id,
                                                desc.wasm_bytes,
                                                desc.wasm_size,
                                                state->name,
                                                init_args,
                                                state->entry_argc);
            process_set_exit_status(process, native_rc == 0 ? 0 : -1);
            wasmos_app_stop(&state->app);
            state->in_use = 0;
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }

        if (wasmos_app_start(&state->app,
                             &desc,
                             process->context_id,
                             init_args,
                             state->entry_argc) != 0) {
            klog_write("[pm] app start failed\n");
            process_set_exit_status(process, -1);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        state->started = 1;
    }

    {
        int entry_rc = wasmos_app_call_entry(&state->app);
        if (entry_rc != 0) {
            klog_write("[pm] app entry failed\n");
            process_set_exit_status(process, -1);
        } else {
            process_set_exit_status(process, 0);
        }
    }

    wasmos_app_stop(&state->app);
    state->in_use = 0;
    state->pid = 0;
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_enable();
#endif
    return PROCESS_RUN_EXITED;
}

static int
pm_spawn_module(uint32_t parent_pid, uint32_t module_index, uint32_t *out_pid)
{
    const boot_module_t *mod = pm_module_at(module_index);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
        mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
        return -1;
    }

    pm_app_state_t *slot = pm_find_app_slot();
    if (!slot) {
        return -1;
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
        return -1;
    }

    if (str_copy_bytes(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        return -1;
    }

    slot->blob = (const uint8_t *)(uintptr_t)mod->base;
    slot->blob_size = (uint32_t)mod->size;
    slot->started = 0;
    slot->in_use = 1;
    if (pm_apply_entry_bindings(slot, &desc) != 0) {
        slot->in_use = 0;
        return -1;
    }

    preempt_disable();
    if (process_spawn_as(parent_pid, slot->name, pm_app_entry, slot, out_pid) != 0) {
        preempt_enable();
        slot->in_use = 0;
        return -1;
    }

    slot->pid = *out_pid;
    if (pm_apply_post_spawn_bindings(slot, *out_pid) != 0) {
        preempt_enable();
        slot->in_use = 0;
        return -1;
    }
    preempt_enable();
    return 0;
}

static int
pm_apply_spawn_caps(uint32_t pid, const pm_spawn_caps_t *caps)
{
    process_t *proc = 0;
    uint32_t packed_io = 0;
    if (!caps || !caps->valid) {
        return 0;
    }
    proc = process_get(pid);
    if (!proc || proc->context_id == 0) {
        return -1;
    }
    packed_io = ((uint32_t)caps->io_port_min) | ((uint32_t)caps->io_port_max << 16);
    if (capability_set_spawn_profile(proc->context_id,
                                     caps->cap_flags,
                                     (uint16_t)(packed_io & 0xFFFFu),
                                     (uint16_t)((packed_io >> 16) & 0xFFFFu),
                                     caps->irq_mask,
                                     caps->dma_direction_flags,
                                     caps->dma_max_bytes,
                                     caps->dma_window_base,
                                     caps->dma_window_length) != 0) {
        return -1;
    }
    return 0;
}

static int
pm_spawn_from_buffer(uint32_t parent_pid, const uint8_t *blob, uint32_t blob_size, uint32_t *out_pid)
{
    if (!blob || blob_size == 0 || !out_pid) {
        return -1;
    }
    pm_app_state_t *slot = pm_find_app_slot();
    if (!slot) {
        return -1;
    }
    if (blob_size > sizeof(slot->blob_storage)) {
        return -1;
    }

    for (uint32_t i = 0; i < blob_size; ++i) {
        slot->blob_storage[i] = blob[i];
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse(slot->blob_storage, blob_size, &desc) != 0) {
        return -1;
    }
    if ((desc.flags & (WASMOS_APP_FLAG_APP |
                       WASMOS_APP_FLAG_SERVICE |
                       WASMOS_APP_FLAG_DRIVER)) == 0) {
        return -1;
    }
    if (str_copy_bytes(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        return -1;
    }

    slot->blob = slot->blob_storage;
    slot->blob_size = blob_size;
    slot->started = 0;
    slot->in_use = 1;
    if (pm_apply_entry_bindings(slot, &desc) != 0) {
        slot->in_use = 0;
        return -1;
    }

    if (process_spawn_as(parent_pid, slot->name, pm_app_entry, slot, out_pid) != 0) {
        slot->in_use = 0;
        return -1;
    }
    slot->pid = *out_pid;
    if (pm_apply_post_spawn_bindings(slot, *out_pid) != 0) {
        slot->in_use = 0;
        return -1;
    }
    return 0;
}

static int
pm_send_fs_read(uint32_t pm_context_id, const char *name, uint32_t *out_req_id)
{
    if (!name || !out_req_id || g_pm.fs_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    uint32_t args[4];
    pm_pack_name_args(name, args);

    ipc_message_t req;
    req.type = FS_IPC_READ_APP_REQ;
    req.source = g_pm.fs_reply_endpoint;
    req.destination = g_pm.fs_endpoint;
    req.request_id = g_pm.fs_request_id++;
    req.arg0 = args[0];
    req.arg1 = args[1];
    req.arg2 = args[2];
    req.arg3 = args[3];
    if (ipc_send_from(pm_context_id, g_pm.fs_endpoint, &req) != IPC_OK) {
        return -1;
    }
    *out_req_id = req.request_id;
    return 0;
}

void
pm_poll_spawn(uint32_t pm_context_id)
{
    if (!g_pm.spawn.in_use || g_pm.fs_reply_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }

    ipc_message_t msg;
    int recv_rc = ipc_recv_for(pm_context_id, g_pm.fs_reply_endpoint, &msg);
    if (recv_rc == IPC_EMPTY) {
        return;
    }
    g_pm.spawn.in_use = 0;
    if (recv_rc != IPC_OK ||
        msg.request_id != g_pm.spawn.fs_request_id ||
        msg.type != FS_IPC_RESP) {
        ipc_message_t resp;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = PROC_IPC_SPAWN_NAME;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
        return;
    }

    uint32_t pid = 0;
    uint32_t size = (uint32_t)msg.arg0;
    const uint8_t *fs_blob = (const uint8_t *)process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, pm_context_id);
    if (size == 0 || size > process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM) || !fs_blob ||
        pm_spawn_from_buffer(g_pm.spawn.parent_pid, fs_blob, size, &pid) != 0) {
        ipc_message_t resp;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = PROC_IPC_SPAWN_NAME;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
        return;
    }

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = g_pm.spawn.reply_endpoint;
    resp.request_id = g_pm.spawn.request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
}

void
pm_check_waits(uint32_t pm_context_id)
{
    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        pm_wait_state_t *waiter = &g_pm.waits[i];
        uint32_t reply_owner_context = 0;
        if (!waiter->in_use) {
            continue;
        }
        if (ipc_endpoint_owner(waiter->reply_endpoint, &reply_owner_context) != IPC_OK ||
            reply_owner_context != waiter->owner_context_id) {
            if (!g_pm_wait_owner_deny_logged) {
                g_pm_wait_owner_deny_logged = 1;
                klog_write("[test] pm wait reply owner deny ok\n");
            }
            waiter->in_use = 0;
            continue;
        }
        int32_t exit_status = 0;
        int rc = process_get_exit_status(waiter->pid, &exit_status);
        if (rc != 0) {
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
    }
}

void
pm_reap_apps(process_t *owner)
{
    if (!owner) {
        return;
    }
    for (uint32_t i = 0; i < PM_MAX_MANAGED_APPS; ++i) {
        pm_app_state_t *app = &g_pm.apps[i];
        if (!app->in_use || app->pid == 0) {
            continue;
        }
        int32_t exit_status = 0;
        if (process_get_exit_status(app->pid, &exit_status) != 0) {
            continue;
        }
        if (process_wait(owner, app->pid, &exit_status) != 0) {
            continue;
        }
        wasmos_app_stop(&app->app);
        app->in_use = 0;
        app->pid = 0;
    }
}

int
pm_handle_spawn(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        if (!g_pm_spawn_owner_deny_logged) {
            g_pm_spawn_owner_deny_logged = 1;
            klog_write("[test] pm spawn owner deny ok\n");
        }
        return -1;
    }
    parent_pid = caller->pid;

    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return -1;
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
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

int
pm_handle_spawn_caps(uint32_t pm_context_id, const ipc_message_t *msg)
{
    pm_spawn_caps_t caps = {0};
    uint32_t owner_context = 0;
    process_t *caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    parent_pid = caller->pid;
    caps.valid = 1;
    caps.cap_flags = msg->arg1;
    caps.io_port_min = (uint16_t)((uint32_t)msg->arg2 & 0xFFFFu);
    caps.io_port_max = (uint16_t)(((uint32_t)msg->arg2 >> 16) & 0xFFFFu);
    caps.irq_mask = (uint16_t)((uint32_t)msg->arg3 & 0xFFFFu);
    caps.dma_direction_flags = 0;
    caps.dma_max_bytes = 0;
    caps.dma_window_base = 0;
    caps.dma_window_length = 0;
    if ((caps.cap_flags & DEVMGR_CAP_IO_PORT) == 0) {
        caps.io_port_min = 0;
        caps.io_port_max = 0;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        /* FIXME: PROC_IPC_SPAWN_CAPS cannot carry DMA descriptor schema.
         * Accept DMA only via PROC_IPC_SPAWN_CAPS_V2 once wired end-to-end. */
        return -1;
    }
    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return -1;
    }
    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        return -1;
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
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

int
pm_handle_spawn_caps_v2(uint32_t pm_context_id, const ipc_message_t *msg)
{
    pm_spawn_caps_t caps = {0};
    wasmos_spawn_caps_v2_t in_caps;
    uint32_t owner_context = 0;
    process_t *caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    uint32_t known_cap_mask = DEVMGR_CAP_IO_PORT |
                              DEVMGR_CAP_MMIO_MAP |
                              DEVMGR_CAP_IRQ |
                              DEVMGR_CAP_DMA;
    uint64_t win_end = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    parent_pid = caller->pid;
    if (msg->arg1 == 0 || (uint32_t)msg->arg2 != (uint32_t)sizeof(in_caps)) {
        return -1;
    }
    if (mm_copy_from_user(owner_context,
                          &in_caps,
                          (uint64_t)(uint32_t)msg->arg1,
                          sizeof(in_caps)) != 0) {
        return -1;
    }

    if ((in_caps.cap_flags & ~known_cap_mask) != 0) {
        return -1;
    }
    if ((in_caps.cap_flags & DEVMGR_CAP_IO_PORT) != 0 &&
        in_caps.io_port_min > in_caps.io_port_max) {
        return -1;
    }

    caps.valid = 1;
    caps.cap_flags = in_caps.cap_flags;
    caps.io_port_min = in_caps.io_port_min;
    caps.io_port_max = in_caps.io_port_max;
    caps.irq_mask = in_caps.irq_mask;
    caps.dma_direction_flags = 0;
    caps.dma_max_bytes = 0;
    caps.dma_window_base = 0;
    caps.dma_window_length = 0;

    if ((caps.cap_flags & DEVMGR_CAP_IO_PORT) == 0) {
        caps.io_port_min = 0;
        caps.io_port_max = 0;
    }
    if ((caps.cap_flags & DEVMGR_CAP_DMA) != 0) {
        if ((in_caps.dma.direction_flags & ~WASMOS_DMA_DIR_BIDIR) != 0 ||
            in_caps.dma.direction_flags == 0 ||
            in_caps.dma.max_bytes == 0 ||
            in_caps.dma.window_count == 0 ||
            in_caps.dma.window_count > 4) {
            return -1;
        }
        if (in_caps.dma.window_count != 1) {
            /* TODO: Extend kernel spawn-profile storage to preserve multi-window DMA policy. */
            return -1;
        }
        if (in_caps.dma.windows[0].length == 0) {
            return -1;
        }
        win_end = in_caps.dma.windows[0].base + in_caps.dma.windows[0].length;
        if (win_end < in_caps.dma.windows[0].base) {
            return -1;
        }
        caps.dma_direction_flags = in_caps.dma.direction_flags;
        caps.dma_max_bytes = in_caps.dma.max_bytes;
        caps.dma_window_base = in_caps.dma.windows[0].base;
        caps.dma_window_length = in_caps.dma.windows[0].length;
    }

    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return -1;
    }
    if (pm_apply_spawn_caps(pid, &caps) != 0) {
        return -1;
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
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

int
pm_handle_spawn_name(uint32_t pm_context_id, const ipc_message_t *msg)
{
    char name[32];
    uint32_t owner_context = 0;
    process_t *caller = 0;
    uint32_t parent_pid = 0;

    pm_unpack_name_args((uint32_t)msg->arg0,
                        (uint32_t)msg->arg1,
                        (uint32_t)msg->arg2,
                        (uint32_t)msg->arg3,
                        name,
                        sizeof(name));
    if (name[0] == '\0') {
        return -1;
    }
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    parent_pid = caller->pid;

    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || g_pm.fs_reply_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (g_pm.spawn.in_use) {
        return -2;
    }
    uint32_t fs_req_id = 0;
    if (pm_send_fs_read(pm_context_id, name, &fs_req_id) != 0) {
        return -1;
    }

    g_pm.spawn.in_use = 1;
    g_pm.spawn.reply_endpoint = msg->source;
    g_pm.spawn.request_id = msg->request_id;
    g_pm.spawn.parent_pid = parent_pid;
    g_pm.spawn.fs_request_id = fs_req_id;
    for (uint32_t i = 0; i < sizeof(g_pm.spawn.name); ++i) {
        g_pm.spawn.name[i] = name[i];
        if (!name[i]) {
            break;
        }
    }
    return 0;
}

int
pm_handle_module_meta(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    wasmos_app_desc_t desc;
    uint32_t match_index = msg->arg1;
    uint32_t match_count = 0;
    wasmos_app_driver_match_t *match = 0;
    uint32_t cap_flags = 0;
    uint32_t packed_match = 0;
    uint32_t packed_vendor_device = 0;
    uint32_t packed_caps = 0;
    uint32_t packed_io = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    if (wasmos_app_module_desc(g_pm.boot_info, msg->arg0, &desc) != 0) {
        return -1;
    }
    if ((desc.flags & WASMOS_APP_FLAG_DRIVER) == 0) {
        return -1;
    }
    match_count = desc.driver_match_count;
    if (match_count == 0 || match_index >= match_count) {
        return -1;
    }
    match = &desc.driver_matches[match_index];
    cap_flags = wasmos_app_driver_cap_flags(&desc);

    packed_match = ((uint32_t)match->class_code << 24) |
                   ((uint32_t)match->subclass << 16) |
                   ((uint32_t)match->prog_if << 8) |
                   ((((desc.flags & WASMOS_APP_FLAG_STORAGE_BOOTSTRAP) != 0) ? 1u : 0u) |
                    ((match_count & 0x7Fu) << 1));
    packed_vendor_device = ((uint32_t)match->vendor_id << 16) |
                           (uint32_t)match->device_id;
    packed_caps = (uint32_t)cap_flags;
    packed_io = ((uint32_t)match->io_port_max << 16) |
                ((uint32_t)match->io_port_min & 0xFFFFu);

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = packed_io;
    resp.arg1 = packed_match;
    resp.arg2 = packed_vendor_device;
    resp.arg3 = packed_caps;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

int
pm_handle_module_meta_path(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    char path[96];
    uint32_t path_ptr = (uint32_t)msg->arg0;
    uint32_t path_len = (uint32_t)msg->arg1;
    uint32_t source = (uint32_t)msg->arg2;
    wasmos_app_desc_t desc;
    uint32_t module_index = 0xFFFFFFFFu;
    uint32_t cap_flags = 0;
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    if (path_len == 0 || path_len >= sizeof(path)) {
        return -1;
    }
    if (mm_copy_from_user(owner_context, path, (uint64_t)path_ptr, path_len) != 0) {
        return -1;
    }
    path[path_len] = '\0';

    if (source == PROC_MODULE_SOURCE_INITFS) {
        if (wasmos_app_module_desc_by_initfs_path(g_pm.boot_info, path, &module_index, &desc) != 0) {
            return -1;
        }
    } else if (source == PROC_MODULE_SOURCE_FS) {
        return -1;
    } else {
        return -1;
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
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}
