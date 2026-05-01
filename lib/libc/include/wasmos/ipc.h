#ifndef WASMOS_LIBC_WASMOS_IPC_H
#define WASMOS_LIBC_WASMOS_IPC_H

#include <stdint.h>

#include "wasmos/api.h"
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

#endif
