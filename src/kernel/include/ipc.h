/* ipc.h - Kernel IPC subsystem: fixed-size message passing and notifications.
 *
 * Each endpoint is a numbered FIFO (depth IPC_QUEUE_DEPTH) owned by one context.
 * Senders deliver ipc_message_t values; receivers drain them.  Notifications are
 * lightweight binary signals with no payload — used for event wakeup.
 * The "from/for" variants take explicit sender/receiver context IDs; the plain
 * variants use the current process's context ID as a convenience wrapper. */
#ifndef WASMOS_IPC_H
#define WASMOS_IPC_H

#include <stdint.h>

#define IPC_QUEUE_DEPTH 32              /* max messages queued per endpoint */
#define IPC_ENDPOINT_TABLE_CHUNK 16u    /* endpoints are allocated in this granularity */
#define IPC_CONTEXT_KERNEL 0u           /* reserved context ID for kernel-side senders */
#define IPC_ENDPOINT_NONE ((uint32_t)~0u)

/* Return codes shared by all IPC operations. */
typedef enum {
    IPC_OK = 0,
    IPC_EMPTY = 1,          /* no message available (non-blocking recv) */
    IPC_ERR_INVALID = -1,   /* bad endpoint or context */
    IPC_ERR_PERM = -2,      /* capability check failed */
    IPC_ERR_FULL = -3       /* endpoint queue is full */
} ipc_result_t;

typedef enum {
    IPC_ENDPOINT_TYPE_MESSAGE = 0,      /* carries ipc_message_t payloads */
    IPC_ENDPOINT_TYPE_NOTIFICATION = 1  /* binary signal, no payload */
} ipc_endpoint_type_t;

/* Fixed-size IPC message.  type is application-defined; source/destination are
 * context IDs filled by the kernel.  request_id correlates a reply to a prior send.
 * arg0..arg3 carry call-specific data (file offsets, lengths, handles, etc.). */
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

/* Initialize the IPC subsystem; called once during kernel startup. */
void ipc_init(void);

/* Create a message endpoint owned by owner_context_id; writes its number to *out_endpoint. */
int ipc_endpoint_create(uint32_t owner_context_id, uint32_t *out_endpoint);

/* Create a notification endpoint owned by owner_context_id. */
int ipc_notification_create(uint32_t owner_context_id, uint32_t *out_endpoint);

/* Query the owning context of an endpoint. */
int ipc_endpoint_owner(uint32_t endpoint, uint32_t *out_owner_context_id);

/* Return the number of messages currently queued on endpoint. */
int ipc_endpoint_count(uint32_t endpoint, uint32_t *out_count);

/* Enqueue a message on endpoint as sender_context_id.  Returns IPC_ERR_FULL if the
 * queue is full.  Wakes a blocked receiver if one is waiting. */
int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t *message);

/* Non-blocking receive: dequeues one message into *out_message or returns IPC_EMPTY. */
int ipc_try_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message);

/* Cooperative receive: yields once if empty then retries; does not block the scheduler. */
int ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message);

/* Blocking receive: blocks the calling process until a message arrives. */
int ipc_recv_blocking_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message);

/* Send a binary notification signal on endpoint (no payload). */
int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint);

/* Cooperative wait on a notification endpoint: yields once if no signal pending. */
int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint);

/* Blocking wait: blocks until a notification is pending. */
int ipc_wait_blocking_for(uint32_t receiver_context_id, uint32_t endpoint);

/* Convenience wrappers that use the current process's context ID. */
int ipc_send(uint32_t endpoint, const ipc_message_t *message);
int ipc_recv(uint32_t endpoint, ipc_message_t *out_message);
int ipc_notify(uint32_t endpoint);
int ipc_wait(uint32_t endpoint);
int ipc_wait_blocking(uint32_t endpoint);

/* Release all endpoints owned by the given context (called on process exit). */
void ipc_endpoints_release_owner(uint32_t owner_context_id);

#endif
