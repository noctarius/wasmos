#ifndef WASMOS_LIBSYS_H
#define WASMOS_LIBSYS_H

#include <stdint.h>
/* printf is DECLARED here rather than pulled in from "stdio.h": the host unit
 * tests compile this header with the platform SDK on the include path, where
 * that name resolves to the system stdio and collides with the project's own
 * FILE (clang-diagnostic-error, caught by the lint gate). The signature matches
 * both declarations, so the wasm build still binds to libc's. */
int printf(const char* format, ...);

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

/* Fixed capacities of an event loop's two tables, both embedded by value in
 * wasmos_sys_event_loop_t so a guest needs no allocator. INTENT_MAX bounds the
 * replies that may be outstanding at once (intent_send fails once full);
 * HANDLER_MAX bounds the distinct message types that can carry a handler. */
#define WASMOS_SYS_INTENT_MAX 16
#define WASMOS_SYS_HANDLER_MAX 16

/* Random-helper statuses are the packed hrng domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_HRNG_*. */

/* One outstanding request awaiting the reply that carries `request_id`. The
 * slot is freed and cleared before on_resolve runs, so the callback may issue a
 * new intent through the same table. */
typedef struct {
    int32_t in_use;
    int32_t request_id;
    void (*on_resolve)(void* user, const wasmos_ipc_message_t* msg);
    void* user;
} wasmos_sys_intent_t;

/* Dispatch entry for unsolicited messages of one `msg_type`; unlike an intent
 * it stays registered across deliveries. */
typedef struct {
    int32_t in_use;
    int32_t msg_type;
    void (*on_message)(void* user, const wasmos_ipc_message_t* msg);
    void* user;
} wasmos_sys_handler_t;

/* Caller-owned reactor state for one receive endpoint. A delivered message is
 * matched against the intent table by request_id first, then against the
 * handler table by type, and otherwise goes to the default handler.
 * next_request_id is a per-loop counter handed out by intent_send().
 *
 * poll_timeout_ms and on_timeout are how a reactor gets a clock. Without them a
 * poll with nothing queued parks until a message arrives, so a loop can express
 * "wake me when something happens" but not "wake me anyway in 250 ms" -- and a
 * caller needing a deadline had no choice but to drive its own pump. Set
 * poll_timeout_ms to a positive value and poll() bounds its park and calls
 * on_timeout when the window elapses with nothing delivered. Zero (the default)
 * keeps the original park-until-a-message behaviour.
 *
 * on_timeout runs on the poller's stack, like the message handlers, so it must
 * not block; the useful thing for it to do is settle a promise. Both fields are
 * appended after the tables so a binding that mirrors this layout only grows at
 * the end. */
typedef struct {
    int32_t receiver_endpoint;
    int32_t select_id; /* select-set watching receiver_endpoint; -1 if not created */
    int32_t next_request_id;
    void (*default_on_message)(void* user, const wasmos_ipc_message_t* msg);
    void* default_user;
    wasmos_sys_intent_t intents[WASMOS_SYS_INTENT_MAX];
    wasmos_sys_handler_t handlers[WASMOS_SYS_HANDLER_MAX];
    int32_t poll_timeout_ms; /* 0 = park until a message arrives */
    void (*on_timeout)(void* user);
    void* timeout_user;
} wasmos_sys_event_loop_t;

/* Caller-owned bridge between one non-blocking IPC intent and a local future.
 * The reply is copied into the record before settlement, so the pointer the
 * future resolves with stays valid for the record's lifetime. reply_status
 * returns zero to resolve; any other value rejects, and a non-negative one is
 * normalised to -1 because a future rejects only with a negative status.
 * Cancellation only stops local reply tracking; transport work may still
 * complete, and a late reply then falls through to the loop's type handler or
 * default handler like any unsolicited message. */
typedef int32_t (*wasmos_sys_wasm_ipc_future_reply_status_fn)(void* user,
                                                              const wasmos_ipc_message_t* reply);

/* `active` is set between a successful send and the reply (or a cancel);
 * `reply` holds the last delivered message and is the value the future
 * resolves with. */
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
/* `length` is the transfer-buffer size the operation was started with, and
 * caps how much finish() will copy back; has_buffer is cleared once the buffer
 * is released. */
typedef struct {
    wasmos_sys_wasm_fs_request_t request;
    int32_t buffer_id;
    int32_t buffer_borrow;
    int32_t length;
    uint8_t has_buffer;
} wasmos_sys_wasm_fs_operation_t;

typedef struct wasmos_sys_random_request wasmos_sys_random_request_t;
/* Runs once per entropy request, with the packed hrng status (0 on success).
 * The request record and its destination buffer are the caller's again once it
 * returns. */
typedef void (*wasmos_sys_random_complete_fn)(void* user, int32_t status);

/* Caller-owned state of one entropy request. The hrng service caps a single
 * reply at HRNG_MAX_BYTES_PER_REQ and the transfer buffer caps it further, so
 * `len` bytes are gathered as a chain of `chunk_max`-sized intents, with `done`
 * counting the bytes already copied to `out`. float_out is set only by
 * random_float_async(), which draws into float_word and converts on completion.
 * Fields are runtime state: initialise via one of the *_async starters, not by
 * hand. */
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

/*
 * The three outcomes of a timed select wait, which its int32_t return conflates.
 *
 * Ready and timeout are both ordinary: the caller loops and re-polls either way.
 * A FAILURE is not, and is the reason this exists -- a failed wait returns
 * IMMEDIATELY, so a loop that cannot tell it from a timeout stops parking and
 * spins at full speed. Callers must not discard the return.
 */
typedef enum {
    WASMOS_SYS_WAIT_READY = 0,   /* an endpoint became ready; its id was returned */
    WASMOS_SYS_WAIT_TIMEOUT = 1, /* the window elapsed with nothing ready */
    WASMOS_SYS_WAIT_FAILED = 2,  /* the wait could not be performed at all */
} wasmos_sys_wait_result_t;

/* Classify the return of wasmos_ipc_select_wait_timeout() (or any wait sharing
 * that convention): a non-negative value is the ready endpoint id, WASMOS_TIMEOUT
 * (-5) is an elapsed window, and every other negative value is a failure. */
static inline wasmos_sys_wait_result_t wasmos_sys_wait_classify(int32_t rc) {
    if (rc >= 0) {
        return WASMOS_SYS_WAIT_READY; /* endpoint 0 is an endpoint, not an error */
    }
    return rc == WASMOS_TIMEOUT ? WASMOS_SYS_WAIT_TIMEOUT : WASMOS_SYS_WAIT_FAILED;
}

/* True while the wait actually blocked. The question a polling loop needs to
 * ask, because the answer decides whether continuing is a park or a spin. */
static inline int wasmos_sys_wait_parked(int32_t rc) {
    return wasmos_sys_wait_classify(rc) != WASMOS_SYS_WAIT_FAILED;
}

/* Block on `reply_endpoint` until a message carrying `request_id` arrives, copy
 * it to out_reply (optional) and return 0; returns -1 as soon as the underlying
 * blocking receive fails. Messages with any other request_id are consumed and
 * DISCARDED, so this belongs on a private reply endpoint, never on a live
 * service endpoint where it would silently drop incoming requests. It does not
 * check the sender, so a request_id must be unique per reply endpoint. */
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

/* Bind a caller-owned loop to `receiver_endpoint` and clear both tables.
 * `request_id_base` seeds the counter intent_send() draws from, so concurrent
 * loops in one process must use disjoint ranges. Ignores a NULL loop; a
 * negative receiver_endpoint is accepted but leaves the loop with no select-set
 * and nothing to drain. */
static inline void wasmos_sys_event_loop_init(wasmos_sys_event_loop_t* loop,
                                              int32_t receiver_endpoint, int32_t request_id_base) {
    if (!loop) {
        return;
    }
    loop->receiver_endpoint = receiver_endpoint;
    loop->next_request_id = request_id_base;
    loop->default_on_message = 0;
    loop->default_user = 0;
    /* No clock by default: a loop parks until a message arrives unless its
     * owner asks for a bounded wait. */
    loop->poll_timeout_ms = 0;
    loop->on_timeout = 0;
    loop->timeout_user = 0;
    /* Create a select-set watching this loop's endpoint so a poll that finds
     * nothing queued can park on it instead of returning immediately into a
     * caller's spin loop (Minos2 design: tasks block on events, never
     * busy-poll; see docs/architecture/07-scheduling-and-preemption.md).
     * select_id stays -1 when the set cannot be created, and poll() then
     * degrades to a non-blocking drain. */
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

/* Install the fallback handler for messages that match no intent and no typed
 * handler. Returns 0 on success, -1 for a NULL loop or callback. Replaces any
 * previous default. */
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

/* Register (or replace) the handler for `msg_type`. Returns 0 on success, -1
 * for a NULL loop or callback, or when all WASMOS_SYS_HANDLER_MAX slots are
 * taken by other types. Registering the same type again rewrites its callback
 * and user pointer rather than consuming a second slot. */
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

/* Send a request and register `on_resolve` for the reply that carries the
 * allocated request_id, which is reported through out_request_id (optional).
 * Non-blocking: the reply is delivered from wasmos_sys_event_loop_poll().
 * Returns 0 on success, or -1 for a NULL loop/callback, an exhausted intent
 * table, or a failed send - in which case the slot is released and no callback
 * will run. The request_id counter advances even on a failed send. */
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
            if (wasmos_ipc_send(destination_endpoint,
                                source_endpoint,
                                type,
                                request_id,
                                arg0,
                                arg1,
                                arg2,
                                arg3) != 0) {
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

/* intent_send() with a caller-chosen `request_id`, for protocols where the id
 * is derived rather than allocated. It must be positive and must not already be
 * pending on this loop; the loop's own counter is left untouched, so a caller
 * mixing both forms is responsible for keeping the ranges disjoint. Returns 0
 * on success, -1 on a NULL loop/callback, a non-positive or duplicate id, a
 * full intent table, or a failed send. */
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
            if (wasmos_ipc_send(destination_endpoint,
                                source_endpoint,
                                type,
                                request_id,
                                arg0,
                                arg1,
                                arg2,
                                arg3) != 0) {
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

/* Drop local tracking of a pending request so its callback can no longer run.
 * The request itself is not recalled: a reply that arrives afterwards matches
 * no intent and falls through to the type handler or the default handler.
 * Silently does nothing for a NULL loop, a non-positive id, or an id that is
 * not pending. */
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

/* Zero the record and put its future back in the pending state, with
 * `reply_status` (optional; NULL accepts every reply) as the resolve/reject
 * decision. Required before the first send and before every reuse. */
void wasmos_sys_wasm_ipc_future_init(wasmos_sys_wasm_ipc_future_t* operation,
                                     wasmos_sys_wasm_ipc_future_reply_status_fn reply_status,
                                     void* user);
/* Issue the request as a loop intent and return the record's future, or NULL
 * when loop/operation is NULL, the record is already in flight, or its future
 * has already settled (call init() again to reuse it). A failed send returns
 * the future ALREADY REJECTED rather than NULL, so a non-NULL return does not
 * imply the request is on the wire. out_request_id (optional) receives the
 * allocated id, or 0 when nothing was sent. */
wasmos_future_t* wasmos_sys_wasm_ipc_future_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_ipc_future_t* operation,
                                                 int32_t destination_endpoint,
                                                 int32_t source_endpoint, int32_t msg_type,
                                                 int32_t arg0, int32_t arg1, int32_t arg2,
                                                 int32_t arg3, int32_t* out_request_id);
/* Stop tracking the reply and reject the future with `status` (a non-negative
 * status is normalised to -1). Does nothing when the record is not in flight.
 * The transport request is not recalled - see the type comment above. */
void wasmos_sys_wasm_ipc_future_cancel(wasmos_sys_wasm_ipc_future_t* operation, int32_t status);
/* The record's stored reply, valid for the record's lifetime, or NULL for a
 * NULL record. It is zeroed until a reply lands, so read it only after the
 * future has settled READY. */
const wasmos_ipc_message_t*
wasmos_sys_wasm_ipc_future_reply(const wasmos_sys_wasm_ipc_future_t* operation);

/* ipc_future_init() with the FS reply predicate installed: anything other than
 * an FS_IPC_RESP rejects the future with -1. */
void wasmos_sys_wasm_fs_request_init(wasmos_sys_wasm_fs_request_t* request);
/* ipc_future_send() for an FS request. Additionally returns NULL when
 * fs_endpoint or reply_endpoint is negative. */
wasmos_future_t* wasmos_sys_wasm_fs_request_send(wasmos_sys_event_loop_t* loop,
                                                 wasmos_sys_wasm_fs_request_t* request,
                                                 int32_t fs_endpoint, int32_t reply_endpoint,
                                                 int32_t msg_type, int32_t arg0, int32_t arg1,
                                                 int32_t arg2, int32_t arg3,
                                                 int32_t* out_request_id);
/* The FS reply stored in the record, with the same lifetime and read-after-
 * settle rule as wasmos_sys_wasm_ipc_future_reply(). */
const wasmos_ipc_message_t*
wasmos_sys_wasm_fs_request_reply(const wasmos_sys_wasm_fs_request_t* request);

/* Reset a typed FS operation to idle with no transfer buffer held. The *_async
 * starters below re-run this themselves, so an explicit call is only needed to
 * zero a fresh record. */
void wasmos_sys_wasm_fs_operation_init(wasmos_sys_wasm_fs_operation_t* operation);
/* Start one asynchronous filesystem operation on `operation` and return its
 * future, or NULL when the record still holds a buffer or an in-flight request
 * (call finish() first), when the arguments are invalid, or when the transfer
 * buffer could not be acquired, filled, or granted to fs_endpoint. Each starter
 * resets the record itself, acquires a buffer sized for its payload, grants
 * fs_endpoint read+write over it, and ships buffer id and borrow as arg2/arg3;
 * the buffer is held until wasmos_sys_wasm_fs_operation_finish(). The path
 * pointer is borrowed for the duration of the call only (it is copied into the
 * buffer, NUL included); `dst` for a read is ignored here and supplied again to
 * finish(), while `src` for a write is copied up front. Replies are delivered
 * through the loop, so the caller must keep polling it. */
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
/* Close carries no payload, so it is the one starter that acquires no transfer
 * buffer; finish() is still the way to read its status. */
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
 * reply's arg0 (the FS status/byte count), or -1 for a null operation or a
 * failed copy out of the transfer buffer.  Buffer release is idempotent.
 * Call only after the future has settled: the record's reply starts zeroed, so
 * an early call reports 0 instead of a real status. */
int32_t wasmos_sys_wasm_fs_operation_finish(wasmos_sys_wasm_fs_operation_t* operation,
                                            void* read_dst, int32_t read_capacity,
                                            wasmos_ipc_message_t* out_reply);

/* Deliver up to `budget` queued messages (0 is treated as 1) and return how
 * many were dispatched; 0 for a NULL loop. Each message goes to the matching
 * intent, else the handler for its type, else the default handler; a message
 * matching none of those is counted as handled and dropped. If nothing is
 * queued on the first iteration and the loop owns a select-set, this PARKS on
 * that set until the endpoint becomes ready, so a caller that loops on poll()
 * sleeps rather than spins. Without a select-set (init() could not create one)
 * it degrades to a non-blocking drain and returns 0 immediately - a bare
 * `while (1) poll()` then busy-spins. */
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
            /* Nothing queued.  On the first iteration, and only when the loop
             * owns a select-set, park on it rather than returning 0 into a
             * caller that would immediately poll again. */
            if (i == 0 && loop->select_id > 0) {
                /* A bounded park is what lets a caller hold a deadline without
                 * driving its own pump; on_timeout is then the only thing that
                 * runs, and it is what wakes whoever is waiting. */
                if (loop->poll_timeout_ms > 0) {
                    (void)wasmos_ipc_select_wait_timeout(loop->select_id, loop->poll_timeout_ms);
                } else {
                    (void)wasmos_ipc_select_wait(loop->select_id);
                }
                if (wasmos_ipc_drain(loop->receiver_endpoint) <= 0) {
                    if (loop->on_timeout) {
                        loop->on_timeout(loop->timeout_user);
                    }
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

/* Pack up to 16 bytes of a NUL-terminated name into four IPC args (4 bytes
 * each, little-endian); a longer name is truncated and the args are zero-filled
 * first, so a shorter name is NOT NUL-terminated when it exactly fills a slot.
 * Verbatim forwarder to wasmos_ipc_pack_name16() (src/libc/include/wasmos/ipc.h). */
static inline void wasmos_sys_ipc_pack_name16(const char* name, int32_t out_args[4]) {
    wasmos_ipc_pack_name16(name, out_args);
}

/* Inverse of pack_name16(): writes at most out_len-1 bytes plus a NUL, stopping
 * early at the first zero byte. Does nothing for a NULL `out` or out_len 0. */
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

/* Park a process that has reached a terminal state: block forever on a fresh
 * endpoint nobody knows about, discarding whatever arrives. Never returns.
 * FIXME: when endpoint creation fails this degrades into a bare spin loop that
 * burns the CPU instead of parking. */
static inline void wasmos_sys_ipc_recv_loop(void) {
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_select_one(endpoint);
        }
    }
}

/* Send PROC_IPC_NOTIFY_READY to the process manager and block until PM acks.
 *
 * The ack is awaited on a process-private endpoint, created on the first call
 * and cached in a function-static; the `source_endpoint` argument is unused.
 * The PM identifies the notifier by the owner context of the message source and
 * marks readiness per process, so any endpoint owned by this process is
 * equivalent for readiness while a private one isolates the ack from real
 * request traffic.
 *
 * Two constraints make this shape mandatory:
 *  - The wait is load-bearing. A one-shot service (pci-bus, acpi-bus) exits
 *    right after this and must stay alive until PM has marked it ready and
 *    completed the parent's sync spawn, so it cannot fire-and-forget.
 *  - The wait must not run on the *service* endpoint. A request-id-matching
 *    receive there drains and DROPS any request that races in right after
 *    registration (e.g. a driver client's first request), silently breaking
 *    that client's request/response contract. */
static inline void wasmos_sys_notify_ready(int32_t proc_endpoint, int32_t source_endpoint) {
    static int32_t s_ready_reply_ep = -1;
    wasmos_ipc_message_t reply;
    (void)source_endpoint;
    if (s_ready_reply_ep < 0) {
        s_ready_reply_ep = wasmos_ipc_create_endpoint();
    }
    if (s_ready_reply_ep < 0) {
        return;
    }
    (void)wasmos_ipc_call(
        proc_endpoint, s_ready_reply_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, 0, &reply);
}

/* Spawn a module by index and block until the child signals ready or until
 * timeout_ms milliseconds have elapsed (0 = wait forever).  Returns the child
 * PID, or -1 if the call fails or PM answers anything other than
 * PROC_IPC_RESP (it answers PROC_IPC_ERROR on timeout and on a child that
 * died before becoming ready). */
static inline int32_t wasmos_sys_spawn_sync(int32_t proc_endpoint, int32_t reply_endpoint,
                                            int32_t module_index, int32_t timeout_ms,
                                            int32_t request_id) {
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(proc_endpoint,
                        reply_endpoint,
                        PROC_IPC_SPAWN_SYNC,
                        request_id,
                        module_index,
                        timeout_ms,
                        0,
                        0,
                        &reply) != 0) {
        return -1;
    }
    return reply.type == PROC_IPC_RESP ? (int32_t)reply.arg0 : -1;
}

/* Spawn by path and block until the child signals ready or until timeout_ms
 * milliseconds have elapsed (0 = wait forever).  The caller must write the path
 * bytes to the xfer buffer before calling.  Returns the child PID, or -1 if the
 * call fails or PM answers anything other than PROC_IPC_RESP. */
static inline int32_t wasmos_sys_spawn_path_sync(int32_t proc_endpoint, int32_t reply_endpoint,
                                                 int32_t path_len, int32_t timeout_ms,
                                                 int32_t request_id) {
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(proc_endpoint,
                        reply_endpoint,
                        PROC_IPC_SPAWN_PATH_SYNC,
                        request_id,
                        0,
                        path_len,
                        0,
                        timeout_ms,
                        &reply) != 0) {
        return -1;
    }
    return reply.type == PROC_IPC_RESP ? (int32_t)reply.arg0 : -1;
}

/* Resolve `service_name` through the process manager, retrying up to `attempts`
 * times (values <= 0 mean one attempt) and yielding between tries so a service
 * that has not registered yet gets a chance to run. Each attempt consumes one
 * request id from request_id_base upwards. Returns the endpoint id (>= 0) or -1
 * once the attempts are exhausted. Blocks on `reply_endpoint` inside every
 * attempt, which must therefore be a private reply endpoint. */
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

/* Send, retrying up to `retries` times (values <= 0 mean one attempt) with a
 * yield between tries. Returns 0 on success, the send's status unchanged for
 * any failure other than a full destination queue, and IPC_ERR_FULL (-3) once
 * the retries are used up. Does not wait for a reply. */
static inline int32_t wasmos_sys_ipc_send_retry(int32_t destination_endpoint,
                                                int32_t source_endpoint, int32_t type,
                                                int32_t request_id, int32_t arg0, int32_t arg1,
                                                int32_t arg2, int32_t arg3, int32_t retries) {
    /* Mirrors IPC_ERR_FULL / WASMOS_FULL (kernel include/ipc.h, abi/errors.yaml):
     * the only send failure worth retrying, because it means the destination
     * queue is momentarily full rather than misnamed or forbidden. */
    const int32_t ipc_err_full = -3;
    int32_t tries = 0;
    if (retries <= 0) {
        retries = 1;
    }
    for (;;) {
        int32_t rc = wasmos_ipc_send(
            destination_endpoint, source_endpoint, type, request_id, arg0, arg1, arg2, arg3);
        if (rc == 0 || rc != ipc_err_full) {
            return rc;
        }
        if (++tries >= retries) {
            return ipc_err_full;
        }
        (void)wasmos_sched_yield();
    }
}

/* Callback for a message that arrived while awaiting a reply but is not it.
 * Return non-zero if the message was handled (the wait continues silently),
 * zero to have it reported as a discard. `user` is passed through untouched. */
typedef int32_t (*wasmos_sys_ipc_other_fn)(const wasmos_ipc_message_t* message, void* user);

/* Blocking wait for the reply carrying `request_id` on `endpoint`.
 *
 * This exists to make one thing visible. The hand-written form of this loop --
 * receive, compare the request id, `continue` on a mismatch -- CONSUMES the
 * message it rejects. When that message was a reply somebody else was waiting
 * for, the loss is silent and the other side blocks forever; that is how typed
 * characters went missing in the CLI before its VT path became a pump, and it
 * is a live suspect in the intermittent whole-session wedge (docs/TASKS.md).
 * Every discard is therefore reported, with enough identity to say whose reply
 * it was: the endpoint, the id awaited, and the type/id/source of what arrived.
 *
 * Reporting is bounded rather than unconditional: the first
 * WASMOS_IPC_DISCARD_REPORT_MAX discards per process print in full and later
 * ones only every 64th, because the console is a slow serial line and a storm
 * of reports would itself change the timing of the bug being chased.
 *
 * `on_other` gets first refusal on every non-matching message, so a caller that
 * legitimately handles other traffic (a relay forwarding stream chunks, say)
 * stays correct and silent; pass NULL when any other message really is a
 * discard. `who` is the caller's name, printed in the report.
 *
 * Returns 0 with `out` filled, or -1 when the endpoint fails or `retries` empty
 * receives pass without the reply (values <= 0 wait forever). It does not
 * validate the reply's type -- callers check that themselves, since the type
 * that is legal varies per request. */
#ifndef WASMOS_IPC_DISCARD_REPORT_MAX
#define WASMOS_IPC_DISCARD_REPORT_MAX 8
#endif

/* Report one consumed-but-unwanted message.  Split out of the await loop below
 * so a caller that polls with wasmos_ipc_drain instead of blocking -- several do,
 * deliberately, at init before their event pump exists -- can report the same
 * loss without changing its wait into a blocking one. */
static inline void wasmos_sys_ipc_report_discard(const char* who, int32_t endpoint,
                                                 int32_t awaiting,
                                                 const wasmos_ipc_message_t* got) {
    static int32_t discarded_total;
    discarded_total++;
    if (discarded_total <= WASMOS_IPC_DISCARD_REPORT_MAX || (discarded_total % 64) == 0) {
        printf("[ipc-discard] %s ep=%d awaiting=%d dropped type=0x%x req=%d src=%d (total %d)\n",
               who ? who : "?",
               (int)endpoint,
               (int)awaiting,
               (unsigned)(got ? got->type : 0u),
               (int)(got ? got->request_id : 0),
               (int)(got ? got->source : 0),
               (int)discarded_total);
    }
}

static inline int32_t wasmos_sys_ipc_await_reply(int32_t endpoint, int32_t request_id,
                                                 wasmos_ipc_message_t* out,
                                                 wasmos_sys_ipc_other_fn on_other, void* user,
                                                 const char* who, int32_t retries) {
    int32_t empty_polls = 0;
    if (endpoint < 0 || !out) {
        return -1;
    }
    for (;;) {
        int32_t rc = wasmos_ipc_select_one(endpoint);
        if (rc < 0) {
            return -1;
        }
        wasmos_ipc_message_read_last(out);
        if (out->request_id == request_id) {
            return 0;
        }
        if (on_other && on_other(out, user)) {
            continue;
        }
        wasmos_sys_ipc_report_discard(who, endpoint, request_id, out);
        if (retries > 0 && ++empty_polls >= retries) {
            return -1;
        }
    }
}

/* Grantee-side read of a transfer buffer object named by `buffer_id`. The owner
 * must already have granted this context READ (via borrow/reborrow) before
 * sending buffer_id; the kernel enforces access. No borrow is taken here.
 * Copies `len` bytes from `offset` in the object into `dst`. Returns 0 on
 * success, -1 for a NULL dst, a non-positive buffer_id, a negative len/offset,
 * or a hostcall that refused the read. */
static inline int32_t wasmos_sys_buffer_read(int32_t buffer_id, void* dst, int32_t len,
                                             int32_t offset) {
    if (!dst || buffer_id <= 0 || len < 0 || offset < 0) {
        return -1;
    }
    return wasmos_xfer_buffer_read(buffer_id, dst, len, offset) == 0 ? 0 : -1;
}

/* Grantee-side write of a transfer buffer object named by `buffer_id`. The owner
 * must already have granted this context WRITE before sending buffer_id.
 * Copies `len` bytes from `src` to `offset` in the object. Returns 0 on
 * success, -1 for a NULL src, a non-positive buffer_id, a negative len/offset,
 * or a hostcall that refused the write. */
static inline int32_t wasmos_sys_buffer_write(int32_t buffer_id, const void* src, int32_t len,
                                              int32_t offset) {
    if (!src || buffer_id <= 0 || len < 0 || offset < 0) {
        return -1;
    }
    return wasmos_xfer_buffer_write(buffer_id, src, len, offset) == 0 ? 0 : -1;
}

/* Terminate an entropy request with `status`: release the transfer buffer,
 * convert the drawn word for a float request when status is 0, and run the
 * completion callback. Called by the machinery below; a caller only invokes it
 * to abandon a request it started, and must not do so from inside the
 * completion callback. */
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

/* Intent callback for one HRNG_IPC_GET_BYTES_REQ chunk: copies the delivered
 * bytes out of the transfer buffer, then either issues the next chunk or
 * finishes the request. A malformed reply, an over-long byte count, or a failed
 * copy finishes the request with a packed WASMOS_ERR_HRNG_* status. Registered
 * by wasmos_sys_random_issue(); not called directly. */
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
        wasmos_xfer_buffer_read(request->buffer_id, request->out + request->done, wrote, 0) != 0) {
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

/* Send the next chunk of an entropy request as a loop intent, capped at
 * chunk_max. Returns 0 on success, WASMOS_ERR_HRNG_INVALID for a request that
 * has no loop or is already complete, and -1 when the intent could not be sent.
 * Internal to the random helpers. */
static inline int32_t wasmos_sys_random_issue(wasmos_sys_random_request_t* request) {
    int32_t chunk;
    if (!request || !request->loop || request->done >= request->len) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    chunk = request->len - request->done;
    if (chunk > request->chunk_max) {
        chunk = request->chunk_max;
    }
    return wasmos_sys_intent_send(request->loop,
                                  request->hrng_endpoint,
                                  request->loop->receiver_endpoint,
                                  HRNG_IPC_GET_BYTES_REQ,
                                  request->buffer_id,
                                  chunk,
                                  0,
                                  0,
                                  wasmos_sys_random_reply,
                                  request,
                                  0);
}

/* Start a non-blocking entropy request. Callers retain `request` and `out`
 * until `on_complete` runs, and drive completion through event_loop_poll().
 * Gathers `len` bytes into `out` as one or more chunks of at most
 * min(xfer_buffer_size, HRNG_MAX_BYTES_PER_REQ) bytes. Returns 0 once the first
 * chunk is on the wire, after which the outcome is reported only through
 * on_complete; a negative packed WASMOS_ERR_HRNG_* means nothing was started and
 * on_complete will NOT run: INVALID for a rejected argument, NOT_READY when no
 * usable transfer buffer could be acquired or granted, IO_ERROR when the first
 * intent could not be sent. */
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
    if (request->buffer_id < 0 ||
        wasmos_xfer_buffer_borrow(hrng_endpoint, request->buffer_id, WASMOS_BUFFER_GRANT_WRITE) <
            0) {
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

/* random_bytes_async() for a single uint32: draws sizeof(uint32_t) bytes
 * straight into *out_value, whose byte order is whatever the entropy source
 * produced. Same return convention as random_bytes_async(). */
static inline int32_t wasmos_sys_random_int_async(wasmos_sys_event_loop_t* loop,
                                                  int32_t hrng_endpoint, uint32_t* out_value,
                                                  wasmos_sys_random_request_t* request,
                                                  wasmos_sys_random_complete_fn on_complete,
                                                  void* user) {
    if (!request) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    request->float_out = 0;
    return wasmos_sys_random_bytes_async(loop,
                                         hrng_endpoint,
                                         (uint8_t*)out_value,
                                         (int32_t)sizeof(*out_value),
                                         request,
                                         on_complete,
                                         user);
}

/* random_bytes_async() for a float uniformly distributed in [0, 1): the drawn
 * word is buffered in the request and its top 24 bits are converted into
 * *out_value only when the request completes successfully. Same return
 * convention as random_bytes_async(); *out_value is untouched on failure. */
static inline int32_t wasmos_sys_random_float_async(wasmos_sys_event_loop_t* loop,
                                                    int32_t hrng_endpoint, float* out_value,
                                                    wasmos_sys_random_request_t* request,
                                                    wasmos_sys_random_complete_fn on_complete,
                                                    void* user) {
    int32_t status;
    if (!request || !out_value) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    status = wasmos_sys_random_bytes_async(loop,
                                           hrng_endpoint,
                                           (uint8_t*)&request->float_word,
                                           (int32_t)sizeof(request->float_word),
                                           request,
                                           on_complete,
                                           user);
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
 * succeeds. Returns bytes read or -1.
 *
 * Blocking: waits for the reply on `reply_endpoint` via
 * wasmos_sys_ipc_recv_matching(), so that must be a private reply endpoint.
 * `out_text` needs room for at least 2 bytes and is always NUL-terminated on
 * success; a blob longer than out_text_len-1 is silently TRUNCATED and the
 * truncated length is what is returned. Returns -1 for a NULL argument, an
 * empty path, a path that does not fit the transfer buffer, a transport or
 * buffer failure, a reply that is not FS_IPC_RESP, or an FS status below zero -
 * the FS status itself is not propagated. The transfer buffer is released on
 * every exit path. */
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
    if (wasmos_xfer_buffer_write(bid, path, path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    b1 = wasmos_xfer_buffer_borrow(
        fs_endpoint, bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_send(fs_endpoint,
                        reply_endpoint,
                        FS_IPC_READ_PATH_REQ,
                        request_id,
                        path_len,
                        buf_size,
                        bid,
                        b1) != 0) {
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
    if (read_len > 0 && wasmos_xfer_buffer_read(bid, out_text, read_len, 0) != 0) {
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
