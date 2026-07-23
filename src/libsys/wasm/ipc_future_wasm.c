/* ipc_future_wasm.c - non-blocking WASM IPC intent to local future bridge. */
#include "wasmos/libsys.h"

static int32_t ipc_future_status(int32_t status) { return status < 0 ? status : -1; }

static void ipc_future_reply(void* user, const wasmos_ipc_message_t* reply) {
    wasmos_sys_wasm_ipc_future_t* operation = user;
    int32_t status = 0;

    if (!operation || !reply) {
        return;
    }
    operation->reply = *reply;
    operation->active = 0u;
    operation->request_id = 0;
    if (operation->reply_status) {
        status = operation->reply_status(operation->user, &operation->reply);
    }
    if (status == 0) {
        (void)wasmos_promise_resolve(&operation->promise, (uintptr_t)&operation->reply);
    } else {
        (void)wasmos_promise_reject(&operation->promise, ipc_future_status(status));
    }
}

void wasmos_sys_wasm_ipc_future_init(
    wasmos_sys_wasm_ipc_future_t* operation,
    wasmos_sys_wasm_ipc_future_reply_status_fn reply_status, void* user) {
    if (!operation) {
        return;
    }
    *operation = (wasmos_sys_wasm_ipc_future_t){0};
    wasmos_future_init(&operation->future, &operation->promise);
    operation->reply_status = reply_status;
    operation->user = user;
}

wasmos_future_t* wasmos_sys_wasm_ipc_future_send(
    wasmos_sys_event_loop_t* loop, wasmos_sys_wasm_ipc_future_t* operation,
    int32_t destination_endpoint, int32_t source_endpoint, int32_t msg_type,
    int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3, int32_t* out_request_id) {
    int32_t request_id = 0;
    int32_t status;

    if (out_request_id) {
        *out_request_id = 0;
    }
    if (!loop || !operation || operation->active ||
        operation->future.state != WASMOS_FUTURE_PENDING) {
        return NULL;
    }
    status = wasmos_sys_intent_send(loop, destination_endpoint, source_endpoint, msg_type,
                                    arg0, arg1, arg2, arg3, ipc_future_reply, operation,
                                    &request_id);
    if (status != 0) {
        (void)wasmos_promise_reject(&operation->promise, ipc_future_status(status));
        return &operation->future;
    }
    operation->loop = loop;
    operation->request_id = request_id;
    operation->active = 1u;
    if (out_request_id) {
        *out_request_id = request_id;
    }
    return &operation->future;
}

void wasmos_sys_wasm_ipc_future_cancel(wasmos_sys_wasm_ipc_future_t* operation,
                                       int32_t status) {
    if (!operation || !operation->active) {
        return;
    }
    wasmos_sys_intent_cancel(operation->loop, operation->request_id);
    operation->active = 0u;
    operation->request_id = 0;
    (void)wasmos_promise_reject(&operation->promise, ipc_future_status(status));
}

const wasmos_ipc_message_t*
wasmos_sys_wasm_ipc_future_reply(const wasmos_sys_wasm_ipc_future_t* operation) {
    return operation ? &operation->reply : NULL;
}
