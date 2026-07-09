#include "stdio.h"
#include "string.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"

#define BROKER_TEST_REQUEST_TAG "BRTST"
#define BROKER_TEST_HANDLER_NAME "broker-smoke"
#define BROKER_TEST_PATH "/init/apps/broker_smoke.bro"
#define BROKER_TEST_HOST_PATH "/init/apps/broker_host_smoke.wap"
#define BROKER_TEST_HOST_ARGS "from-broker"

static const wasmos_exec_match_node_t g_broker_test_match_nodes[] = {
    {
        .kind = WASMOS_EXEC_MATCH_EXTENSION,
        .left_index = 0u,
        .right_index = 0u,
        .value_len = 4u,
        .value.text = ".bro",
    },
};

static int32_t
broker_request_string_at(int32_t source_endpoint,
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
    if (wasmos_sys_buffer_copy_from(WASMOS_BUFFER_KIND_FS,
                                    source_endpoint,
                                    WASMOS_BUFFER_GRANT_READ,
                                    out_text,
                                    (int32_t)len,
                                    (int32_t)offset) != 0) {
        return -1;
    }
    out_text[len] = '\0';
    return 0;
}

static int32_t
broker_plan_write_string(uint32_t *io_offset,
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
    if (wasmos_xfer_buffer_write((int32_t)(uintptr_t)text, (int32_t)len, (int32_t)*io_offset) != 0) {
        return -1;
    }
    {
        static const char nul = '\0';
        if (wasmos_xfer_buffer_write((int32_t)(uintptr_t)&nul, 1, (int32_t)(*io_offset + len)) != 0) {
            return -1;
        }
    }
    *out_offset = *io_offset;
    *out_len = len;
    *io_offset += len + 1u;
    return 0;
}

static int32_t
broker_register(int32_t proc_endpoint, int32_t broker_endpoint)
{
    if (wasmos_subsystem_register_broker(proc_endpoint,
                                         broker_endpoint,
                                         BROKER_TEST_REQUEST_TAG,
                                         "NATIVE",
                                         BROKER_TEST_REQUEST_TAG,
                                         0,
                                         0,
                                         0,
                                         1) != 0) {
        puts("[test] broker service register broker failed");
        return -1;
    }
    if (wasmos_exec_handler_register(proc_endpoint,
                                     BROKER_TEST_REQUEST_TAG,
                                     BROKER_TEST_HANDLER_NAME,
                                     100,
                                     1,
                                     g_broker_test_match_nodes,
                                     1,
                                     0,
                                     2) != 0) {
        puts("[test] broker service register handler failed");
        return -1;
    }
    return 0;
}

int32_t
initialize(int32_t proc_endpoint)
{
    int32_t broker_endpoint = -1;

    broker_endpoint = wasmos_ipc_create_endpoint();
    if (broker_endpoint < 0) {
        puts("[test] broker service endpoint failed");
        return -1;
    }
    if (broker_register(proc_endpoint, broker_endpoint) != 0) {
        return -1;
    }
    wasmos_sys_notify_ready(proc_endpoint, broker_endpoint);

    for (;;) {
        wasmos_ipc_message_t msg;
        wasmos_broker_spawn_plan_request_t request;
        wasmos_broker_spawn_plan_response_t plan;
        char request_path[96];
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
            (void)wasmos_ipc_send(msg.source,
                                  broker_endpoint,
                                  PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id,
                                  0,
                                  PROC_SPAWN_ERR_BROKER_PLAN,
                                  0,
                                  0);
            continue;
        }
        if (wasmos_sys_buffer_copy_from(WASMOS_BUFFER_KIND_FS,
                                        msg.source,
                                        WASMOS_BUFFER_GRANT_READ,
                                        &request,
                                        (int32_t)sizeof(request),
                                        msg.arg0) != 0) {
            (void)wasmos_ipc_send(msg.source,
                                  broker_endpoint,
                                  PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id,
                                  0,
                                  PROC_SPAWN_ERR_BROKER_IPC,
                                  0,
                                  0);
            continue;
        }
        if (request.version != WASMOS_BROKER_SPAWN_PLAN_VERSION ||
            strcmp(request.request_tag, BROKER_TEST_REQUEST_TAG) != 0 ||
            strcmp(request.runtime_tag, "NATIVE") != 0 ||
            broker_request_string_at(msg.source,
                                     request.path_offset,
                                     request.path_len,
                                     request_path,
                                     sizeof(request_path)) != 0 ||
            strcmp(request_path, BROKER_TEST_PATH) != 0) {
            (void)wasmos_ipc_send(msg.source,
                                  broker_endpoint,
                                  PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id,
                                  0,
                                  PROC_SPAWN_ERR_BROKER_PLAN,
                                  0,
                                  0);
            continue;
        }

        memset(&plan, 0, sizeof(plan));
        plan.version = WASMOS_BROKER_SPAWN_PLAN_VERSION;
        plan.plan_kind = WASMOS_BROKER_PLAN_KIND_WAP_PATH;
        memcpy(plan.request_tag, BROKER_TEST_REQUEST_TAG, sizeof(BROKER_TEST_REQUEST_TAG));
        memcpy(plan.runtime_tag, "NATIVE", sizeof("NATIVE"));
        plan_cursor = (uint32_t)sizeof(plan);
        if (wasmos_xfer_buffer_write((int32_t)(uintptr_t)&plan, (int32_t)sizeof(plan), 0) != 0 ||
            broker_plan_write_string(&plan_cursor, BROKER_TEST_HOST_PATH, &path_offset, &path_len) != 0 ||
            broker_plan_write_string(&plan_cursor, BROKER_TEST_HOST_ARGS, &args_offset, &args_len) != 0) {
            (void)wasmos_ipc_send(msg.source,
                                  broker_endpoint,
                                  PROC_BROKER_IPC_SPAWN_PLAN_ERROR,
                                  msg.request_id,
                                  0,
                                  PROC_SPAWN_ERR_BROKER_PLAN,
                                  0,
                                  0);
            continue;
        }

        plan.host_path_offset = path_offset;
        plan.host_path_len = path_len;
        plan.host_args_offset = args_offset;
        plan.host_args_len = args_len;
        if (wasmos_xfer_buffer_write((int32_t)(uintptr_t)&plan, (int32_t)sizeof(plan), 0) != 0 ||
            wasmos_ipc_send(msg.source,
                            broker_endpoint,
                            PROC_BROKER_IPC_SPAWN_PLAN_RESP,
                            msg.request_id,
                            0,
                            (int32_t)plan_cursor,
                            0,
                            0) != 0) {
            puts("[test] broker service reply failed");
            return -1;
        }
        puts("[test] broker stub plan ok");
    }
}
