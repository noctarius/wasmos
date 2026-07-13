/* wamos_script_broker - a real subsystem broker for shebang (`#!`) scripts.
 *
 * Unlike the earlier hardcoded smoke fixture, this broker derives its spawn
 * plan from the guest file's contents: it reads the `#!<interpreter>` line from
 * the guest blob PM lent it, maps the interpreter name to a known host `.wap`
 * executor, and returns a WAP_PATH plan that launches that executor with the
 * guest script path as its argv.  PM then reloads the executor and runs it.
 *
 * Registration is gated by the `subsystem.register` capability (declared in
 * this service's manifest); PM rejects the register requests otherwise. */
#include "stdio.h"
#include "string.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"

#define SCRIPT_REQUEST_TAG   "WSCRIPT"
#define SCRIPT_RUNTIME_TAG   "WSCRIPT"
#define SCRIPT_HANDLER_NAME  "shebang-script"
/* Interpreter name understood in the guest `#!` line, and the host executor
 * `.wap` it resolves to. */
#define SCRIPT_INTERP_NAME   "wamos-script"
#define SCRIPT_HOST_PATH     "/init/apps/wamos_script.wap"
#define SCRIPT_PROBE_BYTES   2u

/* Distinct service exit statuses (named-error-code policy).  Per-request plan
 * failures are reported to PM with the shared PROC_SPAWN_ERR_* codes below and
 * do not terminate the broker; these codes cover only fatal startup/serve
 * errors that end the service.  0 is success. */
enum {
    SCRIPT_BROKER_OK = 0,
    SCRIPT_BROKER_ERR_ENDPOINT = -2,   /* broker endpoint create failed */
    SCRIPT_BROKER_ERR_REGISTER = -3,   /* subsystem/handler registration rejected by PM */
    SCRIPT_BROKER_ERR_REPLY = -4       /* failed to send a plan reply to PM */
};

/* Match any file whose first two bytes are "#!". */
static const wasmos_exec_match_node_t g_script_match_nodes[] = {
    {
        .kind = WASMOS_EXEC_MATCH_PREFIX,
        .left_index = 0u,
        .right_index = 0u,
        .value_len = 2u,
        .value.prefix = { '#', '!' },
    },
};

static int32_t
script_request_string_at(int32_t buffer_id,
                         uint32_t offset,
                         uint32_t len,
                         char *out_text,
                         uint32_t out_text_size)
{
    if (!out_text || out_text_size == 0u) {
        return -1;
    }
    out_text[0] = '\0';
    if (len == 0u) {
        return 0;
    }
    if (len + 1u > out_text_size) {
        return -1;
    }
    if (wasmos_xfer_buffer_read(buffer_id,
                                (int32_t)(uintptr_t)out_text,
                                (int32_t)len,
                                (int32_t)offset) != 0) {
        return -1;
    }
    out_text[len] = '\0';
    return 0;
}

/* Copy the leading bytes of the guest blob and extract the interpreter token
 * from a "#!<interpreter>" first line into out_interp. */
static int32_t
script_read_interpreter(int32_t buffer_id,
                        uint32_t blob_offset,
                        uint32_t blob_size,
                        char *out_interp,
                        uint32_t out_interp_size)
{
    char head[64];
    uint32_t copy_len = blob_size;
    uint32_t i = 0u;
    uint32_t out = 0u;

    if (!out_interp || out_interp_size == 0u) {
        return -1;
    }
    out_interp[0] = '\0';
    if (copy_len == 0u) {
        return -1;
    }
    if (copy_len > sizeof(head) - 1u) {
        copy_len = sizeof(head) - 1u;
    }
    if (wasmos_xfer_buffer_read(buffer_id,
                                (int32_t)(uintptr_t)head,
                                (int32_t)copy_len,
                                (int32_t)blob_offset) != 0) {
        return -1;
    }
    head[copy_len] = '\0';
    if (head[0] != '#' || head[1] != '!') {
        return -1;
    }
    i = 2u;
    while (i < copy_len && (head[i] == ' ' || head[i] == '\t')) {
        i++;
    }
    while (i < copy_len && head[i] != '\0' && head[i] != '\n' && head[i] != '\r' &&
           head[i] != ' ' && head[i] != '\t') {
        if (out + 1u >= out_interp_size) {
            return -1;
        }
        out_interp[out++] = head[i++];
    }
    out_interp[out] = '\0';
    return out > 0u ? 0 : -1;
}

static int32_t
script_plan_write_string(int32_t buffer_id,
                         uint32_t *io_offset,
                         const char *text,
                         uint32_t *out_offset,
                         uint32_t *out_len)
{
    uint32_t len = 0u;
    int32_t buf_size = wasmos_xfer_buffer_size();

    if (!io_offset || !text || !out_offset || !out_len || buf_size <= 0) {
        return -1;
    }
    len = (uint32_t)strlen(text);
    if (*io_offset >= (uint32_t)buf_size || len + 1u > ((uint32_t)buf_size - *io_offset)) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(buffer_id, (int32_t)(uintptr_t)text, (int32_t)len, (int32_t)*io_offset) != 0) {
        return -1;
    }
    {
        static const char nul = '\0';
        if (wasmos_xfer_buffer_write(buffer_id, (int32_t)(uintptr_t)&nul, 1, (int32_t)(*io_offset + len)) != 0) {
            return -1;
        }
    }
    *out_offset = *io_offset;
    *out_len = len;
    *io_offset += len + 1u;
    return 0;
}

static int32_t
script_register(int32_t proc_endpoint, int32_t broker_endpoint)
{
    if (wasmos_subsystem_register_broker(proc_endpoint,
                                         broker_endpoint,
                                         SCRIPT_REQUEST_TAG,
                                         SCRIPT_RUNTIME_TAG,
                                         SCRIPT_REQUEST_TAG,
                                         0,
                                         0,
                                         0,
                                         1) != 0) {
        puts("[wamos-script-broker] register broker failed");
        return SCRIPT_BROKER_ERR_REGISTER;
    }
    if (wasmos_exec_handler_register(proc_endpoint,
                                     SCRIPT_REQUEST_TAG,
                                     SCRIPT_HANDLER_NAME,
                                     100,
                                     (int32_t)SCRIPT_PROBE_BYTES,
                                     g_script_match_nodes,
                                     1,
                                     0,
                                     2) != 0) {
        puts("[wamos-script-broker] register handler failed");
        return SCRIPT_BROKER_ERR_REGISTER;
    }
    return SCRIPT_BROKER_OK;
}

int32_t
initialize(int32_t proc_endpoint)
{
    /* proc.endpoint now comes from the spawn-info contract, not an entry arg. */
    proc_endpoint = wasmos_startup_proc_endpoint();
    int32_t broker_endpoint = wasmos_ipc_create_endpoint();
    int32_t register_rc = 0;

    if (broker_endpoint < 0) {
        puts("[wamos-script-broker] endpoint failed");
        return SCRIPT_BROKER_ERR_ENDPOINT;
    }
    register_rc = script_register(proc_endpoint, broker_endpoint);
    if (register_rc != SCRIPT_BROKER_OK) {
        return register_rc;
    }
    wasmos_sys_notify_ready(proc_endpoint, broker_endpoint);

    for (;;) {
        wasmos_ipc_message_t msg;
        wasmos_broker_spawn_plan_request_t request;
        wasmos_broker_spawn_plan_response_t plan;
        char guest_path[96];
        char interp[32];
        uint32_t plan_cursor = 0u;
        uint32_t path_offset = 0u;
        uint32_t path_len = 0u;
        uint32_t args_offset = 0u;
        uint32_t args_len = 0u;

        if (wasmos_ipc_select_one(broker_endpoint) < 0) {
            continue;
        }
        wasmos_ipc_message_read_last(&msg);
        if (msg.type != PROC_BROKER_IPC_SPAWN_PLAN_REQ) {
            continue;
        }
        if (msg.arg1 < (int32_t)sizeof(request) || msg.arg0 < 0) {
            puts("[dbg-broker] bad request envelope");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        /* PM owns the buffer (msg.arg2 = buffer_id) and, owner-push, granted this
         * broker R|W. Read/write by buffer_id; PM unborrows the grant on its side
         * after the reply (it holds the borrow handle), so the broker does not. */
        if (wasmos_xfer_buffer_read(msg.arg2,
                                    (int32_t)(uintptr_t)&request,
                                    (int32_t)sizeof(request),
                                    msg.arg0) != 0) {
            puts("[dbg-broker] request read failed");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_IPC, 0, 0);
            continue;
        }
        /* Validate the request identity, extract the guest path (used as the
         * host executor's argv), and derive the interpreter from the guest's
         * `#!` line. */
        if (request.version != WASMOS_BROKER_SPAWN_PLAN_VERSION) {
            puts("[dbg-broker] bad version");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        if (strcmp(request.request_tag, SCRIPT_REQUEST_TAG) != 0) {
            puts("[dbg-broker] bad request tag");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        if (script_request_string_at(msg.arg2, request.path_offset, request.path_len,
                                     guest_path, sizeof(guest_path)) != 0) {
            puts("[dbg-broker] guest path read failed");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        if (guest_path[0] == '\0') {
            puts("[dbg-broker] empty guest path");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        if (script_read_interpreter(msg.arg2, request.blob_offset, request.blob_size,
                                    interp, sizeof(interp)) != 0) {
            puts("[dbg-broker] interpreter read failed");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        if (strcmp(interp, SCRIPT_INTERP_NAME) != 0) {
            puts("[dbg-broker] interpreter mismatch");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }
        memset(&plan, 0, sizeof(plan));
        plan.version = WASMOS_BROKER_SPAWN_PLAN_VERSION;
        plan.plan_kind = WASMOS_BROKER_PLAN_KIND_WAP_PATH;
        memcpy(plan.request_tag, SCRIPT_REQUEST_TAG, sizeof(SCRIPT_REQUEST_TAG));
        memcpy(plan.runtime_tag, SCRIPT_RUNTIME_TAG, sizeof(SCRIPT_RUNTIME_TAG));
        plan_cursor = (uint32_t)sizeof(plan);
        if (wasmos_xfer_buffer_write(msg.arg2, (int32_t)(uintptr_t)&plan, (int32_t)sizeof(plan), 0) != 0 ||
            script_plan_write_string(msg.arg2, &plan_cursor, SCRIPT_HOST_PATH, &path_offset, &path_len) != 0 ||
            script_plan_write_string(msg.arg2, &plan_cursor, guest_path, &args_offset, &args_len) != 0) {
            puts("[dbg-broker] plan write failed");
            (void)wasmos_ipc_send(msg.source, broker_endpoint, PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id, 0, PROC_SPAWN_ERR_BROKER_PLAN, 0, 0);
            continue;
        }

        plan.host_path_offset = path_offset;
        plan.host_path_len = path_len;
        plan.host_args_offset = args_offset;
        plan.host_args_len = args_len;
        if (wasmos_xfer_buffer_write(msg.arg2, (int32_t)(uintptr_t)&plan, (int32_t)sizeof(plan), 0) != 0 ||
            wasmos_ipc_send(msg.source,
                            broker_endpoint,
                            PROC_BROKER_IPC_SPAWN_PLAN_RESP,
                            msg.request_id,
                            0,
                            (int32_t)plan_cursor,
                            0,
                            0) != 0) {
            puts("[wamos-script-broker] reply failed");
            return SCRIPT_BROKER_ERR_REPLY;
        }
        puts("[wamos-script-broker] plan ok");
    }
}
