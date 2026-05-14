#ifndef WASMOS_LIBC_WASMOS_IPC_H
#define WASMOS_LIBC_WASMOS_IPC_H

#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos_driver_abi.h"
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
    wasmos_ipc_message_t reply;
    int32_t rc = wasmos_ipc_send(destination_endpoint,
                                 source_endpoint,
                                 type,
                                 request_id,
                                 arg0,
                                 arg1,
                                 arg2,
                                 arg3);
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

#endif
