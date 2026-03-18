#ifndef WASMOS_IPC_H
#define WASMOS_IPC_H

#include <stdint.h>

#define IPC_MAX_ENDPOINTS 128
#define IPC_QUEUE_DEPTH 32
#define IPC_CONTEXT_KERNEL 0u
#define IPC_ENDPOINT_NONE ((uint32_t)~0u)

typedef enum {
    IPC_OK = 0,
    IPC_EMPTY = 1,
    IPC_ERR_INVALID = -1,
    IPC_ERR_PERM = -2,
    IPC_ERR_FULL = -3
} ipc_result_t;

typedef enum {
    IPC_ENDPOINT_TYPE_MESSAGE = 0,
    IPC_ENDPOINT_TYPE_NOTIFICATION = 1
} ipc_endpoint_type_t;

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
int ipc_notification_create(uint32_t owner_context_id, uint32_t *out_endpoint);
int ipc_endpoint_owner(uint32_t endpoint, uint32_t *out_owner_context_id);
int ipc_endpoint_count(uint32_t endpoint, uint32_t *out_count);
int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t *message);
int ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message);
int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint);
int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint);
int ipc_send(uint32_t endpoint, const ipc_message_t *message);
int ipc_recv(uint32_t endpoint, ipc_message_t *out_message);
int ipc_notify(uint32_t endpoint);
int ipc_wait(uint32_t endpoint);
void ipc_endpoints_release_owner(uint32_t owner_context_id);

#endif
