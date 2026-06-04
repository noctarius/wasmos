/* ipc.h - IPC message struct, send/receive helpers, and service lookup wrappers */
#ifndef WASMOS_LIBC_WASMOS_IPC_H
#define WASMOS_LIBC_WASMOS_IPC_H

#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WASMOS_IPC_ERR_FULL
#define WASMOS_IPC_ERR_FULL (-3)
#endif

#ifndef WASMOS_IPC_SEND_RETRY_LIMIT
#define WASMOS_IPC_SEND_RETRY_LIMIT 4096
#endif
/* Decoded IPC message.  Note that the kernel's IPC_FIELD ordering is:
 * field 0=type, 1=request_id, 2=arg0, 3=arg1, 4=source, 5=destination,
 * 6=arg2, 7=arg3 — arg2/arg3 are not fields 4/5. */
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

static inline int32_t
wasmos_ipc_call_retry(int32_t destination_endpoint,
                      int32_t source_endpoint,
                      int32_t type,
                      int32_t request_id,
                      int32_t arg0,
                      int32_t arg1,
                      int32_t arg2,
                      int32_t arg3,
                      wasmos_ipc_message_t *out_reply,
                      int32_t send_retry_limit);

/* Populate message from the last received IPC fields.
 * Must be called immediately after wasmos_ipc_recv/try_recv returns > 0. */
static inline void
wasmos_ipc_message_read_last(wasmos_ipc_message_t *message)
{
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

static inline int32_t
wasmos_ipc_reply(int32_t reply_endpoint,
                 int32_t source_endpoint,
                 int32_t type,
                 int32_t request_id,
                 int32_t arg0,
                 int32_t arg1)
{
    return wasmos_ipc_send(reply_endpoint,
                           source_endpoint,
                           type,
                           request_id,
                           arg0,
                           arg1,
                           0,
                           0);
}

/* Send with automatic retry on IPC_ERR_FULL (-3) up to retry_limit times,
 * yielding between each attempt.  Use 0 for the default limit. */
static inline int32_t
wasmos_ipc_send_retry(int32_t destination_endpoint,
                      int32_t source_endpoint,
                      int32_t type,
                      int32_t request_id,
                      int32_t arg0,
                      int32_t arg1,
                      int32_t arg2,
                      int32_t arg3,
                      int32_t retry_limit)
{
    int32_t tries = 0;
    int32_t rc;
    if (retry_limit <= 0) {
        retry_limit = WASMOS_IPC_SEND_RETRY_LIMIT;
    }
    for (;;) {
        rc = wasmos_ipc_send(destination_endpoint,
                             source_endpoint,
                             type,
                             request_id,
                             arg0,
                             arg1,
                             arg2,
                             arg3);
        if (rc == 0) {
            return 0;
        }
        if (rc != WASMOS_IPC_ERR_FULL || ++tries >= retry_limit) {
            return rc;
        }
        (void)wasmos_sched_yield();
    }
}

static inline int32_t
wasmos_ipc_call(int32_t destination_endpoint,
                int32_t source_endpoint,
                int32_t type,
                int32_t request_id,
                int32_t arg0,
                int32_t arg1,
                int32_t arg2,
                int32_t arg3,
                wasmos_ipc_message_t *out_reply)
{
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

static inline int32_t
wasmos_ipc_call_retry(int32_t destination_endpoint,
                      int32_t source_endpoint,
                      int32_t type,
                      int32_t request_id,
                      int32_t arg0,
                      int32_t arg1,
                      int32_t arg2,
                      int32_t arg3,
                      wasmos_ipc_message_t *out_reply,
                      int32_t send_retry_limit)
{
    wasmos_ipc_message_t reply;
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
    rc = wasmos_ipc_recv(source_endpoint);
    if (rc < 0) {
        return rc;
    }
    wasmos_ipc_message_read_last(&reply);
    if (reply.request_id != request_id) {
        return -1;
    }
    if (out_reply) {
        *out_reply = reply;
    }
    return 0;
}

/* Pack up to 16 chars of a service name into four int32 IPC args (4 bytes each,
 * little-endian).  Used by wasmos_svc_register/lookup. */
static inline void
wasmos_ipc_pack_name16(const char *name, int32_t out_args[4])
{
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

/* Register service_endpoint under service_name with the process manager.
 * Returns the assigned service handle on success, -1 on failure. */
static inline int32_t
wasmos_svc_register(int32_t proc_endpoint,
                    int32_t service_endpoint,
                    const char *service_name,
                    int32_t request_id)
{
    int32_t args[4];
    wasmos_ipc_message_t resp;
    wasmos_ipc_pack_name16(service_name, args);
    if (wasmos_ipc_call(proc_endpoint,
                        service_endpoint,
                        SVC_IPC_REGISTER_REQ,
                        request_id,
                        args[0],
                        args[1],
                        args[2],
                        args[3],
                        &resp) != 0) {
        return -1;
    }
    return (resp.type == SVC_IPC_REGISTER_RESP) ? resp.arg0 : -1;
}

/* Look up a service by name; returns its endpoint or -1 if not registered.
 * Does not retry — caller must loop with yield for services not yet ready. */
static inline int32_t
wasmos_svc_lookup(int32_t proc_endpoint,
                  int32_t reply_endpoint,
                  const char *service_name,
                  int32_t request_id)
{
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

/*
 * Managed reply endpoint — state is a per-context static in startup.c so it
 * is never shared across WASM contexts even when multiple contexts belong to
 * the same OS process (each context has independent linear memory).
 */
int32_t wasmos_ipc_ensure_reply_endpoint(void);
int32_t wasmos_ipc_next_request_id(void);

static inline int32_t
wasmos_ipc_call_managed(int32_t server,
                         int32_t type,
                         int32_t arg0,
                         int32_t arg1,
                         int32_t arg2,
                         int32_t arg3,
                         wasmos_ipc_message_t *out_reply)
{
    int32_t reply_ep = wasmos_ipc_ensure_reply_endpoint();
    if (reply_ep < 0) {
        return -1;
    }
    return wasmos_ipc_call_retry(server,
                                  reply_ep,
                                  type,
                                  wasmos_ipc_next_request_id(),
                                  arg0, arg1, arg2, arg3,
                                  out_reply,
                                  WASMOS_IPC_SEND_RETRY_LIMIT);
}

static inline int32_t
wasmos_ipc_reply_full(int32_t destination,
                       int32_t source,
                       int32_t type,
                       int32_t request_id,
                       int32_t arg0,
                       int32_t arg1,
                       int32_t arg2,
                       int32_t arg3)
{
    return wasmos_ipc_send(destination, source, type, request_id,
                            arg0, arg1, arg2, arg3);
}

#ifdef __cplusplus
}
#endif

#endif
