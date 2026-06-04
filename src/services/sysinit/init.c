/* init.c - system initialiser service: reads sysinit.rc and drives sequential
 * service startup via the script engine callback table */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/script.h"
#include "wasmos_driver_abi.h"
#include "sysinit_types.h"

static sysinit_state_t g_state = {
    .reply_endpoint    = -1,
    .spawn_request_id  = 1,
    .proc_endpoint     = -1,
};
static wasmos_script_state_t g_script_state;
static int32_t (*volatile g_console_write)(int32_t, int32_t);
static int32_t (*volatile g_debug_mark)(int32_t);

static void
log_line(const char *s)
{
    if (!s) {
        return;
    }
    int len = (int)strlen(s);
    if (len > 0) {
        int32_t rc = g_console_write((int32_t)(uintptr_t)s, len);
        if (rc < 0) {
            char ch = '!';
            (void)g_console_write((int32_t)(uintptr_t)&ch, 1);
        }
    }
}

static void
fatal_stall(const char *msg)
{
    log_line(msg);
    wasmos_sys_ipc_recv_loop();
}

/* Fire-and-forget spawn: writes path into the FS buffer and sends
 * PROC_IPC_SPAWN_PATH.  Retries up to SYSINIT_MAX_SPAWN_ATTEMPTS on
 * PROC_IPC_ERROR with arg1==-2 (loader busy). */
static int
spawn_path(const char *path)
{
    uint32_t path_len = 0;
    if (!path || path[0] == '\0') {
        return -1;
    }
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 240u) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    for (uint32_t attempt = 0; attempt < SYSINIT_MAX_SPAWN_ATTEMPTS; ++attempt) {
        if (wasmos_ipc_send(g_state.proc_endpoint,
                            g_state.reply_endpoint,
                            PROC_IPC_SPAWN_PATH,
                            g_state.spawn_request_id,
                            0,
                            (int32_t)path_len,
                            0,
                            0) != 0) {
            return -1;
        }
        int32_t recv_rc = wasmos_ipc_recv(g_state.reply_endpoint);
        if (recv_rc < 0) {
            return -1;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != g_state.spawn_request_id) {
            return -1;
        }
        if (resp_type == PROC_IPC_RESP) {
            g_state.spawn_request_id++;
            return 0;
        }
        if (resp_type == PROC_IPC_ERROR &&
            wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1) == -2) {
            wasmos_sched_yield();
            continue;
        }
        return -1;
    }
    return -1;
}

/* Script 'start' callback: synchronous spawn with SYSINIT_START_TIMEOUT_MS
 * deadline; blocks until the spawned service sends PROC_IPC_NOTIFY_READY. */
static int
sysinit_on_start(void *user, const char *path)
{
    (void)user;
    uint32_t path_len = 0;
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 240u) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    int32_t pid = wasmos_sys_spawn_path_sync(g_state.proc_endpoint,
                                             g_state.reply_endpoint,
                                             (int32_t)path_len,
                                             SYSINIT_START_TIMEOUT_MS,
                                             g_state.spawn_request_id);
    if (pid < 0) {
        return -1;
    }
    g_state.spawn_request_id++;
    return 0;
}

static int
sysinit_on_spawn(void *user, const char *path)
{
    (void)user;
    return spawn_path(path);
}

/* Script 'exec' callback: spawns path with args in the FS buffer (path at
 * offset 0, args at offset path_len+1), then sends PROC_IPC_WAIT and blocks
 * until the child exits; sets *out_exit_code to the exit status. */
static int
sysinit_on_exec(void *user, const char *path, const char *args, int32_t *out_exit_code)
{
    (void)user;
    uint32_t path_len = 0;
    uint32_t args_len = 0;
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 240u) {
        return -1;
    }
    if (args && args[0]) {
        while (args[args_len]) {
            args_len++;
        }
    }
    uint32_t write_off = path_len + 1u;
    int32_t fs_buf_size = wasmos_fs_buffer_size();
    if (fs_buf_size <= 0 || (int32_t)path_len >= fs_buf_size) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    if (args_len > 0u) {
        if ((int32_t)write_off >= fs_buf_size ||
            (int32_t)(write_off + args_len) > fs_buf_size) {
            return -1;
        }
        if (wasmos_fs_buffer_write((int32_t)(uintptr_t)args, (int32_t)args_len, (int32_t)write_off) != 0) {
            return -1;
        }
    }
    if (wasmos_ipc_send(g_state.proc_endpoint,
                        g_state.reply_endpoint,
                        PROC_IPC_SPAWN_PATH,
                        g_state.spawn_request_id,
                        0,
                        (int32_t)path_len,
                        (int32_t)args_len,
                        0) != 0) {
        return -1;
    }
    int32_t recv_rc = wasmos_ipc_recv(g_state.reply_endpoint);
    if (recv_rc < 0) {
        return -1;
    }
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if (resp_req != g_state.spawn_request_id) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    int32_t pid = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    g_state.spawn_request_id++;
    if (pid <= 0) {
        return -1;
    }
    if (wasmos_ipc_send(g_state.proc_endpoint,
                        g_state.reply_endpoint,
                        PROC_IPC_WAIT,
                        g_state.spawn_request_id,
                        pid,
                        0,
                        0,
                        0) != 0) {
        return -1;
    }
    recv_rc = wasmos_ipc_recv(g_state.reply_endpoint);
    if (recv_rc < 0) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != g_state.spawn_request_id) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    if (out_exit_code) {
        *out_exit_code = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    }
    g_state.spawn_request_id++;
    return 0;
}

/* Script 'wait-svc' callback: spins on wasmos_svc_lookup until the named
 * service registers; yields between attempts to avoid stalling the scheduler. */
static int
sysinit_on_wait_svc(void *user, const char *name)
{
    (void)user;
    int32_t req_id = g_state.spawn_request_id;
    for (;;) {
        int32_t endpoint = wasmos_svc_lookup(g_state.proc_endpoint,
                                             g_state.reply_endpoint,
                                             name,
                                             req_id);
        req_id++;
        if (endpoint >= 0) {
            g_state.spawn_request_id = req_id;
            return 0;
        }
        wasmos_sched_yield();
    }
}

static void
sysinit_on_echo(void *user, const char *text)
{
    (void)user;
    log_line(text);
    log_line("\n");
}

static int
sysinit_on_export(void *user, const char *name, const char *value)
{
    (void)user;
    int32_t name_len = (int32_t)strlen(name);
    int32_t val_len  = (int32_t)strlen(value);
    return wasmos_env_set(name, name_len, value, val_len);
}

/* Service entry point.  Calls wasmos_sys_notify_ready immediately (sysinit
 * has no readiness dependency of its own), then runs the sysinit.rc script
 * via wasmos_script_run.  Loops on ipc_recv after the script completes. */
WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_console_write = wasmos_console_write;
    g_debug_mark = wasmos_debug_mark;

    g_state.reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_state.reply_endpoint < 0) {
        fatal_stall("[sysinit] failed to create reply endpoint\n");
    }

    if (proc_endpoint < 0) {
        fatal_stall("[sysinit] invalid init args\n");
    }
    g_state.proc_endpoint = proc_endpoint;

    wasmos_sys_notify_ready(g_state.proc_endpoint, g_state.reply_endpoint);

    wasmos_script_state_init(&g_script_state);

    wasmos_script_ops_t ops;
    ops.on_start    = sysinit_on_start;
    ops.on_spawn    = sysinit_on_spawn;
    ops.on_exec     = sysinit_on_exec;
    ops.on_wait_svc = sysinit_on_wait_svc;
    ops.on_echo     = sysinit_on_echo;
    ops.on_export   = sysinit_on_export;
    ops.user        = &g_state;

    int rc = wasmos_script_run(&g_script_state, &ops, SYSINIT_SCRIPT_PATH);
    if (rc != 0) {
        fatal_stall("[sysinit] script failed or not found\n");
    }

    for (;;) {
        (void)wasmos_ipc_recv(g_state.reply_endpoint);
    }
}
