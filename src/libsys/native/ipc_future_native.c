/* ipc_future_native.c - native non-blocking IPC request to local future bridge. */
#include "wasmos/libsys_native.h"

static int32_t native_ipc_future_status(int32_t status) {
    return status < 0 ? status : -1;
}

static void native_ipc_future_reply(void* user, const nd_ipc_message_t* reply) {
    wasmos_sys_native_ipc_future_t* operation = (wasmos_sys_native_ipc_future_t*)user;
    int32_t status = 0;

    if (!operation || !reply) {
        return;
    }
    operation->reply = *reply;
    operation->active = 0u;
    operation->request_id = 0u;
    if (operation->reply_status) {
        status = operation->reply_status(operation->user, &operation->reply);
    }
    if (status == 0) {
        (void)wasmos_promise_resolve(&operation->promise, (uintptr_t)&operation->reply);
    } else {
        (void)wasmos_promise_reject(&operation->promise, native_ipc_future_status(status));
    }
}

void wasmos_sys_native_ipc_future_init(
    wasmos_sys_native_ipc_future_t* operation,
    wasmos_sys_native_ipc_future_reply_status_fn reply_status, void* user) {
    if (!operation) {
        return;
    }
    *operation = (wasmos_sys_native_ipc_future_t){0};
    wasmos_future_init(&operation->future, &operation->promise);
    operation->reply_status = reply_status;
    operation->user = user;
}

wasmos_future_t* wasmos_sys_native_ipc_future_send(
    wasmos_sys_native_event_loop_t* loop, wasmos_sys_native_ipc_future_t* operation,
    uint32_t destination_endpoint, uint32_t source_endpoint, uint32_t msg_type, uint32_t arg0,
    uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t* out_request_id) {
    uint32_t request_id = 0u;
    int32_t status;

    if (out_request_id) {
        *out_request_id = 0u;
    }
    if (!loop || !operation || operation->active ||
        operation->future.state != WASMOS_FUTURE_PENDING) {
        return 0;
    }
    status = wasmos_sys_native_intent_send(loop, destination_endpoint, source_endpoint, msg_type,
                                           arg0, arg1, arg2, arg3, native_ipc_future_reply,
                                           operation, &request_id);
    if (status != 0) {
        (void)wasmos_promise_reject(&operation->promise, native_ipc_future_status(status));
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

void wasmos_sys_native_ipc_future_cancel(wasmos_sys_native_ipc_future_t* operation,
                                         int32_t status) {
    if (!operation || !operation->active) {
        return;
    }
    wasmos_sys_native_intent_cancel(operation->loop, operation->request_id);
    operation->active = 0u;
    operation->request_id = 0u;
    (void)wasmos_promise_reject(&operation->promise, native_ipc_future_status(status));
}
