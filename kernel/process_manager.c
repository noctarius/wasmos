#include "process_manager.h"
#include "ipc.h"
#include "serial.h"
#include "wasmos_app.h"
#include "wasm_chardev.h"

#define PM_MAX_MANAGED_APPS 8u
#define PM_MAX_WAITERS 8u

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    const uint8_t *blob;
    uint32_t blob_size;
    uint8_t started;
    uint32_t step_arg0;
    uint32_t step_arg1;
    uint32_t step_arg2;
    uint32_t step_arg3;
    wasmos_app_instance_t app;
    char name[64];
} pm_app_state_t;

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t reply_endpoint;
    uint32_t request_id;
} pm_wait_state_t;

typedef struct {
    const boot_info_t *boot_info;
    uint32_t proc_endpoint;
    uint8_t started;
    uint32_t init_module_index;
    uint32_t chardev_module_index;
    uint32_t module_count;
    pm_app_state_t apps[PM_MAX_MANAGED_APPS];
    pm_wait_state_t waits[PM_MAX_WAITERS];
} pm_state_t;

static pm_state_t g_pm;

static void
pm_write_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static int
copy_name(char *dst, uint32_t dst_len, const uint8_t *src, uint32_t src_len)
{
    if (!dst || !src || dst_len == 0 || src_len == 0 || src_len >= dst_len) {
        return -1;
    }
    for (uint32_t i = 0; i < src_len; ++i) {
        dst[i] = (char)src[i];
    }
    dst[src_len] = '\0';
    return 0;
}

static int
name_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

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

static uint32_t
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
        if (copy_name(temp, sizeof(temp), desc.name, desc.name_len) != 0) {
            continue;
        }
        if (name_eq(temp, name)) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static process_run_result_t
pm_app_entry(process_t *process, void *arg)
{
    pm_app_state_t *state = (pm_app_state_t *)arg;
    ipc_message_t step_msg;
    int32_t step_result = WASMOS_WASM_STEP_FAILED;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

    if (!state->started) {
        wasmos_app_desc_t desc;
        if (wasmos_app_parse(state->blob, state->blob_size, &desc) != 0) {
            serial_write("[pm] app parse failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (wasmos_app_start(&state->app, &desc, process->context_id) != 0) {
            serial_write("[pm] app start failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->started = 1;
    }

    step_msg.type = 0;
    step_msg.source = 0;
    step_msg.destination = 0;
    step_msg.request_id = 0;
    step_msg.arg0 = state->step_arg0;
    step_msg.arg1 = state->step_arg1;
    step_msg.arg2 = state->step_arg2;
    step_msg.arg3 = state->step_arg3;

    if (wasmos_app_dispatch(&state->app, &step_msg, &step_result) != 0) {
        serial_write("[pm] app dispatch failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    if (step_result == WASMOS_WASM_STEP_BLOCKED) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    if (step_result == WASMOS_WASM_STEP_DONE) {
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    if (step_result == WASMOS_WASM_STEP_FAILED) {
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    return PROCESS_RUN_YIELDED;
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

    if (copy_name(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        return -1;
    }

    slot->blob = (const uint8_t *)(uintptr_t)mod->base;
    slot->blob_size = (uint32_t)mod->size;
    slot->started = 0;
    slot->step_arg0 = 0;
    slot->step_arg1 = 0;
    slot->step_arg2 = 0;
    slot->step_arg3 = 0;
    slot->in_use = 1;

    if (name_eq(slot->name, "init")) {
        if (g_pm.chardev_module_index == 0xFFFFFFFFu) {
            slot->in_use = 0;
            return -1;
        }
        slot->step_arg0 = g_pm.proc_endpoint;
        slot->step_arg1 = g_pm.module_count;
        slot->step_arg2 = g_pm.init_module_index;
        slot->step_arg3 = 0;
    } else if (name_eq(slot->name, "chardev-client")) {
        uint32_t chardev_endpoint = IPC_ENDPOINT_NONE;
        if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
            slot->in_use = 0;
            return -1;
        }
        slot->step_arg0 = chardev_endpoint;
    }

    if (process_spawn_as(parent_pid, slot->name, pm_app_entry, slot, out_pid) != 0) {
        slot->in_use = 0;
        return -1;
    }

    slot->pid = *out_pid;
    return 0;
}

static void
pm_check_waits(uint32_t pm_context_id)
{
    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        pm_wait_state_t *waiter = &g_pm.waits[i];
        if (!waiter->in_use) {
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

static int
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

static int
pm_handle_kill(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    process_t *target = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }

    target = process_get(msg->arg0);
    if (!target || target->parent_pid != caller->pid) {
        return -1;
    }

    if (process_kill(msg->arg0, (int32_t)msg->arg1) != 0) {
        return -1;
    }

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = msg->arg0;
    resp.arg1 = msg->arg1;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

static int
pm_handle_status(uint32_t pm_context_id, const ipc_message_t *msg)
{
    process_t *target = process_get(msg->arg0);
    ipc_message_t resp;

    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = msg->arg0;
    resp.arg1 = PROC_STATUS_UNKNOWN;
    resp.arg2 = 0;
    resp.arg3 = 0;

    if (target) {
        if (target->state == PROCESS_STATE_ZOMBIE) {
            resp.arg1 = PROC_STATUS_ZOMBIE;
            resp.arg2 = (uint32_t)target->exit_status;
        } else if (target->state != PROCESS_STATE_UNUSED) {
            resp.arg1 = PROC_STATUS_RUNNING;
        }
    }

    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

static int
pm_handle_wait(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    process_t *target = 0;
    int32_t exit_status = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }

    target = process_get(msg->arg0);
    if (!target || target->parent_pid != caller->pid) {
        return -1;
    }

    if (process_get_exit_status(msg->arg0, &exit_status) == 0) {
        ipc_message_t resp;
        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = msg->source;
        resp.request_id = msg->request_id;
        resp.arg0 = msg->arg0;
        resp.arg1 = (uint32_t)exit_status;
        resp.arg2 = 0;
        resp.arg3 = 0;
        return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
    }

    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        pm_wait_state_t *waiter = &g_pm.waits[i];
        if (waiter->in_use) {
            continue;
        }
        waiter->in_use = 1;
        waiter->pid = msg->arg0;
        waiter->reply_endpoint = msg->source;
        waiter->request_id = msg->request_id;
        return 0;
    }

    return -1;
}

static void
pm_boot_spawn(void)
{
    const boot_info_t *info = g_pm.boot_info;
    if (!info || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        serial_write("[pm] no boot modules\n");
        return;
    }

    g_pm.init_module_index = pm_find_module_index_by_name("init");
    g_pm.chardev_module_index = pm_find_module_index_by_name("chardev-client");
    g_pm.module_count = info->module_count;

    if (g_pm.init_module_index == 0xFFFFFFFFu) {
        serial_write("[pm] init module not found\n");
        return;
    }

    uint32_t pid = 0;
    if (pm_spawn_module(process_current_pid(), g_pm.init_module_index, &pid) == 0) {
        serial_write("[pm] spawned init pid=");
        pm_write_hex64(pid);
    } else {
        serial_write("[pm] init spawn failed\n");
    }
}

int
process_manager_init(const boot_info_t *boot_info)
{
    g_pm.boot_info = boot_info;
    g_pm.proc_endpoint = IPC_ENDPOINT_NONE;
    g_pm.started = 0;
    g_pm.init_module_index = 0xFFFFFFFFu;
    g_pm.chardev_module_index = 0xFFFFFFFFu;
    g_pm.module_count = 0;
    for (uint32_t i = 0; i < PM_MAX_MANAGED_APPS; ++i) {
        g_pm.apps[i].in_use = 0;
        g_pm.apps[i].pid = 0;
        g_pm.apps[i].blob = 0;
        g_pm.apps[i].blob_size = 0;
        g_pm.apps[i].started = 0;
        g_pm.apps[i].name[0] = '\0';
    }
    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        g_pm.waits[i].in_use = 0;
        g_pm.waits[i].pid = 0;
        g_pm.waits[i].reply_endpoint = IPC_ENDPOINT_NONE;
        g_pm.waits[i].request_id = 0;
    }
    return 0;
}

uint32_t
process_manager_endpoint(void)
{
    return g_pm.proc_endpoint;
}

process_run_result_t
process_manager_entry(process_t *process, void *arg)
{
    ipc_message_t msg;

    (void)arg;

    if (!process) {
        return PROCESS_RUN_IDLE;
    }

    if (!g_pm.started) {
        if (ipc_endpoint_create(process->context_id, &g_pm.proc_endpoint) != IPC_OK) {
            serial_write("[pm] endpoint create failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        g_pm.started = 1;
        serial_write("[pm] proc endpoint=");
        pm_write_hex64(g_pm.proc_endpoint);
        pm_boot_spawn();
    }

    pm_check_waits(process->context_id);

    int recv_rc = ipc_recv_for(process->context_id, g_pm.proc_endpoint, &msg);
    if (recv_rc == IPC_EMPTY) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    if (recv_rc != IPC_OK) {
        return PROCESS_RUN_YIELDED;
    }

    int rc = -1;
    switch (msg.type) {
        case PROC_IPC_SPAWN:
            rc = pm_handle_spawn(process->context_id, &msg);
            break;
        case PROC_IPC_KILL:
            rc = pm_handle_kill(process->context_id, &msg);
            break;
        case PROC_IPC_STATUS:
            rc = pm_handle_status(process->context_id, &msg);
            break;
        case PROC_IPC_WAIT:
            rc = pm_handle_wait(process->context_id, &msg);
            break;
        default:
            rc = -1;
            break;
    }

    if (rc != 0) {
        ipc_message_t resp;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = msg.source;
        resp.request_id = msg.request_id;
        resp.arg0 = msg.type;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(process->context_id, msg.source, &resp);
    }

    return PROCESS_RUN_YIELDED;
}
