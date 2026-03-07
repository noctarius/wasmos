#ifndef WASMOS_IPC_H
#define WASMOS_IPC_H

#include <stdint.h>

#define IPC_MAX_ENDPOINTS 32
#define IPC_QUEUE_DEPTH 32

typedef struct {
    uint32_t type;
    uint32_t source;
    uint32_t destination;
    uint32_t request_id;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
} ipc_message_t;

void ipc_init(void);
int ipc_endpoint_create(uint32_t owner_context_id, uint32_t *out_endpoint);
int ipc_endpoint_owner(uint32_t endpoint, uint32_t *out_owner_context_id);
int ipc_send(uint32_t endpoint, const ipc_message_t *message);
int ipc_recv(uint32_t endpoint, ipc_message_t *out_message);

#endif
