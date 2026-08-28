/* ipc.h - IPC message struct, send/receive helpers, and service lookup wrappers */
#ifndef WASMOS_LIBC_WASMOS_IPC_H
#define WASMOS_LIBC_WASMOS_IPC_H

#include <stdint.h>

#include "wasmos_cast.h"
#include "wasmos/api.h"
#include "string.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one kernel IPC status these helpers act on rather than propagate: a send
 * that found the destination queue full and is worth retrying. Mirrors the
 * kernel's IPC_ERR_FULL; overridable so a component that already defines it
 * keeps a single definition. */
#ifndef WASMOS_IPC_ERR_FULL
#define WASMOS_IPC_ERR_FULL (-3)
#endif

/* Default number of send attempts before a queue-full failure is reported to
 * the caller, one scheduler yield apart. Large because the peer only has to be
 * scheduled once to drain its queue; it bounds a livelock, not a latency. */
#ifndef WASMOS_IPC_SEND_RETRY_LIMIT
#define WASMOS_IPC_SEND_RETRY_LIMIT 4096
#endif
/* Decoded IPC message.  The kernel's IPC_FIELD ordering is:
 * field 0=type, 1=request_id, 2=arg0, 3=arg1, 4=source, 5=destination,
 * 6=arg2, 7=arg3 — arg2/arg3 are NOT fields 4/5.
 * `source` is the endpoint the sender declared as its reply address and is what
 * a server answers on; `destination` is the endpoint the message was delivered
 * to. The four args are opaque payload words whose meaning belongs to `type`;
 * by convention arg2 carries a transfer-buffer id when one is involved. */
typedef struct {
    int32_t type;
    int32_t request_id;
    int32_t arg0;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
    int32_t source;
    int32_t destination;
} wasmos_ipc_message_t;

/* Forward declarations; contracts are on the definitions below. */
static inline int32_t wasmos_ipc_call_retry(int32_t destination_endpoint, int32_t source_endpoint,
                                            int32_t type, int32_t request_id, int32_t arg0,
                                            int32_t arg1, int32_t arg2, int32_t arg3,
                                            wasmos_ipc_message_t* out_reply,
                                            int32_t send_retry_limit);

static inline int32_t wasmos_ipc_call_managed(int32_t server, int32_t type, int32_t arg0,
                                              int32_t arg1, int32_t arg2, int32_t arg3,
                                              wasmos_ipc_message_t* out_reply);

/* Populate message from the last received IPC fields.
 * Must be called immediately after wasmos_ipc_recv/try_recv returns > 0.
 * The last-message slot is per context and is overwritten by the next receive
 * on ANY endpoint, so copy it out before receiving again. A NULL `message` is a
 * no-op. */
static inline void wasmos_ipc_message_read_last(wasmos_ipc_message_t* message) {
    if (!message) {
        return;
    }
    message->type = wasmos_ipc_last_field(0);
    message->request_id = wasmos_ipc_last_field(1);
    message->arg0 = wasmos_ipc_last_field(2);
    message->arg1 = wasmos_ipc_last_field(3);
    message->source = wasmos_ipc_last_field(4);
    message->destination = wasmos_ipc_last_field(5);
    message->arg2 = wasmos_ipc_last_field(6);
    message->arg3 = wasmos_ipc_last_field(7);
}

/* Send a two-argument reply to `reply_endpoint` (the requester's source
 * endpoint), echoing `request_id` so the requester can correlate it; arg2/arg3
 * are sent as 0. Returns wasmos_ipc_send's status: 0 on success, negative
 * otherwise. Does not retry on a full queue — use wasmos_ipc_send_retry when the
 * reply must not be dropped. */
static inline int32_t wasmos_ipc_reply(int32_t reply_endpoint, int32_t source_endpoint,
                                       int32_t type, int32_t request_id, int32_t arg0,
                                       int32_t arg1) {
    return wasmos_ipc_send(reply_endpoint, source_endpoint, type, request_id, arg0, arg1, 0, 0);
}

/* Send with automatic retry on IPC_ERR_FULL (-3) up to retry_limit times,
 * yielding between each attempt.  Use 0 for the default limit.
 * Returns 0 once the message is queued, or the last failing status: any error
 * other than queue-full is returned immediately without a retry, and an
 * exhausted retry budget returns WASMOS_IPC_ERR_FULL. Blocks only in the sense
 * that it yields the CPU between attempts; it never parks the caller. */
static inline int32_t wasmos_ipc_send_retry(int32_t destination_endpoint, int32_t source_endpoint,
                                            int32_t type, int32_t request_id, int32_t arg0,
                                            int32_t arg1, int32_t arg2, int32_t arg3,
                                            int32_t retry_limit) {
    int32_t tries = 0;
    int32_t rc;
    if (retry_limit <= 0) {
        retry_limit = WASMOS_IPC_SEND_RETRY_LIMIT;
    }
    for (;;) {
        rc = wasmos_ipc_send(
            destination_endpoint, source_endpoint, type, request_id, arg0, arg1, arg2, arg3);
        if (rc == 0) {
            return 0;
        }
        if (rc != WASMOS_IPC_ERR_FULL || ++tries >= retry_limit) {
            return rc;
        }
        (void)wasmos_sched_yield();
    }
}

/* wasmos_ipc_call_retry with the default send-retry budget; see there for the
 * matching rules and return values. */
static inline int32_t wasmos_ipc_call(int32_t destination_endpoint, int32_t source_endpoint,
                                      int32_t type, int32_t request_id, int32_t arg0, int32_t arg1,
                                      int32_t arg2, int32_t arg3, wasmos_ipc_message_t* out_reply) {
    return wasmos_ipc_call_retry(destination_endpoint,
                                 source_endpoint,
                                 type,
                                 request_id,
                                 arg0,
                                 arg1,
                                 arg2,
                                 arg3,
                                 out_reply,
                                 WASMOS_IPC_SEND_RETRY_LIMIT);
}

/* Synchronous request/reply: send the request from `source_endpoint` to
 * `destination_endpoint` (retrying a full queue up to `send_retry_limit`
 * attempts), then BLOCK on `source_endpoint` until the matching reply arrives,
 * copying it into *out_reply (may be NULL to discard it).
 *
 * `request_id` must be unique among the caller's in-flight requests on that
 * endpoint. Only a message carrying this request_id AND sent from
 * `destination_endpoint` is accepted; every other message that lands on
 * `source_endpoint` meanwhile is consumed and DISCARDED, so this must be called
 * on a private reply endpoint, never on a live service endpoint.
 *
 * Returns 0 on a matched reply, the send status if the request could not be
 * queued, or the negative receive status. There is no timeout: a peer that never
 * answers blocks the caller indefinitely. */
static inline int32_t wasmos_ipc_call_retry(int32_t destination_endpoint, int32_t source_endpoint,
                                            int32_t type, int32_t request_id, int32_t arg0,
                                            int32_t arg1, int32_t arg2, int32_t arg3,
                                            wasmos_ipc_message_t* out_reply,
                                            int32_t send_retry_limit) {
    int32_t rc = wasmos_ipc_send_retry(destination_endpoint,
                                       source_endpoint,
                                       type,
                                       request_id,
                                       arg0,
                                       arg1,
                                       arg2,
                                       arg3,
                                       send_retry_limit);
    if (rc != 0) {
        return rc;
    }
    for (;;) {
        int32_t response_request_id;
        int32_t response_source;
        rc = wasmos_ipc_select_one(source_endpoint);
        if (rc < 0) {
            return rc;
        }
        /* Match on the last-field hostcalls before building a message struct:
         * only a reply carrying this request_id AND coming from the endpoint
         * the request went to is accepted; anything else that lands on the
         * reply endpoint is consumed and discarded, so this call belongs on a
         * private reply endpoint and never on a live service endpoint. */
        response_request_id = wasmos_ipc_last_field(1);
        if (response_request_id != request_id) {
            continue;
        }
        response_source = wasmos_ipc_last_field(4);
        if (response_source != destination_endpoint) {
            continue;
        }
        break;
    }
    if (out_reply) {
        wasmos_ipc_message_read_last(out_reply);
    }
    return 0;
}

/* Pack up to 16 chars of a service name into four int32 IPC args (4 bytes each,
 * little-endian).  Used by wasmos_svc_register/lookup.
 * Unused byte positions stay zero, so a name shorter than 16 chars is
 * zero-padded and a name of exactly 16 travels without a terminator; anything
 * longer is silently truncated to its first 16 bytes. A NULL `name` yields four
 * zero words, and a NULL `out_args` is a no-op. */
static inline void wasmos_ipc_pack_name16(const char* name, int32_t out_args[4]) {
    if (!out_args) {
        return;
    }
    out_args[0] = 0;
    out_args[1] = 0;
    out_args[2] = 0;
    out_args[3] = 0;
    if (!name) {
        return;
    }
    for (int32_t i = 0; name[i] && i < 16; ++i) {
        int32_t slot = i / 4;
        int32_t shift = (i % 4) * 8;
        out_args[slot] |= ((int32_t)(uint8_t)name[i]) << shift;
    }
}

/* Acquire a transfer buffer sized for `len` bytes and copy `len` bytes from
 * `src` into it at offset 0. Returns the owned buffer_id (>= 0) or -1. The
 * caller sends the id to the kernel (arg2 by convention) and releases it with
 * wasmos_xfer_buffer_release once the synchronous request completes. */
static inline int32_t wasmos_xfer_stage(const void* src, int32_t len) {
    int32_t bid = wasmos_xfer_buffer_acquire(len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, src, len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    return bid;
}

/* Register service_endpoint under service_name with the process manager,
 * declaring `flags` (WASMOS_SVC_FLAG_*; unknown bits are dropped). Returns the
 * assigned service handle on success, -1 on failure.
 *
 * The request payload is a svc_register_desc_t placed in the per-context xfer
 * buffer; the reply is awaited on a dedicated reply endpoint distinct from
 * service_endpoint.  This keeps the SVC_IPC_REGISTER_RESP off the (live) service
 * endpoint, where peer traffic would otherwise race with — and be discarded by —
 * wasmos_ipc_call's reply matcher.  The reply endpoint is created once per
 * translation unit and reused across registrations (no per-call leak), and is
 * private to this helper rather than the managed endpoint of ipc_managed.c. */
static inline int32_t wasmos_svc_register_class_flags(int32_t proc_endpoint,
                                                      int32_t service_endpoint,
                                                      const char* service_name,
                                                      const char* class_name, uint32_t instance,
                                                      uint32_t flags, int32_t request_id) {
    static int32_t s_reg_reply_ep = -1;
    svc_register_desc_t desc;
    wasmos_ipc_message_t resp;
    uint32_t i;
    uint8_t* raw = (uint8_t*)&desc;
    if (s_reg_reply_ep < 0) {
        s_reg_reply_ep = wasmos_ipc_create_endpoint();
    }
    int32_t reply_ep = s_reg_reply_ep;
    if (reply_ep < 0) {
        return -1;
    }
    for (i = 0; i < sizeof(desc); ++i) {
        raw[i] = 0; /* empty class_name / zero instance by default */
    }
    desc.version = WASMOS_SVC_REGISTER_DESC_VERSION;
    desc.service_endpoint = (uint32_t)service_endpoint;
    desc.flags = flags & WASMOS_SVC_FLAG_MASK;
    for (i = 0; i + 1u < WASMOS_SVC_NAME_MAX && service_name[i] != '\0'; ++i) {
        desc.name[i] = service_name[i];
    }
    desc.name[i] = '\0';
    desc.instance = instance;
    if (class_name != 0) {
        for (i = 0; i + 1u < WASMOS_SVC_CLASS_MAX && class_name[i] != '\0'; ++i) {
            desc.class_name[i] = class_name[i];
        }
        desc.class_name[i] = '\0';
    }
    int32_t bid = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_ipc_call(proc_endpoint,
                        reply_ep,
                        SVC_IPC_REGISTER_DESC_REQ,
                        request_id,
                        0,
                        (int32_t)sizeof(desc),
                        bid,
                        0,
                        &resp) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    return (resp.type == SVC_IPC_REGISTER_RESP) ? resp.arg0 : -1;
}

/* Register without declaring any WASMOS_SVC_FLAG_*, which is what a service that
 * holds nothing needing to reach a device before power is cut wants. */
static inline int32_t wasmos_svc_register_class(int32_t proc_endpoint, int32_t service_endpoint,
                                                const char* service_name, const char* class_name,
                                                uint32_t instance, int32_t request_id) {
    return wasmos_svc_register_class_flags(
        proc_endpoint, service_endpoint, service_name, class_name, instance, 0u, request_id);
}

/* Register a service by name only (no virtual class). */
static inline int32_t wasmos_svc_register(int32_t proc_endpoint, int32_t service_endpoint,
                                          const char* service_name, int32_t request_id) {
    return wasmos_svc_register_class(
        proc_endpoint, service_endpoint, service_name, 0, 0, request_id);
}

/* Look up a service by name; returns its endpoint or -1 if not registered.
 * Does not retry — caller must loop with yield for services not yet ready. */
static inline int32_t wasmos_svc_lookup(int32_t proc_endpoint, int32_t reply_endpoint,
                                        const char* service_name, int32_t request_id) {
    int32_t args[4];
    wasmos_ipc_message_t resp;
    uint32_t endpoint_raw;
    wasmos_ipc_pack_name16(service_name, args);
    if (wasmos_ipc_call(proc_endpoint,
                        reply_endpoint,
                        SVC_IPC_LOOKUP_REQ,
                        request_id,
                        args[0],
                        args[1],
                        args[2],
                        args[3],
                        &resp) != 0) {
        return -1;
    }
    if (resp.type != SVC_IPC_LOOKUP_RESP) {
        return -1;
    }
    endpoint_raw = (uint32_t)resp.arg0;
    if (endpoint_raw == 0xFFFFFFFFu) {
        return -1;
    }
    return (int32_t)endpoint_raw;
}

/* Enumerate providers of a virtual class into out[0..max_entries). Returns the
 * total match count (may exceed max_entries; only min(count,max_entries) entries
 * are written), or -1 on error. */
static inline int32_t wasmos_svc_lookup_class(int32_t proc_endpoint, int32_t reply_endpoint,
                                              const char* class_name, svc_class_entry_t* out,
                                              int32_t max_entries, int32_t request_id) {
    wasmos_ipc_message_t resp;
    char cn[WASMOS_SVC_CLASS_MAX];
    uint32_t i;
    int32_t sz;
    int32_t bid;
    int32_t count;
    int32_t got;
    if (max_entries < 0) {
        max_entries = 0;
    }
    for (i = 0; i + 1u < WASMOS_SVC_CLASS_MAX && class_name[i] != '\0'; ++i) {
        cn[i] = class_name[i];
    }
    cn[i] = '\0';
    sz = max_entries * (int32_t)sizeof(svc_class_entry_t);
    if (sz < (int32_t)sizeof(cn)) {
        sz = (int32_t)sizeof(cn); /* room for the class name on input */
    }
    bid = wasmos_xfer_buffer_acquire(sz);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, cn, (int32_t)i + 1, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_call(proc_endpoint,
                        reply_endpoint,
                        SVC_IPC_LOOKUP_CLASS_REQ,
                        request_id,
                        bid,
                        max_entries,
                        0,
                        0,
                        &resp) != 0 ||
        resp.type != SVC_IPC_LOOKUP_CLASS_RESP) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    count = (int32_t)resp.arg0;
    got = (count < max_entries) ? count : max_entries;
    if (out != 0 && got > 0) {
        (void)wasmos_xfer_buffer_read(bid, out, got * (int32_t)sizeof(svc_class_entry_t), 0);
    }
    (void)wasmos_xfer_buffer_release(bid);
    return count;
}

/* Subscribe notify_endpoint to existence events (SVC_IPC_CLASS_EVENT) for a
 * class. Returns 0 on success, -1 on error. */
static inline int32_t wasmos_svc_subscribe_class(int32_t proc_endpoint, int32_t reply_endpoint,
                                                 int32_t notify_endpoint, const char* class_name,
                                                 int32_t request_id) {
    wasmos_ipc_message_t resp;
    char cn[WASMOS_SVC_CLASS_MAX];
    uint32_t i;
    int32_t bid;
    for (i = 0; i + 1u < WASMOS_SVC_CLASS_MAX && class_name[i] != '\0'; ++i) {
        cn[i] = class_name[i];
    }
    cn[i] = '\0';
    bid = wasmos_xfer_stage(cn, (int32_t)i + 1);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_ipc_call(proc_endpoint,
                        reply_endpoint,
                        SVC_IPC_SUBSCRIBE_CLASS_REQ,
                        request_id,
                        notify_endpoint,
                        bid,
                        0,
                        0,
                        &resp) != 0 ||
        resp.type != SVC_IPC_SUBSCRIBE_CLASS_RESP) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    return 0;
}

/* Register a subsystem broker with PM. The descriptor travels in an owned xfer
 * buffer, released before returning. Returns 0 on a PROC_IPC_RESP, the reply's
 * arg1 status when PM answers with anything else, and -1 when the request could
 * not be staged or sent. `request_id` is ignored: the managed call assigns its
 * own id on the per-context reply endpoint. */
static inline int32_t wasmos_subsystem_register_broker(
    int32_t proc_endpoint, int32_t broker_endpoint, const char* request_tag,
    const char* runtime_tag, const char* broker_name, int32_t uses_wasm_payload,
    int32_t needs_runtime_lock, int32_t gates_ready_for_services, int32_t request_id) {
    wasmos_subsystem_broker_register_desc_t desc;
    wasmos_ipc_message_t resp;
    uint32_t i = 0u;

    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_SUBSYSTEM_REGISTER_BROKER_DESC_VERSION;
    desc.broker_endpoint = (uint32_t)broker_endpoint;
    desc.uses_wasm_payload = uses_wasm_payload ? 1u : 0u;
    desc.needs_runtime_lock = needs_runtime_lock ? 1u : 0u;
    desc.gates_ready_for_services = gates_ready_for_services ? 1u : 0u;
    if (request_tag) {
        for (i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && request_tag[i] != '\0'; ++i) {
            desc.request_tag[i] = request_tag[i];
        }
        desc.request_tag[i] = '\0';
    }
    if (runtime_tag) {
        for (i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && runtime_tag[i] != '\0'; ++i) {
            desc.runtime_tag[i] = runtime_tag[i];
        }
        desc.runtime_tag[i] = '\0';
    }
    if (broker_name) {
        for (i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && broker_name[i] != '\0'; ++i) {
            desc.broker_name[i] = broker_name[i];
        }
        desc.broker_name[i] = '\0';
    }
    int32_t bid = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_ipc_call_managed(proc_endpoint,
                                PROC_IPC_SUBSYSTEM_REGISTER_BROKER,
                                0,
                                (int32_t)sizeof(desc),
                                bid,
                                0,
                                &resp) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    (void)request_id;
    return (resp.type == PROC_IPC_RESP) ? 0 : resp.arg1;
}

/* Register an exec handler with PM: the descriptor followed by `node_count`
 * match nodes in one owned xfer buffer, released before returning. Returns 0 on
 * a PROC_IPC_RESP, the reply's arg1 status when PM answers with anything else,
 * and -1 on bad arguments or a staging/send failure. `request_id` is ignored:
 * the managed call assigns its own id on the per-context reply endpoint. */
static inline int32_t wasmos_exec_handler_register(int32_t proc_endpoint, const char* request_tag,
                                                   const char* handler_name, int32_t priority,
                                                   int32_t max_probe_bytes,
                                                   const wasmos_exec_match_node_t* nodes,
                                                   int32_t node_count, int32_t root_index,
                                                   int32_t request_id) {
    wasmos_exec_handler_register_desc_t desc;
    wasmos_ipc_message_t resp;
    int32_t total_size = 0;
    uint32_t i = 0u;

    if (!nodes || node_count <= 0) {
        return -1;
    }
    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_EXEC_HANDLER_REGISTER_DESC_VERSION;
    desc.priority = (uint32_t)priority;
    desc.max_probe_bytes = (uint32_t)max_probe_bytes;
    desc.node_count = (uint32_t)node_count;
    desc.root_index = (uint32_t)root_index;
    if (request_tag) {
        for (i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && request_tag[i] != '\0'; ++i) {
            desc.request_tag[i] = request_tag[i];
        }
        desc.request_tag[i] = '\0';
    }
    if (handler_name) {
        for (i = 0; i < WASMOS_EXEC_HANDLER_NAME_LEN && handler_name[i] != '\0'; ++i) {
            desc.handler_name[i] = handler_name[i];
        }
        desc.handler_name[i] = '\0';
    }
    total_size =
        (int32_t)sizeof(desc) + (int32_t)(sizeof(wasmos_exec_match_node_t) * (uint32_t)node_count);
    int32_t bid = wasmos_xfer_buffer_acquire(total_size);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, &desc, (int32_t)sizeof(desc), 0) != 0 ||
        wasmos_xfer_buffer_write(bid,
                                 nodes,
                                 (int32_t)(sizeof(wasmos_exec_match_node_t) * (uint32_t)node_count),
                                 (int32_t)sizeof(desc)) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_call_managed(
            proc_endpoint, PROC_IPC_EXEC_HANDLER_REGISTER, 0, total_size, bid, 0, &resp) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    (void)request_id;
    return (resp.type == PROC_IPC_RESP) ? 0 : resp.arg1;
}

/*
 * Managed reply endpoint — state is a per-context static in ipc_managed.c (part
 * of the always-linked libc core) so it is never shared across WASM contexts
 * even when multiple contexts belong to the same OS process (each context has
 * independent linear memory).
 */
/* Create the context's managed reply endpoint on first use and return it
 * thereafter; returns the endpoint id, or a negative value if the endpoint
 * could not be created (the failure is not cached and the next call retries). */
int32_t wasmos_ipc_ensure_reply_endpoint(void);
/* Hand out the next request id for the managed endpoint. Ids increase from 1
 * and wrap back to 1 on signed overflow, so they are unique only among the
 * requests actually in flight, which is all the reply matcher needs. */
int32_t wasmos_ipc_next_request_id(void);

/* wasmos_ipc_call_retry on the context's managed reply endpoint with an
 * automatically assigned request id — the common case for a client that has no
 * reply endpoint of its own. Returns 0 on a matched reply, -1 if the managed
 * endpoint could not be created, otherwise the underlying call's status.
 * Blocks until the reply arrives, and discards unrelated traffic that lands on
 * the managed endpoint while waiting. */
static inline int32_t wasmos_ipc_call_managed(int32_t server, int32_t type, int32_t arg0,
                                              int32_t arg1, int32_t arg2, int32_t arg3,
                                              wasmos_ipc_message_t* out_reply) {
    int32_t reply_ep = wasmos_ipc_ensure_reply_endpoint();
    if (reply_ep < 0) {
        return -1;
    }
    return wasmos_ipc_call_retry(server,
                                 reply_ep,
                                 type,
                                 wasmos_ipc_next_request_id(),
                                 arg0,
                                 arg1,
                                 arg2,
                                 arg3,
                                 out_reply,
                                 WASMOS_IPC_SEND_RETRY_LIMIT);
}

/* wasmos_ipc_reply with all four payload arguments; a plain wasmos_ipc_send
 * named for the reply direction. Returns 0 on success, negative otherwise, and
 * does not retry a full queue. */
static inline int32_t wasmos_ipc_reply_full(int32_t destination, int32_t source, int32_t type,
                                            int32_t request_id, int32_t arg0, int32_t arg1,
                                            int32_t arg2, int32_t arg3) {
    return wasmos_ipc_send(destination, source, type, request_id, arg0, arg1, arg2, arg3);
}

#ifdef __cplusplus
}
#endif

#endif
