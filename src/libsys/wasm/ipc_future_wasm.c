/* ipc_future_wasm.c - non-blocking WASM IPC intent to local future bridge. */
#include "wasmos/libsys.h"

static int32_t ipc_future_status(int32_t status) {
    return status < 0 ? status : -1;
}

/* libsys exposes the event loop as header inlines for C.  Language bindings
 * link against symbols, so these two shims give them callable exports. */
void wasmos_sys_wasm_event_loop_init(wasmos_sys_event_loop_t* loop, int32_t endpoint,
                                     int32_t request_id_base) {
    wasmos_sys_event_loop_init(loop, endpoint, request_id_base);
}
int32_t wasmos_sys_wasm_event_loop_poll(wasmos_sys_event_loop_t* loop, int32_t budget) {
    return wasmos_sys_event_loop_poll(loop, budget);
}

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

void wasmos_sys_wasm_ipc_future_init(wasmos_sys_wasm_ipc_future_t* operation,
                                     wasmos_sys_wasm_ipc_future_reply_status_fn reply_status,
                                     void* user) {
    if (!operation) {
        return;
    }
    *operation = (wasmos_sys_wasm_ipc_future_t){0};
    wasmos_future_init(&operation->future, &operation->promise);
    operation->reply_status = reply_status;
    operation->user = user;
}

wasmos_future_t* wasmos_sys_wasm_ipc_future_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_ipc_future_t* operation,
                                                 int32_t destination_endpoint,
                                                 int32_t source_endpoint, int32_t msg_type,
                                                 int32_t arg0, int32_t arg1, int32_t arg2,
                                                 int32_t arg3, int32_t* out_request_id) {
    int32_t request_id = 0;
    int32_t status;

    if (out_request_id) {
        *out_request_id = 0;
    }
    if (!loop || !operation || operation->active ||
        operation->future.state != WASMOS_FUTURE_PENDING) {
        return NULL;
    }
    status = wasmos_sys_intent_send(loop, destination_endpoint, source_endpoint, msg_type, arg0,
                                    arg1, arg2, arg3, ipc_future_reply, operation, &request_id);
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

void wasmos_sys_wasm_ipc_future_cancel(wasmos_sys_wasm_ipc_future_t* operation, int32_t status) {
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

static int32_t fs_reply_status(void* user, const wasmos_ipc_message_t* reply) {
    (void)user;
    return reply && reply->type == FS_IPC_RESP ? 0 : -1;
}

void wasmos_sys_wasm_fs_request_init(wasmos_sys_wasm_fs_request_t* request) {
    if (!request) {
        return;
    }
    wasmos_sys_wasm_ipc_future_init(&request->ipc, fs_reply_status, NULL);
}

wasmos_future_t* wasmos_sys_wasm_fs_request_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_fs_request_t* request,
                                                 int32_t fs_endpoint, int32_t reply_endpoint,
                                                 int32_t msg_type, int32_t arg0, int32_t arg1,
                                                 int32_t arg2, int32_t arg3,
                                                 int32_t* out_request_id) {
    if (!request || fs_endpoint < 0 || reply_endpoint < 0) {
        return NULL;
    }
    return wasmos_sys_wasm_ipc_future_send(loop, &request->ipc, fs_endpoint, reply_endpoint,
                                           msg_type, arg0, arg1, arg2, arg3, out_request_id);
}

const wasmos_ipc_message_t*
wasmos_sys_wasm_fs_request_reply(const wasmos_sys_wasm_fs_request_t* request) {
    return request ? wasmos_sys_wasm_ipc_future_reply(&request->ipc) : NULL;
}

void wasmos_sys_wasm_fs_operation_init(wasmos_sys_wasm_fs_operation_t* operation) {
    if (!operation)
        return;
    *operation = (wasmos_sys_wasm_fs_operation_t){.buffer_id = -1, .buffer_borrow = -1};
    wasmos_sys_wasm_fs_request_init(&operation->request);
}

/* Starts are deliberately self-initialising: exposing this reset burden to
 * every language wrapper makes reuse error-prone.  A live transfer buffer
 * still requires finish() first, because it is the caller's data lifetime. */
static int fs_operation_prepare(wasmos_sys_wasm_fs_operation_t* operation) {
    if (!operation || operation->has_buffer || operation->request.ipc.active)
        return -1;
    wasmos_sys_wasm_fs_operation_init(operation);
    return 0;
}

static void fs_operation_release(wasmos_sys_wasm_fs_operation_t* operation) {
    if (operation && operation->has_buffer) {
        (void)wasmos_xfer_buffer_release(operation->buffer_id);
        operation->buffer_id = -1;
        operation->buffer_borrow = -1;
        operation->has_buffer = 0;
    }
}

static wasmos_future_t* fs_operation_buffer_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_fs_operation_t* operation,
                                                 int32_t fs_endpoint, int32_t reply_endpoint,
                                                 int32_t type, const void* data, int32_t length,
                                                 int32_t arg0, int32_t arg1,
                                                 int32_t* out_request_id) {
    int32_t bid, borrow;
    if (!operation || length <= 0)
        return NULL;
    bid = wasmos_xfer_buffer_acquire(length);
    if (bid < 0 ||
        (data && wasmos_xfer_buffer_write(bid, addr_cast(int32_t, data), length, 0) != 0)) {
        if (bid >= 0)
            (void)wasmos_xfer_buffer_release(bid);
        return NULL;
    }
    borrow = wasmos_xfer_buffer_borrow(fs_endpoint, bid, 3 /* GRANT_READ|GRANT_WRITE */);
    if (borrow < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return NULL;
    }
    operation->buffer_id = bid;
    operation->buffer_borrow = borrow;
    operation->length = length;
    operation->has_buffer = 1;
    wasmos_future_t* future =
        wasmos_sys_wasm_fs_request_send(loop, &operation->request, fs_endpoint, reply_endpoint,
                                        type, arg0, arg1, bid, borrow, out_request_id);
    if (!future)
        fs_operation_release(operation);
    return future;
}

static wasmos_future_t* fs_operation_path_send(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               int32_t type, const char* path, int32_t arg1,
                                               int32_t* out_request_id) {
    int32_t length = 0;
    if (!path)
        return NULL;
    while (path[length])
        ++length;
    return fs_operation_buffer_send(loop, operation, fs_endpoint, reply_endpoint, type, path,
                                    length + 1, length, arg1, out_request_id);
}

wasmos_future_t* wasmos_sys_wasm_fs_open_async(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               const char* path, int32_t flags,
                                               int32_t* out_request_id) {
    if (fs_operation_prepare(operation) != 0)
        return NULL;
    return fs_operation_path_send(loop, operation, fs_endpoint, reply_endpoint, FS_IPC_OPEN_REQ,
                                  path, flags, out_request_id);
}
wasmos_future_t* wasmos_sys_wasm_fs_read_async(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               int32_t fd, void* dst, int32_t len,
                                               int32_t* out_request_id) {
    if (fs_operation_prepare(operation) != 0)
        return NULL;
    (void)dst; /* destination is supplied to finish after reply validation. */
    return fs_operation_buffer_send(loop, operation, fs_endpoint, reply_endpoint, FS_IPC_READ_REQ,
                                    NULL, len, fd, len, out_request_id);
}
wasmos_future_t* wasmos_sys_wasm_fs_write_async(wasmos_sys_event_loop_t* loop,
                                                wasmos_sys_wasm_fs_operation_t* operation,
                                                int32_t fs_endpoint, int32_t reply_endpoint,
                                                int32_t fd, const void* src, int32_t len,
                                                int32_t* out_request_id) {
    if (fs_operation_prepare(operation) != 0)
        return NULL;
    return fs_operation_buffer_send(loop, operation, fs_endpoint, reply_endpoint, FS_IPC_WRITE_REQ,
                                    src, len, fd, len, out_request_id);
}
wasmos_future_t* wasmos_sys_wasm_fs_close_async(wasmos_sys_event_loop_t* loop,
                                                wasmos_sys_wasm_fs_operation_t* operation,
                                                int32_t fs_endpoint, int32_t reply_endpoint,
                                                int32_t fd, int32_t* out_request_id) {
    if (fs_operation_prepare(operation) != 0)
        return NULL;
    return wasmos_sys_wasm_fs_request_send(loop, &operation->request, fs_endpoint, reply_endpoint,
                                           FS_IPC_CLOSE_REQ, fd, 0, 0, 0, out_request_id);
}
wasmos_future_t* wasmos_sys_wasm_fs_unlink_async(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_fs_operation_t* operation,
                                                 int32_t fs_endpoint, int32_t reply_endpoint,
                                                 const char* path, int32_t* out_request_id) {
    if (fs_operation_prepare(operation) != 0)
        return NULL;
    return fs_operation_path_send(loop, operation, fs_endpoint, reply_endpoint, FS_IPC_UNLINK_REQ,
                                  path, 0, out_request_id);
}
wasmos_future_t* wasmos_sys_wasm_fs_stat_async(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               const char* path, int32_t* out_request_id) {
    if (fs_operation_prepare(operation) != 0)
        return NULL;
    return fs_operation_path_send(loop, operation, fs_endpoint, reply_endpoint, FS_IPC_STAT_REQ,
                                  path, 0, out_request_id);
}
int32_t wasmos_sys_wasm_fs_operation_finish(wasmos_sys_wasm_fs_operation_t* operation,
                                            void* read_dst, int32_t read_capacity,
                                            wasmos_ipc_message_t* out_reply) {
    const wasmos_ipc_message_t* reply;
    int32_t result = -1;
    if (!operation)
        return -1;
    reply = wasmos_sys_wasm_fs_request_reply(&operation->request);
    if (reply) {
        if (out_reply)
            *out_reply = *reply;
        result = reply->arg0;
        if (read_dst && result >= 0 && result <= read_capacity && result <= operation->length &&
            operation->has_buffer &&
            wasmos_xfer_buffer_read(operation->buffer_id, addr_cast(int32_t, read_dst), result,
                                    0) != 0)
            result = -1;
    }
    fs_operation_release(operation);
    return result;
}
