#ifndef WASMOS_IPC_H
#define WASMOS_IPC_H

#include <stdint.h>

#define IPC_QUEUE_DEPTH 32
#define IPC_ENDPOINT_TABLE_CHUNK 16u
#define IPC_CONTEXT_KERNEL 0u
#define IPC_ENDPOINT_NONE ((uint32_t)~0u)
#define IPC_SELECT_EPS_MAX 8u

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
/*
 * ipc_recv_blocking_for — like ipc_recv_for but blocks via sched_event_wait
 * under WASMOS_SCHED_THREADABLE.  Use for callers that want to sleep until
 * a message arrives (e.g. WASM host ipc_recv, kernel_init_runtime).
 * On spurious wake returns IPC_EMPTY; caller should retry.
 */
int ipc_recv_blocking_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message);
int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint);
int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint);
int ipc_send(uint32_t endpoint, const ipc_message_t *message);
int ipc_recv(uint32_t endpoint, ipc_message_t *out_message);
int ipc_notify(uint32_t endpoint);
int ipc_wait(uint32_t endpoint);
void ipc_endpoints_release_owner(uint32_t owner_context_id);

/*
 * Select-set API — multi-endpoint blocking wait.
 *
 * A select set watches up to IPC_SELECT_EPS_MAX endpoints simultaneously.
 * ipc_select_wait blocks until any of them has a message or notification
 * ready, then returns the ready endpoint ID.  The caller then calls
 * ipc_recv_for / ipc_wait_for to consume the payload.
 */
int ipc_select_create(uint32_t owner_context_id, uint32_t *out_select_id);
int ipc_select_add(uint32_t select_id, uint32_t endpoint_id,
                   uint32_t owner_context_id);
int ipc_select_wait(uint32_t select_id, uint32_t owner_context_id,
                    uint32_t *out_ready_ep);
/* Create a select set watching endpoints[0..count). */
int ipc_select_listen(uint32_t owner_context_id, const uint32_t *endpoints,
                      uint32_t count, uint32_t *out_select_id);
/* Block until a watched endpoint has a message, then dequeue it. Returns
 * IPC_OK / IPC_EMPTY (spurious or lost race; loop) / error. */
int ipc_select_recv(uint32_t select_id, uint32_t owner_context_id,
                    uint32_t *out_endpoint, ipc_message_t *out_message);
void ipc_select_destroy(uint32_t select_id, uint32_t owner_context_id);

#ifdef WASMOS_SCHED_THREADABLE
struct ipc_select;
/* Called by poll_notify to signal a select set from the sender side. */
void ipc_select_signal(struct ipc_select *sel, uint32_t ep_id);
#endif

#endif
