/* init.c - system initialiser service: reads sysinit.rc and drives sequential
 * service startup via the script engine callback table */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/script.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"
#include "sysinit_types.h"

static sysinit_state_t g_state = {
    .reply_endpoint = -1,
    .spawn_request_id = 1,
    .proc_endpoint = -1,
};
static wasmos_script_state_t g_script_state;
static int32_t (*volatile g_console_write)(int32_t, int32_t);
static int32_t (*volatile g_debug_mark)(int32_t);

static void log_line(const char* s) {
    if (!s) {
        return;
    }
    int len = (int)strlen(s);
    if (len > 0) {
        int32_t rc = g_console_write(addr_cast(int32_t, s), len);
        if (rc < 0) {
            char ch = '!';
            (void)g_console_write(addr_cast(int32_t, &ch), 1);
        }
    }
}

static void fatal_stall(const char* msg) {
    log_line(msg);
    wasmos_sys_ipc_recv_loop();
}

static const char* sysinit_spawn_error_reason(int32_t rc) {
    switch (rc) {
    case WASMOS_ERR_PROC_SPAWN_BAD_ENDPOINT:
        return "bad request endpoint";
    case WASMOS_ERR_PROC_SPAWN_NO_CALLER:
        return "caller not found";
    case WASMOS_ERR_PROC_SPAWN_BAD_PATH:
        return "bad path";
    case WASMOS_ERR_PROC_SPAWN_CALLER_FSBUF:
        return "caller transfer buffer unavailable";
    case WASMOS_ERR_PROC_SPAWN_ARGS_TOOBIG:
        return "args too long";
    case WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF:
        return "pm transfer buffer unavailable";
    case WASMOS_ERR_PROC_SPAWN_FS_READ:
        return "cannot read executable";
    case WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED:
        return "process create/start failed";
    case WASMOS_ERR_PROC_SPAWN_BROKER_IPC:
        return "broker plan IPC failed";
    case WASMOS_ERR_PROC_SPAWN_BROKER_PLAN:
        return "broker returned an invalid spawn plan";
    case WASMOS_ERR_PROC_SPAWN_BROKER_DEFERRED:
        return "broker plan deferred";
    case WASMOS_ERR_PROC_PM_BUSY:
        return "process manager busy";
    default:
        return 0;
    }
}

static void sysinit_log_spawn_failure(const char* op, const char* path, int32_t rc) {
    const char* reason = sysinit_spawn_error_reason(rc);

    log_line("[sysinit] ");
    log_line(op ? op : "spawn");
    log_line(" failed");
    if (path && path[0] != '\0') {
        log_line(" for ");
        log_line(path);
    }
    if (reason) {
        log_line(": ");
        log_line(reason);
    }
    log_line("\n");
}

/* Spawn `path` without waiting for the child to finish: PROC_SPAWN_PATH_FLAG_
 * AUTOREAP makes the PM reap it, so sysinit never issues a PROC_IPC_WAIT.  The
 * PM's spawn reply is still awaited; a PROC_IPC_ERROR carrying
 * WASMOS_ERR_PROC_PM_BUSY is retried up to SYSINIT_MAX_SPAWN_ATTEMPTS times.
 * Returns 0 once the PM accepted the spawn, -1 otherwise. */
static int spawn_path(const char* path) {
    wasmos_ipc_message_t reply;
    uint32_t path_len = 0;
    int32_t bid;
    if (!path || path[0] == '\0') {
        return -1;
    }
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 240u) {
        return -1;
    }
    /* Own a per-spawn buffer with the path at offset 0; PM reads it via
     * ownership (arg1 = (buffer_id << 12) | path_len). Released after the reply. */
    bid = wasmos_xfer_buffer_acquire((int32_t)path_len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), (int32_t)path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    for (uint32_t attempt = 0; attempt < SYSINIT_MAX_SPAWN_ATTEMPTS; ++attempt) {
        if (wasmos_ipc_call(
                g_state.proc_endpoint,
                g_state.reply_endpoint,
                PROC_IPC_SPAWN_PATH,
                g_state.spawn_request_id,
                PROC_SPAWN_PATH_FLAG_AUTOREAP, /* fire-and-forget: reap the child on exit */
                (int32_t)(((uint32_t)bid << 12) | (path_len & 0xFFFu)),
                0,
                0,
                &reply) != 0) {
            (void)wasmos_xfer_buffer_release(bid);
            return -1;
        }
        if (reply.type == PROC_IPC_RESP) {
            g_state.spawn_request_id++;
            (void)wasmos_xfer_buffer_release(bid);
            return 0;
        }
        if (reply.type == PROC_IPC_ERROR && (int32_t)reply.arg1 == WASMOS_ERR_PROC_PM_BUSY) {
            wasmos_sched_yield();
            continue;
        }
        if (reply.type == PROC_IPC_ERROR) {
            sysinit_log_spawn_failure("spawn", path, (int32_t)reply.arg1);
        }
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    sysinit_log_spawn_failure("spawn", path, WASMOS_ERR_PROC_PM_BUSY);
    (void)wasmos_xfer_buffer_release(bid);
    return -1;
}

/* Script 'start' callback: synchronous spawn with SYSINIT_START_TIMEOUT_MS
 * deadline; blocks until the spawned service sends PROC_IPC_NOTIFY_READY. */
static int sysinit_on_start(void* user, const char* path) {
    wasmos_ipc_message_t reply;
    (void)user;
    uint32_t path_len = 0;
    int32_t bid;
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 240u) {
        return -1;
    }
    bid = wasmos_xfer_buffer_acquire((int32_t)path_len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), (int32_t)path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_call(g_state.proc_endpoint,
                        g_state.reply_endpoint,
                        PROC_IPC_SPAWN_PATH_SYNC,
                        g_state.spawn_request_id,
                        0,
                        (int32_t)(((uint32_t)bid << 12) | (path_len & 0xFFFu)),
                        0,
                        SYSINIT_START_TIMEOUT_MS,
                        &reply) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        sysinit_log_spawn_failure("start", path, -1);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    if (reply.type != PROC_IPC_RESP || (int32_t)reply.arg0 < 0) {
        if (reply.type == PROC_IPC_ERROR) {
            sysinit_log_spawn_failure("start", path, (int32_t)reply.arg1);
        } else {
            sysinit_log_spawn_failure("start", path, -1);
        }
        return -1;
    }
    g_state.spawn_request_id++;
    return 0;
}

static int sysinit_on_spawn(void* user, const char* path) {
    (void)user;
    return spawn_path(path);
}

/* Script 'exec' callback: spawns path with args in the xfer buffer (path at
 * offset 0, args at offset path_len+1), then sends PROC_IPC_WAIT and blocks
 * until the child exits; sets *out_exit_code to the exit status. */
static int sysinit_on_exec(void* user, const char* path, const char* args, int32_t* out_exit_code) {
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
    int32_t fs_buf_size = wasmos_xfer_buffer_size();
    int32_t bid;
    if (fs_buf_size <= 0 || (int32_t)path_len >= fs_buf_size) {
        return -1;
    }
    /* Own a buffer holding path@0 and args@path_len+1; PM reads via ownership
     * (arg1 = (buffer_id << 12) | path_len, arg2 = args_len). */
    bid = wasmos_xfer_buffer_acquire((int32_t)(write_off + args_len + 1u));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), (int32_t)path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (args_len > 0u) {
        if ((int32_t)write_off >= fs_buf_size || (int32_t)(write_off + args_len) > fs_buf_size) {
            (void)wasmos_xfer_buffer_release(bid);
            return -1;
        }
        if (wasmos_xfer_buffer_write(
                bid, addr_cast(int32_t, args), (int32_t)args_len, (int32_t)write_off) != 0) {
            (void)wasmos_xfer_buffer_release(bid);
            return -1;
        }
    }
    if (wasmos_ipc_send(g_state.proc_endpoint,
                        g_state.reply_endpoint,
                        PROC_IPC_SPAWN_PATH,
                        g_state.spawn_request_id,
                        0,
                        (int32_t)(((uint32_t)bid << 12) | (path_len & 0xFFFu)),
                        (int32_t)args_len,
                        0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    int32_t recv_rc = wasmos_ipc_select_one(g_state.reply_endpoint);
    (void)wasmos_xfer_buffer_release(bid);
    if (recv_rc < 0) {
        return -1;
    }
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if (resp_req != g_state.spawn_request_id) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) == PROC_IPC_ERROR) {
            sysinit_log_spawn_failure("exec", path, wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1));
        }
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
    recv_rc = wasmos_ipc_select_one(g_state.reply_endpoint);
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

/* Script 'wait-svc' callback: polls wasmos_svc_lookup until the named service
 * registers, yielding between attempts.  Never gives up, so a `wait-svc` for a
 * service that never appears stalls the rest of sysinit.rc forever.
 * TODO: replace the poll with a class subscription (SVC_IPC_SUBSCRIBE_CLASS_REQ)
 * plus a bounded wait; the poll loads the process manager with lookup traffic
 * for as long as the service is missing. */
static int sysinit_on_wait_svc(void* user, const char* name) {
    (void)user;
    int32_t req_id = g_state.spawn_request_id;
    for (;;) {
        int32_t endpoint =
            wasmos_svc_lookup(g_state.proc_endpoint, g_state.reply_endpoint, name, req_id);
        req_id++;
        if (endpoint >= 0) {
            g_state.spawn_request_id = req_id;
            return 0;
        }
        wasmos_sched_yield();
    }
}

static void sysinit_on_echo(void* user, const char* text) {
    (void)user;
    log_line(text);
    log_line("\n");
}

static int sysinit_on_export(void* user, const char* name, const char* value) {
    (void)user;
    int32_t name_len = (int32_t)strlen(name);
    int32_t val_len = (int32_t)strlen(value);
    return wasmos_env_set(name, name_len, value, val_len);
}

/* Service entry point.  Calls wasmos_sys_notify_ready immediately (sysinit
 * has no readiness dependency of its own), then runs the sysinit.rc script
 * via wasmos_script_run.  Loops on ipc_recv after the script completes. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* The proc endpoint comes from the spawn-info contract; the entry args carry
     * nothing and the parameter is overwritten. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();

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

    wasmos_script_ops_t ops = {0};
    ops.on_start = sysinit_on_start;
    ops.on_spawn = sysinit_on_spawn;
    ops.on_exec = sysinit_on_exec;
    ops.on_wait_svc = sysinit_on_wait_svc;
    ops.on_echo = sysinit_on_echo;
    ops.on_export = sysinit_on_export;
    ops.user = &g_state;

    int rc = wasmos_script_run(&g_script_state, &ops, SYSINIT_SCRIPT_PATH);
    if (rc != 0) {
        fatal_stall("[sysinit] script failed or not found\n");
    }

    for (;;) {
        (void)wasmos_ipc_select_one(g_state.reply_endpoint);
    }
}
