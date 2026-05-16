#include "process_manager.h"
#include "process_manager_internal.h"
#include "ipc.h"
#include "serial.h"
#include "string.h"

pm_state_t g_pm;
uint8_t g_pm_wait_owner_deny_logged;
uint8_t g_pm_kill_owner_deny_logged;
uint8_t g_pm_status_owner_deny_logged;
uint8_t g_pm_spawn_owner_deny_logged;

uint32_t
pm_alloc_cli_tty(void)
{
    uint32_t tty = g_pm.next_cli_tty;
    if (tty < 1 || tty > 3) {
        tty = 1;
    }
    g_pm.next_cli_tty = (tty >= 3) ? 1 : (tty + 1);
    return tty;
}

void
process_manager_inject_wait_owner_mismatch_test(uint32_t expected_owner_context_id)
{
    uint32_t reply_endpoint = IPC_ENDPOINT_NONE;
    if (expected_owner_context_id == 0) {
        return;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &reply_endpoint) != IPC_OK ||
        reply_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }
    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        pm_wait_state_t *waiter = &g_pm.waits[i];
        if (waiter->in_use) {
            continue;
        }
        waiter->in_use = 1;
        waiter->pid = 0;
        waiter->reply_endpoint = reply_endpoint;
        waiter->request_id = 0xFFFF0001u;
        waiter->owner_context_id = expected_owner_context_id;
        return;
    }
}

void
process_manager_inject_kill_owner_deny_test(void)
{
    uint32_t source_endpoint = IPC_ENDPOINT_NONE;
    ipc_message_t msg;

    if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &source_endpoint) != IPC_OK ||
        source_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }

    msg.type = PROC_IPC_KILL;
    msg.source = source_endpoint;
    msg.destination = g_pm.proc_endpoint;
    msg.request_id = 0xFFFF1001u;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    (void)ipc_send_from(IPC_CONTEXT_KERNEL, g_pm.proc_endpoint, &msg);
}

void
process_manager_inject_status_owner_deny_test(void)
{
    uint32_t source_endpoint = IPC_ENDPOINT_NONE;
    ipc_message_t msg;

    if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &source_endpoint) != IPC_OK ||
        source_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }

    msg.type = PROC_IPC_STATUS;
    msg.source = source_endpoint;
    msg.destination = g_pm.proc_endpoint;
    msg.request_id = 0xFFFF1002u;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    (void)ipc_send_from(IPC_CONTEXT_KERNEL, g_pm.proc_endpoint, &msg);
}

void
process_manager_inject_spawn_owner_deny_test(void)
{
    uint32_t source_endpoint = IPC_ENDPOINT_NONE;
    ipc_message_t msg;

    if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &source_endpoint) != IPC_OK ||
        source_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }

    msg.type = PROC_IPC_SPAWN;
    msg.source = source_endpoint;
    msg.destination = g_pm.proc_endpoint;
    msg.request_id = 0xFFFF1003u;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    (void)ipc_send_from(IPC_CONTEXT_KERNEL, g_pm.proc_endpoint, &msg);
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
        if (!g_pm_kill_owner_deny_logged) {
            g_pm_kill_owner_deny_logged = 1;
            serial_write("[test] pm kill owner deny ok\n");
        }
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
    uint32_t owner_context = 0;
    process_t *caller = 0;
    process_t *target = process_get(msg->arg0);
    ipc_message_t resp;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        if (!g_pm_status_owner_deny_logged) {
            g_pm_status_owner_deny_logged = 1;
            serial_write("[test] pm status owner deny ok\n");
        }
        return -1;
    }

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
        waiter->owner_context_id = owner_context;
        return 0;
    }

    return -1;
}

int
process_manager_init(const boot_info_t *boot_info)
{
    g_pm.init_module_index = 0xFFFFFFFFu;
    g_pm.module_count = 0;
    g_pm.boot_info = boot_info;
    g_pm.proc_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fs_endpoint = IPC_ENDPOINT_NONE;
    g_pm.block_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fb_endpoint = IPC_ENDPOINT_NONE;
    g_pm.vt_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fs_reply_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fs_request_id = 1;
    g_pm.next_cli_tty = 1;
    g_pm.started = 0;
    g_pm_wait_owner_deny_logged = 0;
    g_pm_kill_owner_deny_logged = 0;
    g_pm_status_owner_deny_logged = 0;
    g_pm_spawn_owner_deny_logged = 0;
    if (boot_info && (boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        g_pm.module_count = boot_info->module_count;
        g_pm.init_module_index = pm_find_module_index_by_name("sysinit");
    }
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
    g_pm.spawn.in_use = 0;
    for (uint32_t i = 0; i < PM_SERVICE_REGISTRY_CAP; ++i) {
        g_pm.services[i].in_use = 0;
        g_pm.services[i].endpoint = IPC_ENDPOINT_NONE;
        g_pm.services[i].owner_context_id = 0;
        g_pm.services[i].name[0] = '\0';
    }
    return 0;
}

uint32_t process_manager_endpoint(void) { return g_pm.proc_endpoint; }
uint32_t process_manager_fs_endpoint(void) { return g_pm.fs_endpoint; }
uint32_t process_manager_block_endpoint(void) { return g_pm.block_endpoint; }
uint32_t process_manager_vt_endpoint(void) { return g_pm.vt_endpoint; }
uint32_t process_manager_framebuffer_endpoint(void) { return g_pm.fb_endpoint; }

void
process_manager_set_framebuffer_endpoint(uint32_t endpoint)
{
    g_pm.fb_endpoint = endpoint;
    (void)pm_service_set("fb", endpoint, IPC_CONTEXT_KERNEL);
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
        if (ipc_endpoint_create(process->context_id, &g_pm.fs_reply_endpoint) != IPC_OK) {
            serial_write("[pm] fs reply endpoint create failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        g_pm.started = 1;
    }

    pm_check_waits(process->context_id);
    pm_reap_apps(process);
    pm_poll_spawn(process->context_id);

    int recv_rc = ipc_recv_for(process->context_id, g_pm.proc_endpoint, &msg);
    if (recv_rc == IPC_EMPTY) {
        return PROCESS_RUN_YIELDED;
    }
    if (recv_rc != IPC_OK) {
        return PROCESS_RUN_YIELDED;
    }

    int rc = -1;
    switch (msg.type) {
        case PROC_IPC_SPAWN:
            rc = pm_handle_spawn(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_CAPS:
            rc = pm_handle_spawn_caps(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_NAME:
            rc = pm_handle_spawn_name(process->context_id, &msg);
            break;
        case PROC_IPC_MODULE_META:
            rc = pm_handle_module_meta(process->context_id, &msg);
            break;
        case PROC_IPC_MODULE_META_PATH:
            rc = pm_handle_module_meta_path(process->context_id, &msg);
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
        case SVC_IPC_REGISTER_REQ:
            rc = pm_handle_service_register(process->context_id, &msg);
            break;
        case SVC_IPC_LOOKUP_REQ:
            rc = pm_handle_service_lookup(process->context_id, &msg);
            break;
        default:
            rc = -1;
            break;
    }

    if (rc != 0) {
        ipc_message_t resp;
        if (msg.type == SVC_IPC_REGISTER_REQ || msg.type == SVC_IPC_LOOKUP_REQ) {
            resp.type = SVC_IPC_ERROR;
        } else {
            resp.type = PROC_IPC_ERROR;
        }
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
