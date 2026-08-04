#ifndef WASMOS_LIBSYS_H
#define WASMOS_LIBSYS_H

#include <stdint.h>

#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/sha256.h"
#include "wasmos/libsys_string.h"
#include "wasmos/coroutine_wasm.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASMOS_SYS_INTENT_MAX 16
#define WASMOS_SYS_HANDLER_MAX 16

/* Random-helper statuses are the packed hrng domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_HRNG_*. */

typedef struct {
    int32_t in_use;
    int32_t request_id;
    void (*on_resolve)(void* user, const wasmos_ipc_message_t* msg);
    void* user;
} wasmos_sys_intent_t;

typedef struct {
    int32_t in_use;
    int32_t msg_type;
    void (*on_message)(void* user, const wasmos_ipc_message_t* msg);
    void* user;
} wasmos_sys_handler_t;

typedef struct {
    int32_t receiver_endpoint;
    int32_t select_id; /* select-set watching receiver_endpoint; -1 if not created */
    int32_t next_request_id;
    void (*default_on_message)(void* user, const wasmos_ipc_message_t* msg);
    void* default_user;
    wasmos_sys_intent_t intents[WASMOS_SYS_INTENT_MAX];
    wasmos_sys_handler_t handlers[WASMOS_SYS_HANDLER_MAX];
} wasmos_sys_event_loop_t;

/* Caller-owned bridge between one non-blocking IPC intent and a local future.
 * The reply is copied before settlement. reply_status returns zero to resolve
 * or a negative protocol status to reject. Cancellation only stops local
 * reply tracking; transport work may still complete and its late reply is
 * dispatched normally. */
typedef int32_t (*wasmos_sys_wasm_ipc_future_reply_status_fn)(void* user,
                                                              const wasmos_ipc_message_t* reply);

typedef struct {
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_sys_event_loop_t* loop;
    wasmos_ipc_message_t reply;
    wasmos_sys_wasm_ipc_future_reply_status_fn reply_status;
    void* user;
    int32_t request_id;
    uint8_t active;
} wasmos_sys_wasm_ipc_future_t;

/* Filesystem-specialized IPC future. The generic IPC bridge remains available
 * for protocols with their own response shape; this record rejects replies
 * that are not FS_IPC_RESP before application state machines observe them. */
typedef struct {
    wasmos_sys_wasm_ipc_future_t ipc;
} wasmos_sys_wasm_fs_request_t;

/* A typed asynchronous filesystem operation.  It owns the transfer buffer
 * used for its path or payload until finish() is called after its future has
 * settled.  The caller owns this record and its destination/source memory. */
typedef struct {
    wasmos_sys_wasm_fs_request_t request;
    int32_t buffer_id;
    int32_t buffer_borrow;
    int32_t length;
    uint8_t has_buffer;
} wasmos_sys_wasm_fs_operation_t;

typedef struct wasmos_sys_random_request wasmos_sys_random_request_t;
typedef void (*wasmos_sys_random_complete_fn)(void* user, int32_t status);

struct wasmos_sys_random_request {
    wasmos_sys_event_loop_t* loop;
    int32_t hrng_endpoint;
    int32_t buffer_id;
    int32_t chunk_max;
    int32_t len;
    int32_t done;
    uint8_t* out;
    uint32_t float_word;
    float* float_out;
    wasmos_sys_random_complete_fn on_complete;
    void* user;
};

static inline int32_t wasmos_sys_ipc_recv_matching(int32_t reply_endpoint, int32_t request_id,
                                                   wasmos_ipc_message_t* out_reply) {
    for (;;) {
        if (wasmos_ipc_select_one(reply_endpoint) < 0) {
            return -1;
        }
        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);
        if (msg.request_id != request_id) {
            continue;
        }
        if (out_reply) {
            *out_reply = msg;
        }
        return 0;
    }
}

static inline void wasmos_sys_event_loop_init(wasmos_sys_event_loop_t* loop,
                                              int32_t receiver_endpoint, int32_t request_id_base) {
    if (!loop) {
        return;
    }
    loop->receiver_endpoint = receiver_endpoint;
    loop->next_request_id = request_id_base;
    loop->default_on_message = 0;
    loop->default_user = 0;
    /* Create a select-set watching this loop's endpoint so that when the
     * poll budget is exhausted the loop can block instead of busy-spinning.
     * Minos2 design: tasks always block on events, never busy-poll. */
    loop->select_id = -1;
    if (receiver_endpoint >= 0) {
        int32_t sel = wasmos_ipc_select_create();
        if (sel > 0) {
            if (wasmos_ipc_select_add(sel, receiver_endpoint) == 0) {
                loop->select_id = sel;
            } else {
                (void)wasmos_ipc_select_destroy(sel);
            }
        }
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        loop->intents[i].in_use = 0;
        loop->intents[i].request_id = 0;
        loop->intents[i].on_resolve = 0;
        loop->intents[i].user = 0;
    }
    for (int32_t i = 0; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        loop->handlers[i].in_use = 0;
        loop->handlers[i].msg_type = 0;
        loop->handlers[i].on_message = 0;
        loop->handlers[i].user = 0;
    }
}

static inline int32_t
wasmos_sys_event_set_default(wasmos_sys_event_loop_t* loop,
                             void (*on_message)(void* user, const wasmos_ipc_message_t* msg),
                             void* user) {
    if (!loop || !on_message) {
        return -1;
    }
    loop->default_on_message = on_message;
    loop->default_user = user;
    return 0;
}

static inline int32_t wasmos_sys_event_register(wasmos_sys_event_loop_t* loop, int32_t msg_type,
                                                void (*on_message)(void* user,
                                                                   const wasmos_ipc_message_t* msg),
                                                void* user) {
    if (!loop || !on_message) {
        return -1;
    }
    for (int32_t i = 0; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        if (loop->handlers[i].in_use && loop->handlers[i].msg_type == msg_type) {
            loop->handlers[i].on_message = on_message;
            loop->handlers[i].user = user;
            return 0;
        }
    }
    for (int32_t i = 0; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        if (!loop->handlers[i].in_use) {
            loop->handlers[i].in_use = 1;
            loop->handlers[i].msg_type = msg_type;
            loop->handlers[i].on_message = on_message;
            loop->handlers[i].user = user;
            return 0;
        }
    }
    return -1;
}

static inline int32_t
wasmos_sys_intent_send(wasmos_sys_event_loop_t* loop, int32_t destination_endpoint,
                       int32_t source_endpoint, int32_t type, int32_t arg0, int32_t arg1,
                       int32_t arg2, int32_t arg3,
                       void (*on_resolve)(void* user, const wasmos_ipc_message_t* msg), void* user,
                       int32_t* out_request_id) {
    int32_t request_id = 0;
    if (!loop || !on_resolve) {
        return -1;
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (!loop->intents[i].in_use) {
            request_id = loop->next_request_id++;
            loop->intents[i].in_use = 1;
            loop->intents[i].request_id = request_id;
            loop->intents[i].on_resolve = on_resolve;
            loop->intents[i].user = user;
            if (wasmos_ipc_send(destination_endpoint, source_endpoint, type, request_id, arg0, arg1,
                                arg2, arg3) != 0) {
                loop->intents[i].in_use = 0;
                loop->intents[i].request_id = 0;
                loop->intents[i].on_resolve = 0;
                loop->intents[i].user = 0;
                return -1;
            }
            if (out_request_id) {
                *out_request_id = request_id;
            }
            return 0;
        }
    }
    return -1;
}

static inline int32_t wasmos_sys_intent_send_with_request_id(
    wasmos_sys_event_loop_t* loop, int32_t destination_endpoint, int32_t source_endpoint,
    int32_t request_id, int32_t type, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
    void (*on_resolve)(void* user, const wasmos_ipc_message_t* msg), void* user) {
    if (!loop || !on_resolve || request_id <= 0) {
        return -1;
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (loop->intents[i].in_use && loop->intents[i].request_id == request_id) {
            return -1;
        }
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (!loop->intents[i].in_use) {
            loop->intents[i].in_use = 1;
            loop->intents[i].request_id = request_id;
            loop->intents[i].on_resolve = on_resolve;
            loop->intents[i].user = user;
            if (wasmos_ipc_send(destination_endpoint, source_endpoint, type, request_id, arg0, arg1,
                                arg2, arg3) != 0) {
                loop->intents[i].in_use = 0;
                loop->intents[i].request_id = 0;
                loop->intents[i].on_resolve = 0;
                loop->intents[i].user = 0;
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static inline void wasmos_sys_intent_cancel(wasmos_sys_event_loop_t* loop, int32_t request_id) {
    if (!loop || request_id <= 0) {
        return;
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (loop->intents[i].in_use && loop->intents[i].request_id == request_id) {
            loop->intents[i].in_use = 0;
            loop->intents[i].request_id = 0;
            loop->intents[i].on_resolve = 0;
            loop->intents[i].user = 0;
            return;
        }
    }
}

void wasmos_sys_wasm_ipc_future_init(wasmos_sys_wasm_ipc_future_t* operation,
                                     wasmos_sys_wasm_ipc_future_reply_status_fn reply_status,
                                     void* user);
wasmos_future_t* wasmos_sys_wasm_ipc_future_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_ipc_future_t* operation,
                                                 int32_t destination_endpoint,
                                                 int32_t source_endpoint, int32_t msg_type,
                                                 int32_t arg0, int32_t arg1, int32_t arg2,
                                                 int32_t arg3, int32_t* out_request_id);
void wasmos_sys_wasm_ipc_future_cancel(wasmos_sys_wasm_ipc_future_t* operation, int32_t status);
const wasmos_ipc_message_t*
wasmos_sys_wasm_ipc_future_reply(const wasmos_sys_wasm_ipc_future_t* operation);

void wasmos_sys_wasm_fs_request_init(wasmos_sys_wasm_fs_request_t* request);
wasmos_future_t* wasmos_sys_wasm_fs_request_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_fs_request_t* request,
                                                 int32_t fs_endpoint, int32_t reply_endpoint,
                                                 int32_t msg_type, int32_t arg0, int32_t arg1,
                                                 int32_t arg2, int32_t arg3,
                                                 int32_t* out_request_id);
const wasmos_ipc_message_t*
wasmos_sys_wasm_fs_request_reply(const wasmos_sys_wasm_fs_request_t* request);

void wasmos_sys_wasm_fs_operation_init(wasmos_sys_wasm_fs_operation_t* operation);
wasmos_future_t* wasmos_sys_wasm_fs_open_async(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               const char* path, int32_t flags,
                                               int32_t* out_request_id);
wasmos_future_t* wasmos_sys_wasm_fs_read_async(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               int32_t fd, void* dst, int32_t len,
                                               int32_t* out_request_id);
wasmos_future_t* wasmos_sys_wasm_fs_write_async(wasmos_sys_event_loop_t* loop,
                                                wasmos_sys_wasm_fs_operation_t* operation,
                                                int32_t fs_endpoint, int32_t reply_endpoint,
                                                int32_t fd, const void* src, int32_t len,
                                                int32_t* out_request_id);
wasmos_future_t* wasmos_sys_wasm_fs_close_async(wasmos_sys_event_loop_t* loop,
                                                wasmos_sys_wasm_fs_operation_t* operation,
                                                int32_t fs_endpoint, int32_t reply_endpoint,
                                                int32_t fd, int32_t* out_request_id);
wasmos_future_t* wasmos_sys_wasm_fs_unlink_async(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_fs_operation_t* operation,
                                                 int32_t fs_endpoint, int32_t reply_endpoint,
                                                 const char* path, int32_t* out_request_id);
wasmos_future_t* wasmos_sys_wasm_fs_stat_async(wasmos_sys_event_loop_t* loop,
                                               wasmos_sys_wasm_fs_operation_t* operation,
                                               int32_t fs_endpoint, int32_t reply_endpoint,
                                               const char* path, int32_t* out_request_id);
/* Copies a completed read payload, releases an owned buffer, and returns the
 * response status/arg0.  finish() is idempotent for buffer release. */
int32_t wasmos_sys_wasm_fs_operation_finish(wasmos_sys_wasm_fs_operation_t* operation,
                                            void* read_dst, int32_t read_capacity,
                                            wasmos_ipc_message_t* out_reply);

static inline int32_t wasmos_sys_event_loop_poll(wasmos_sys_event_loop_t* loop, int32_t budget) {
    int32_t handled = 0;
    if (!loop) {
        return 0;
    }
    if (budget == 0) {
        budget = 1;
    }
    for (int32_t i = 0; i < budget; ++i) {
        wasmos_ipc_message_t msg;
        if (wasmos_ipc_drain(loop->receiver_endpoint) <= 0) {
            /* No message available.  If this is the first iteration and the
             * loop has a select-set, block until a message arrives instead of
             * returning immediately (Minos2: never busy-poll). */
            if (i == 0 && loop->select_id > 0) {
                (void)wasmos_ipc_select_wait(loop->select_id);
                if (wasmos_ipc_drain(loop->receiver_endpoint) <= 0) {
                    break;
                }
                wasmos_ipc_message_read_last(&msg);
                goto wasmos_sys_event_loop_poll_handle;
            }
            break;
        }
        wasmos_ipc_message_read_last(&msg);
    wasmos_sys_event_loop_poll_handle:
        handled++;
        for (int32_t j = 0; j < WASMOS_SYS_INTENT_MAX; ++j) {
            if (loop->intents[j].in_use && loop->intents[j].request_id == msg.request_id) {
                void (*cb)(void* user, const wasmos_ipc_message_t* msg) =
                    loop->intents[j].on_resolve;
                void* cb_user = loop->intents[j].user;
                loop->intents[j].in_use = 0;
                loop->intents[j].request_id = 0;
                loop->intents[j].on_resolve = 0;
                loop->intents[j].user = 0;
                cb(cb_user, &msg);
                goto wasmos_sys_event_loop_poll_done_message;
            }
        }
        int32_t dispatched = 0;
        for (int32_t j = 0; j < WASMOS_SYS_HANDLER_MAX; ++j) {
            if (loop->handlers[j].in_use && loop->handlers[j].msg_type == msg.type &&
                loop->handlers[j].on_message) {
                loop->handlers[j].on_message(loop->handlers[j].user, &msg);
                dispatched = 1;
                break;
            }
        }
        if (!dispatched && loop->default_on_message) {
            loop->default_on_message(loop->default_user, &msg);
        }
    wasmos_sys_event_loop_poll_done_message:;
    }
    return handled;
}

static inline void wasmos_sys_ipc_pack_name16(const char* name, int32_t out_args[4]) {
    wasmos_ipc_pack_name16(name, out_args);
}

static inline void wasmos_sys_ipc_unpack_name16(uint32_t arg0, uint32_t arg1, uint32_t arg2,
                                                uint32_t arg3, char* out, uint32_t out_len) {
    uint32_t args[4] = {arg0, arg1, arg2, arg3};
    uint32_t pos = 0;
    if (!out || out_len == 0) {
        return;
    }
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFFu);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = c;
            v >>= 8u;
        }
    }
    out[pos] = '\0';
}

static inline void wasmos_sys_ipc_recv_loop(void) {
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_select_one(endpoint);
        }
    }
}

/* Send PROC_IPC_NOTIFY_READY to the process manager and block until PM acks.
 * The blocking wait keeps the source_endpoint alive long enough for PM to
 * identify the sender, which lets PM reliably unblock any sync-spawn parent
 * and prevents the race where a short-lived process (e.g. pci-bus) destroys
 * its endpoint before PM processes the IPC. */
static inline void wasmos_sys_notify_ready(int32_t proc_endpoint, int32_t source_endpoint) {
    /* Wait for the PM's ack on a DEDICATED endpoint, never on the service
     * endpoint. The wait is load-bearing: a one-shot service (pci-bus, acpi-bus)
     * exits right after this and must stay alive until the PM has marked it
     * ready and completed the parent's sync spawn, so it cannot fire-and-forget.
     * But blocking a request-id-matching receive on the *service* endpoint
     * drains and DROPS any request that races in right after registration
     * (e.g. a driver client's first request), silently breaking its
     * request/response contract. The PM identifies the notifier by the owner
     * context of the message source and marks readiness by process, so any
     * endpoint owned by this process is equivalent for readiness while a private
     * one isolates the ack from real request traffic. */
    static int32_t s_ready_reply_ep = -1;
    wasmos_ipc_message_t reply;
    (void)source_endpoint;
    if (s_ready_reply_ep < 0) {
        s_ready_reply_ep = wasmos_ipc_create_endpoint();
    }
    if (s_ready_reply_ep < 0) {
        return;
    }
    (void)wasmos_ipc_call(proc_endpoint, s_ready_reply_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0,
                          &reply);
}

/* Spawn a module by index and block until the child first blocks on IPC
 * (implicit ready signal) or until timeout_ms milliseconds have elapsed
 * (0 = wait forever).  Returns the child PID on success or a negative error
 * code on failure or timeout. */
static inline int32_t wasmos_sys_spawn_sync(int32_t proc_endpoint, int32_t reply_endpoint,
                                            int32_t module_index, int32_t timeout_ms,
                                            int32_t request_id) {
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(proc_endpoint, reply_endpoint, PROC_IPC_SPAWN_SYNC, request_id,
                        module_index, timeout_ms, 0, 0, &reply) != 0) {
        return -1;
    }
    return reply.type == PROC_IPC_RESP ? (int32_t)reply.arg0 : -1;
}

/* Spawn by path and block until the child first blocks on IPC (implicit ready
 * signal) or until timeout_ms milliseconds have elapsed (0 = wait forever).
 * The caller must write the path bytes to the xfer buffer before calling.
 * Returns the child PID on success or a negative error code on failure or
 * timeout. */
static inline int32_t wasmos_sys_spawn_path_sync(int32_t proc_endpoint, int32_t reply_endpoint,
                                                 int32_t path_len, int32_t timeout_ms,
                                                 int32_t request_id) {
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(proc_endpoint, reply_endpoint, PROC_IPC_SPAWN_PATH_SYNC, request_id, 0,
                        path_len, 0, timeout_ms, &reply) != 0) {
        return -1;
    }
    return reply.type == PROC_IPC_RESP ? (int32_t)reply.arg0 : -1;
}

static inline int32_t wasmos_sys_svc_lookup_retry(int32_t proc_endpoint, int32_t reply_endpoint,
                                                  const char* service_name, int32_t request_id_base,
                                                  int32_t attempts) {
    if (attempts <= 0) {
        attempts = 1;
    }
    for (int32_t i = 0; i < attempts; ++i) {
        int32_t endpoint =
            wasmos_svc_lookup(proc_endpoint, reply_endpoint, service_name, request_id_base + i);
        if (endpoint >= 0) {
            return endpoint;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

static inline int32_t wasmos_sys_ipc_send_retry(int32_t destination_endpoint,
                                                int32_t source_endpoint, int32_t type,
                                                int32_t request_id, int32_t arg0, int32_t arg1,
                                                int32_t arg2, int32_t arg3, int32_t retries) {
    /* Keep in sync with kernel ipc.h */
    const int32_t ipc_err_full = -3;
    int32_t tries = 0;
    if (retries <= 0) {
        retries = 1;
    }
    for (;;) {
        int32_t rc = wasmos_ipc_send(destination_endpoint, source_endpoint, type, request_id, arg0,
                                     arg1, arg2, arg3);
        if (rc == 0 || rc != ipc_err_full) {
            return rc;
        }
        if (++tries >= retries) {
            return ipc_err_full;
        }
        (void)wasmos_sched_yield();
    }
}

/* Grantee-side read of a transfer buffer object named by `buffer_id`. The owner
 * must already have granted this context READ (via borrow/reborrow) before
 * sending buffer_id; the kernel enforces access. No borrow is taken here. */
static inline int32_t wasmos_sys_buffer_read(int32_t buffer_id, void* dst, int32_t len,
                                             int32_t offset) {
    if (!dst || buffer_id <= 0 || len < 0 || offset < 0) {
        return -1;
    }
    return wasmos_xfer_buffer_read(buffer_id, addr_cast(int32_t, dst), len, offset) == 0 ? 0 : -1;
}

/* Grantee-side write of a transfer buffer object named by `buffer_id`. The owner
 * must already have granted this context WRITE before sending buffer_id. */
static inline int32_t wasmos_sys_buffer_write(int32_t buffer_id, const void* src, int32_t len,
                                              int32_t offset) {
    if (!src || buffer_id <= 0 || len < 0 || offset < 0) {
        return -1;
    }
    return wasmos_xfer_buffer_write(buffer_id, addr_cast(int32_t, src), len, offset) == 0 ? 0 : -1;
}

static inline void wasmos_sys_random_finish(wasmos_sys_random_request_t* request, int32_t status) {
    if (!request) {
        return;
    }
    if (request->buffer_id >= 0) {
        (void)wasmos_xfer_buffer_release(request->buffer_id);
        request->buffer_id = -1;
    }
    if (status == 0 && request->float_out) {
        *request->float_out = (float)(request->float_word >> 8u) * (1.0f / 16777216.0f);
    }
    if (request->on_complete) {
        request->on_complete(request->user, status);
    }
}

static inline int32_t wasmos_sys_random_issue(wasmos_sys_random_request_t* request);

static inline void wasmos_sys_random_reply(void* user, const wasmos_ipc_message_t* reply) {
    wasmos_sys_random_request_t* request = (wasmos_sys_random_request_t*)user;
    int32_t wrote;
    if (!request || !reply) {
        return;
    }
    if (reply->type == HRNG_IPC_ERROR) {
        wasmos_sys_random_finish(request, reply->arg0 < 0 ? reply->arg0 : WASMOS_ERR_HRNG_IO_ERROR);
        return;
    }
    if (reply->type != HRNG_IPC_RESP) {
        wasmos_sys_random_finish(request, WASMOS_ERR_HRNG_PROTOCOL);
        return;
    }
    wrote = reply->arg0;
    if (wrote <= 0 || wrote > request->chunk_max || wrote > request->len - request->done ||
        wasmos_xfer_buffer_read(request->buffer_id,
                                addr_cast(int32_t, request->out + request->done), wrote, 0) != 0) {
        wasmos_sys_random_finish(request, WASMOS_ERR_HRNG_IO_ERROR);
        return;
    }
    request->done += wrote;
    if (request->done == request->len) {
        wasmos_sys_random_finish(request, 0);
        return;
    }
    if (wasmos_sys_random_issue(request) != 0) {
        wasmos_sys_random_finish(request, WASMOS_ERR_HRNG_IO_ERROR);
    }
}

static inline int32_t wasmos_sys_random_issue(wasmos_sys_random_request_t* request) {
    int32_t chunk;
    if (!request || !request->loop || request->done >= request->len) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    chunk = request->len - request->done;
    if (chunk > request->chunk_max) {
        chunk = request->chunk_max;
    }
    return wasmos_sys_intent_send(request->loop, request->hrng_endpoint,
                                  request->loop->receiver_endpoint, HRNG_IPC_GET_BYTES_REQ,
                                  request->buffer_id, chunk, 0, 0, wasmos_sys_random_reply, request,
                                  0);
}

/* Start a non-blocking entropy request. Callers retain `request` and `out`
 * until `on_complete` runs, and drive completion through event_loop_poll(). */
static inline int32_t
wasmos_sys_random_bytes_async(wasmos_sys_event_loop_t* loop, int32_t hrng_endpoint, uint8_t* out,
                              int32_t len, wasmos_sys_random_request_t* request,
                              wasmos_sys_random_complete_fn on_complete, void* user) {
    int32_t buffer_size;
    if (!loop || !request || !out || len <= 0 || hrng_endpoint < 0 || loop->receiver_endpoint < 0 ||
        !on_complete) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    request->loop = loop;
    request->hrng_endpoint = hrng_endpoint;
    request->buffer_id = -1;
    request->len = len;
    request->done = 0;
    request->out = out;
    request->float_out = 0;
    request->on_complete = on_complete;
    request->user = user;
    buffer_size = wasmos_xfer_buffer_size();
    request->chunk_max = buffer_size < (int32_t)HRNG_MAX_BYTES_PER_REQ
                             ? buffer_size
                             : (int32_t)HRNG_MAX_BYTES_PER_REQ;
    if (request->chunk_max <= 0) {
        return WASMOS_ERR_HRNG_NOT_READY;
    }
    request->buffer_id = wasmos_xfer_buffer_acquire(request->chunk_max);
    if (request->buffer_id < 0 || wasmos_xfer_buffer_borrow(hrng_endpoint, request->buffer_id,
                                                            WASMOS_BUFFER_GRANT_WRITE) < 0) {
        if (request->buffer_id >= 0) {
            (void)wasmos_xfer_buffer_release(request->buffer_id);
            request->buffer_id = -1;
        }
        return WASMOS_ERR_HRNG_NOT_READY;
    }
    if (wasmos_sys_random_issue(request) != 0) {
        (void)wasmos_xfer_buffer_release(request->buffer_id);
        request->buffer_id = -1;
        return WASMOS_ERR_HRNG_IO_ERROR;
    }
    return 0;
}

static inline int32_t wasmos_sys_random_int_async(wasmos_sys_event_loop_t* loop,
                                                  int32_t hrng_endpoint, uint32_t* out_value,
                                                  wasmos_sys_random_request_t* request,
                                                  wasmos_sys_random_complete_fn on_complete,
                                                  void* user) {
    if (!request) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    request->float_out = 0;
    return wasmos_sys_random_bytes_async(loop, hrng_endpoint, (uint8_t*)out_value,
                                         (int32_t)sizeof(*out_value), request, on_complete, user);
}

static inline int32_t wasmos_sys_random_float_async(wasmos_sys_event_loop_t* loop,
                                                    int32_t hrng_endpoint, float* out_value,
                                                    wasmos_sys_random_request_t* request,
                                                    wasmos_sys_random_complete_fn on_complete,
                                                    void* user) {
    int32_t status;
    if (!request || !out_value) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    status = wasmos_sys_random_bytes_async(loop, hrng_endpoint, (uint8_t*)&request->float_word,
                                           (int32_t)sizeof(request->float_word), request,
                                           on_complete, user);
    if (status != WASMOS_ERR_NONE) {
        return status;
    }
    request->float_out = out_value;
    return 0;
}

/* Read the file at `path` via FS_IPC_READ_PATH_REQ into `out_text`.
 * Owner-push: acquires a per-call buffer, writes the path in, GRANTS the FS
 * endpoint R|W over it (borrow -> b1), and ships buffer_id (arg2) + b1 (arg3).
 * fs-manager reborrows to the backend, which writes the blob straight back into
 * this buffer, then fs-manager unborrows b1 before replying so release()
 * succeeds. Returns bytes read or -1. */
static inline int32_t wasmos_sys_fs_read_path(int32_t fs_endpoint, int32_t reply_endpoint,
                                              int32_t request_id, const char* path, char* out_text,
                                              int32_t out_text_len) {
    wasmos_ipc_message_t resp;
    int32_t path_len = 0;
    int32_t read_len = 0;
    int32_t buf_size = 0;
    int32_t bid = -1;
    int32_t b1 = -1;
    if (!path || !out_text || out_text_len < 2) {
        return -1;
    }
    path_len = (int32_t)strlen(path);
    if (path_len <= 0 || path_len >= wasmos_xfer_buffer_size()) {
        return -1;
    }
    /* The one object holds the path (in) then the blob (out): size for both. */
    buf_size = out_text_len > path_len ? out_text_len : path_len;
    bid = wasmos_xfer_buffer_acquire(buf_size);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    b1 = wasmos_xfer_buffer_borrow(fs_endpoint, bid,
                                   WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_send(fs_endpoint, reply_endpoint, FS_IPC_READ_PATH_REQ, request_id, path_len,
                        buf_size, bid, b1) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_sys_ipc_recv_matching(reply_endpoint, request_id, &resp) != 0 ||
        resp.type != FS_IPC_RESP) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    read_len = resp.arg0;
    if (read_len < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (read_len >= out_text_len) {
        read_len = out_text_len - 1;
    }
    if (read_len > 0 &&
        wasmos_xfer_buffer_read(bid, addr_cast(int32_t, out_text), read_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    out_text[read_len] = '\0';
    return read_len;
}

#ifdef __cplusplus
}
#endif

#endif
