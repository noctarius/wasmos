/* init.c - system initialiser service: interprets sysinit.rc one line at a
 * time and drives service startup from a single event loop.
 *
 * The script runner (wasmos/script.h) suspends on every command that waits --
 * start, spawn, exec, wait-svc, service -- and this file finishes the command
 * when its reply or event arrives on the loop. Nothing here waits inside a
 * hook, so the loop is the only receive on the reply endpoint and sysinit never
 * parks in a nested receive. The runner serializes the script, so at most one command is
 * in flight, held in g_op. */
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
static wasmos_script_runner_t g_runner;
static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static int32_t (*volatile g_console_write)(int32_t, int32_t);

/* The command in flight. `kind` selects which reply handler owns it; the
 * transfer buffer carries the path (and an exec's arguments) until the spawn
 * reply. */
typedef enum {
    SYSINIT_OP_NONE = 0,
    SYSINIT_OP_SPAWN,
    SYSINIT_OP_START,
    SYSINIT_OP_EXEC,
    SYSINIT_OP_WAIT_SVC,
    SYSINIT_OP_SERVICE,
} sysinit_op_kind_t;

typedef struct {
    sysinit_op_kind_t kind;
    int32_t buffer_id; /* -1 when none */
    uint32_t path_len;
    uint32_t args_len;
    uint32_t attempts;                 /* spawn retries after PROC_PM_BUSY */
    uint8_t retry_due;                 /* resend the spawn on the next loop tick */
    char path[WASMOS_SCRIPT_LINE_MAX]; /* the path, the awaited name, or the unit spec */
    wasmos_sys_svc_watch_t watch;
} sysinit_op_t;

static sysinit_op_t g_op = {.buffer_id = -1};
/* The supervisor's endpoint, once a `service` line has found it; -1 before. */
static int32_t g_supervisor_ep = -1;

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
    case WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT:
        return "no provider registered it in time";
    case WASMOS_ERR_PROC_PM_SVC_WATCH_SEND:
        return "could not send to the process manager";
    case WASMOS_ERR_PROC_PM_SVC_WATCH_NO_BUFFER:
        return "no transfer buffer for the subscription";
    case WASMOS_ERR_SUPERVISOR_BAD_DESC:
        return "the supervisor could not read the submission";
    case WASMOS_ERR_SUPERVISOR_BAD_SPEC:
        return "bad unit spec";
    case WASMOS_ERR_SUPERVISOR_EXISTS:
        return "a unit of that name exists";
    case WASMOS_ERR_SUPERVISOR_NO_SPACE:
        return "the supervisor's unit table is full";
    default:
        return 0;
    }
}

/* Logs "[sysinit] <op> failed for <path>: <reason>"; the reason is omitted for
 * a code this file cannot name, rather than guessed. */
static void sysinit_log_failure(const char* op, const char* path, int32_t rc) {
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

static void sysinit_op_release_buffer(void) {
    if (g_op.buffer_id >= 0) {
        (void)wasmos_xfer_buffer_release(g_op.buffer_id);
        g_op.buffer_id = -1;
    }
}

/* Ends the command in flight and hands its result to the runner. */
static void sysinit_op_finish(int32_t rc, int32_t exit_code) {
    sysinit_op_release_buffer();
    g_op.kind = SYSINIT_OP_NONE;
    g_op.retry_due = 0;
    wasmos_script_runner_complete(&g_runner, rc, exit_code);
}

/* Copies `path` into g_op and stages path@0 and `args`@path_len+1 in a transfer
 * buffer the process manager reads by ownership. Returns 0, or -1 for a path
 * that is empty or too long or a buffer that cannot be staged. */
static int sysinit_op_stage(sysinit_op_kind_t kind, const char* path, const char* args) {
    uint32_t path_len = (uint32_t)strlen(path);
    uint32_t args_len = (args && args[0]) ? (uint32_t)strlen(args) : 0u;
    uint32_t write_off = path_len + 1u;
    int32_t buf_size = wasmos_xfer_buffer_size();

    if (path_len == 0 || path_len > 240u || path_len >= sizeof(g_op.path)) {
        return -1;
    }
    if (buf_size <= 0 || (int32_t)(write_off + args_len) > buf_size) {
        return -1;
    }
    memset(&g_op, 0, sizeof(g_op));
    g_op.buffer_id = wasmos_xfer_buffer_acquire((int32_t)(write_off + args_len + 1u));
    if (g_op.buffer_id < 0) {
        g_op.buffer_id = -1;
        return -1;
    }
    if (wasmos_xfer_buffer_write(g_op.buffer_id, path, (int32_t)path_len, 0) != 0 ||
        (args_len > 0u && wasmos_xfer_buffer_write(
                              g_op.buffer_id, args, (int32_t)args_len, (int32_t)write_off) != 0)) {
        sysinit_op_release_buffer();
        return -1;
    }
    memcpy(g_op.path, path, path_len + 1u);
    g_op.kind = kind;
    g_op.path_len = path_len;
    g_op.args_len = args_len;
    return 0;
}

/* The spawn-path argument word: the buffer id and the path length.
 * TODO: this packs two values into one argument, which abi/opcodes.yaml's
 * PROC_IPC_SPAWN_PATH defines; a request descriptor would carry both, and the
 * 4 KiB path-length ceiling with them. */
static int32_t sysinit_op_path_word(void) {
    return (int32_t)(((uint32_t)g_op.buffer_id << 12) | (g_op.path_len & 0xFFFu));
}

/* ------------------------------------------------------------------ exec */

static void sysinit_on_exec_wait_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != PROC_IPC_RESP) {
        sysinit_log_failure("exec", g_op.path, msg->arg1);
        sysinit_op_finish(-1, 0);
        return;
    }
    sysinit_op_finish(0, msg->arg1);
}

static void sysinit_on_exec_spawn_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    sysinit_op_release_buffer();
    if (msg->type != PROC_IPC_RESP || msg->arg0 <= 0) {
        sysinit_log_failure("exec", g_op.path, msg->type == PROC_IPC_ERROR ? msg->arg1 : 0);
        sysinit_op_finish(-1, 0);
        return;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_state.proc_endpoint,
                               g_state.reply_endpoint,
                               PROC_IPC_WAIT,
                               msg->arg0,
                               0,
                               0,
                               0,
                               sysinit_on_exec_wait_reply,
                               0,
                               0) != 0) {
        sysinit_op_finish(-1, 0);
    }
}

/* Script 'exec': spawn path with args, then wait for its exit; the exit status
 * becomes ${?}. */
static int sysinit_on_exec(void* user, const char* path, const char* args, int32_t* out_exit_code) {
    (void)user;
    (void)out_exit_code;
    if (sysinit_op_stage(SYSINIT_OP_EXEC, path, args) != 0) {
        return -1;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_state.proc_endpoint,
                               g_state.reply_endpoint,
                               PROC_IPC_SPAWN_PATH,
                               0,
                               sysinit_op_path_word(),
                               (int32_t)g_op.args_len,
                               0,
                               sysinit_on_exec_spawn_reply,
                               0,
                               0) != 0) {
        sysinit_op_release_buffer();
        g_op.kind = SYSINIT_OP_NONE;
        return -1;
    }
    return WASMOS_SCRIPT_HOOK_PENDING;
}

/* ----------------------------------------------------------------- spawn */

static int sysinit_spawn_send(void);

static void sysinit_on_spawn_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type == PROC_IPC_RESP) {
        sysinit_op_finish(0, 0);
        return;
    }
    if (msg->type == PROC_IPC_ERROR && msg->arg1 == WASMOS_ERR_PROC_PM_BUSY &&
        ++g_op.attempts < SYSINIT_MAX_SPAWN_ATTEMPTS) {
        /* Resent from the loop's next tick, not from here, so a busy process
         * manager is not answered with an immediate resend. */
        g_op.retry_due = 1;
        return;
    }
    sysinit_log_failure("spawn", g_op.path, msg->type == PROC_IPC_ERROR ? msg->arg1 : 0);
    sysinit_op_finish(-1, 0);
}

/* PROC_SPAWN_PATH_FLAG_AUTOREAP: the process manager reaps the child, so
 * sysinit never issues a PROC_IPC_WAIT for a fire-and-forget spawn. */
static int sysinit_spawn_send(void) {
    return wasmos_sys_intent_send(&g_loop,
                                  g_state.proc_endpoint,
                                  g_state.reply_endpoint,
                                  PROC_IPC_SPAWN_PATH,
                                  PROC_SPAWN_PATH_FLAG_AUTOREAP,
                                  sysinit_op_path_word(),
                                  0,
                                  0,
                                  sysinit_on_spawn_reply,
                                  0,
                                  0);
}

/* Script 'spawn': fire-and-forget; the script continues once the process
 * manager has accepted the spawn. */
static int sysinit_on_spawn(void* user, const char* path) {
    (void)user;
    if (sysinit_op_stage(SYSINIT_OP_SPAWN, path, 0) != 0) {
        return -1;
    }
    if (sysinit_spawn_send() != 0) {
        sysinit_op_release_buffer();
        g_op.kind = SYSINIT_OP_NONE;
        return -1;
    }
    return WASMOS_SCRIPT_HOOK_PENDING;
}

/* ----------------------------------------------------------------- start */

static void sysinit_on_start_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != PROC_IPC_RESP || msg->arg0 < 0) {
        sysinit_log_failure("start", g_op.path, msg->type == PROC_IPC_ERROR ? msg->arg1 : 0);
        sysinit_op_finish(-1, 0);
        return;
    }
    sysinit_op_finish(0, 0);
}

/* Script 'start': the process manager replies once the child sent
 * PROC_IPC_NOTIFY_READY, or with an error after SYSINIT_START_TIMEOUT_MS. A
 * failed start fails the script. */
static int sysinit_on_start(void* user, const char* path) {
    (void)user;
    if (sysinit_op_stage(SYSINIT_OP_START, path, 0) != 0) {
        sysinit_log_failure("start", path, 0);
        return -1;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_state.proc_endpoint,
                               g_state.reply_endpoint,
                               PROC_IPC_SPAWN_PATH_SYNC,
                               0,
                               sysinit_op_path_word(),
                               0,
                               SYSINIT_START_TIMEOUT_MS,
                               sysinit_on_start_reply,
                               0,
                               0) != 0) {
        sysinit_op_release_buffer();
        g_op.kind = SYSINIT_OP_NONE;
        sysinit_log_failure("start", path, 0);
        return -1;
    }
    return WASMOS_SCRIPT_HOOK_PENDING;
}

/* -------------------------------------------------------------- wait-svc */

static void sysinit_on_wait_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)endpoint;
    wasmos_sys_svc_watch_stop(watch);
    sysinit_op_finish(0, 0);
}

static void sysinit_on_wait_failed(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    (void)watch;
    sysinit_log_failure("wait-svc", g_op.path, status);
    sysinit_op_finish(-1, 0);
}

/* Script 'wait-svc': continue once the name is registered, or after
 * SYSINIT_WAIT_SVC_TIMEOUT_MS with a logged failure. The script continues either
 * way; a missing service is reported, not fatal. */
static int sysinit_on_wait_svc(void* user, const char* name) {
    (void)user;
    uint32_t len = (uint32_t)strlen(name);
    int32_t rc;
    if (len == 0 || len >= sizeof(g_op.path)) {
        return -1;
    }
    memset(&g_op, 0, sizeof(g_op));
    g_op.buffer_id = -1;
    g_op.kind = SYSINIT_OP_WAIT_SVC;
    memcpy(g_op.path, name, len + 1u);
    rc = wasmos_sys_svc_watch_start(&g_watches,
                                    &g_op.watch,
                                    name,
                                    SYSINIT_WAIT_SVC_TIMEOUT_MS,
                                    sysinit_on_wait_bound,
                                    sysinit_on_wait_failed,
                                    0);
    if (rc != 0) {
        g_op.kind = SYSINIT_OP_NONE;
        sysinit_log_failure("wait-svc", name, rc);
        return -1;
    }
    return WASMOS_SCRIPT_HOOK_PENDING;
}

/* --------------------------------------------------------------- service */

static void sysinit_on_service_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != SUPERVISOR_IPC_RESP) {
        sysinit_log_failure(
            "service", g_op.path, msg->type == SUPERVISOR_IPC_ERROR ? msg->arg0 : 0);
        sysinit_op_finish(-1, 0);
        return;
    }
    sysinit_op_finish(0, 0);
}

/* Sends the unit spec in g_op.path to the supervisor in a descriptor sysinit
 * owns and lends READ; the reply releases it, which revokes the loan. */
static int sysinit_service_submit(void) {
    static wasmos_supervisor_submit_desc_t desc;
    uint32_t len = (uint32_t)strlen(g_op.path);

    memset(&desc, 0, sizeof(desc));
    desc.version = SUPERVISOR_SUBMIT_DESC_VERSION;
    desc.spec_len = len;
    memcpy(desc.spec, g_op.path, len + 1u);
    g_op.buffer_id = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (g_op.buffer_id < 0) {
        g_op.buffer_id = -1;
        return -1;
    }
    if (wasmos_xfer_buffer_borrow(g_supervisor_ep, g_op.buffer_id, WASMOS_BUFFER_GRANT_READ) < 0 ||
        wasmos_sys_intent_send(&g_loop,
                               g_supervisor_ep,
                               g_state.reply_endpoint,
                               SUPERVISOR_IPC_SUBMIT_REQ,
                               g_op.buffer_id,
                               0,
                               (int32_t)sizeof(desc),
                               0,
                               sysinit_on_service_reply,
                               0,
                               0) != 0) {
        sysinit_op_release_buffer();
        return -1;
    }
    return 0;
}

static void sysinit_on_supervisor_bound(void* user, wasmos_sys_svc_watch_t* watch,
                                        int32_t endpoint) {
    (void)user;
    wasmos_sys_svc_watch_stop(watch);
    g_supervisor_ep = endpoint;
    if (sysinit_service_submit() != 0) {
        sysinit_log_failure("service", g_op.path, 0);
        sysinit_op_finish(-1, 0);
    }
}

static void sysinit_on_supervisor_missing(void* user, wasmos_sys_svc_watch_t* watch,
                                          int32_t status) {
    (void)user;
    (void)watch;
    sysinit_log_failure("service", g_op.path, status);
    sysinit_op_finish(-1, 0);
}

/* Script 'service': hand the unit to the supervisor, finding it first if no
 * earlier line has. Continues once the supervisor has accepted the unit, not
 * once it runs; a refused unit is logged and the script goes on. */
static int sysinit_on_service(void* user, const char* spec) {
    (void)user;
    uint32_t len = (uint32_t)strlen(spec);
    if (len == 0 || len > SUPERVISOR_SPEC_MAX || len >= sizeof(g_op.path)) {
        sysinit_log_failure("service", spec, WASMOS_ERR_SUPERVISOR_BAD_SPEC);
        return -1;
    }
    memset(&g_op, 0, sizeof(g_op));
    g_op.buffer_id = -1;
    g_op.kind = SYSINIT_OP_SERVICE;
    memcpy(g_op.path, spec, len + 1u);
    if (g_supervisor_ep >= 0) {
        if (sysinit_service_submit() != 0) {
            g_op.kind = SYSINIT_OP_NONE;
            sysinit_log_failure("service", spec, 0);
            return -1;
        }
        return WASMOS_SCRIPT_HOOK_PENDING;
    }
    if (wasmos_sys_svc_watch_start(&g_watches,
                                   &g_op.watch,
                                   "supervisor",
                                   SYSINIT_WAIT_SVC_TIMEOUT_MS,
                                   sysinit_on_supervisor_bound,
                                   sysinit_on_supervisor_missing,
                                   0) != 0) {
        g_op.kind = SYSINIT_OP_NONE;
        sysinit_log_failure("service", spec, 0);
        return -1;
    }
    return WASMOS_SCRIPT_HOOK_PENDING;
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

/* ------------------------------------------------------------- the loop */

/* The loop's clock: wait-svc deadlines and the resend of a spawn the process
 * manager answered BUSY. */
static void sysinit_on_tick(void* user) {
    (void)user;
    wasmos_sys_svc_watch_set_expire(&g_watches);
    if (g_op.kind == SYSINIT_OP_SPAWN && g_op.retry_due) {
        g_op.retry_due = 0;
        if (sysinit_spawn_send() != 0) {
            sysinit_log_failure("spawn", g_op.path, 0);
            sysinit_op_finish(-1, 0);
        }
    }
}

/* Messages no intent claims -- a late reply to a request whose command already
 * ended -- are dropped. Never answered: an error reply to an unsolicited
 * message can ping-pong with a peer that does the same. */
static void sysinit_on_unclaimed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

/* Consumes the process manager's acknowledgement of NOTIFY_READY. */
static void sysinit_on_ready_ack(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

/* ------------------------------------------------------------- boot set */

/* Watch for the supervisor when no `service` line found it; zeroed before its
 * first start, as a watch record must be. */
static wasmos_sys_svc_watch_t g_boot_watch;

static void sysinit_on_boot_submitted_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != SUPERVISOR_IPC_RESP) {
        sysinit_log_failure(
            "boot set", "supervisor", msg->type == SUPERVISOR_IPC_ERROR ? msg->arg0 : 0);
    }
}

/* Sends the announcement and stops the tick: nothing is left to expire. */
static void sysinit_send_boot_submitted(void) {
    g_loop.poll_timeout_ms = 0;
    if (wasmos_sys_intent_send(&g_loop,
                               g_supervisor_ep,
                               g_state.reply_endpoint,
                               SUPERVISOR_IPC_BOOT_SUBMITTED_REQ,
                               0,
                               0,
                               0,
                               0,
                               sysinit_on_boot_submitted_reply,
                               0,
                               0) != 0) {
        sysinit_log_failure("boot set", "supervisor", 0);
    }
}

static void sysinit_on_boot_supervisor_bound(void* user, wasmos_sys_svc_watch_t* watch,
                                             int32_t endpoint) {
    (void)user;
    wasmos_sys_svc_watch_stop(watch);
    g_supervisor_ep = endpoint;
    sysinit_send_boot_submitted();
}

static void sysinit_on_boot_supervisor_missing(void* user, wasmos_sys_svc_watch_t* watch,
                                               int32_t status) {
    (void)user;
    (void)watch;
    g_loop.poll_timeout_ms = 0;
    sysinit_log_failure("boot set", "supervisor", status);
}

/* sysinit.rc has run to its end: tell the supervisor the boot set is complete,
 * finding it first when no `service` line has. The boot milestone is the
 * supervisor's to decide from here. */
static void sysinit_submit_boot_set(void) {
    if (g_supervisor_ep >= 0) {
        sysinit_send_boot_submitted();
        return;
    }
    if (wasmos_sys_svc_watch_start(&g_watches,
                                   &g_boot_watch,
                                   "supervisor",
                                   SYSINIT_WAIT_SVC_TIMEOUT_MS,
                                   sysinit_on_boot_supervisor_bound,
                                   sysinit_on_boot_supervisor_missing,
                                   0) != 0) {
        g_loop.poll_timeout_ms = 0;
        sysinit_log_failure("boot set", "supervisor", 0);
    }
}

/* Service entry point. Reports ready first (sysinit has no readiness dependency
 * of its own), then interprets sysinit.rc, stepping the runner whenever it can
 * make progress and otherwise parking in the loop until a reply, an event or a
 * tick completes the command in flight. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    wasmos_script_ops_t ops = {0};

    g_console_write = wasmos_console_write;

    g_state.reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_state.reply_endpoint < 0) {
        fatal_stall("[sysinit] failed to create reply endpoint\n");
    }
    if (proc_endpoint < 0) {
        fatal_stall("[sysinit] invalid init args\n");
    }
    g_state.proc_endpoint = proc_endpoint;

    wasmos_sys_event_loop_init(&g_loop, g_state.reply_endpoint, g_state.spawn_request_id);
    (void)wasmos_sys_event_set_default(&g_loop, sysinit_on_unclaimed, 0);
    g_loop.on_timeout = sysinit_on_tick;
    if (wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, g_state.proc_endpoint) != 0) {
        fatal_stall("[sysinit] failed to set up service watches\n");
    }
    (void)wasmos_sys_intent_send(&g_loop,
                                 g_state.proc_endpoint,
                                 g_state.reply_endpoint,
                                 PROC_IPC_NOTIFY_READY,
                                 0,
                                 0,
                                 0,
                                 0,
                                 sysinit_on_ready_ack,
                                 0,
                                 0);

    wasmos_script_state_init(&g_script_state);
    ops.on_start = sysinit_on_start;
    ops.on_spawn = sysinit_on_spawn;
    ops.on_exec = sysinit_on_exec;
    ops.on_wait_svc = sysinit_on_wait_svc;
    ops.on_service = sysinit_on_service;
    ops.on_echo = sysinit_on_echo;
    ops.on_export = sysinit_on_export;
    ops.user = &g_state;

    /* TODO: the runner reads script files through libc fopen/fgets, which are
     * synchronous filesystem requests; a slow or absent filesystem still stalls
     * a step. */
    if (wasmos_script_runner_start(&g_runner, &g_script_state, &ops, SYSINIT_SCRIPT_PATH) != 0) {
        fatal_stall("[sysinit] script failed or not found\n");
    }
    /* The tick runs only while a command is in flight; an idle sysinit parks
     * until a message arrives. */
    g_loop.poll_timeout_ms = SYSINIT_TICK_MS;
    for (;;) {
        int step = wasmos_script_runner_step(&g_runner);
        if (step == WASMOS_SCRIPT_STEP_MORE) {
            continue;
        }
        if (step == WASMOS_SCRIPT_STEP_DONE) {
            break;
        }
        if (step == WASMOS_SCRIPT_STEP_ERROR) {
            fatal_stall("[sysinit] script failed or not found\n");
        }
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
    }
    /* The tick runs on until the announcement is sent: a watch for the
     * supervisor may still have to expire. */
    sysinit_submit_boot_set();
    for (;;) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
    }
}
