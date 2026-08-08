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
int ipc_endpoint_create(uint32_t owner_context_id, uint32_t* out_endpoint);
int ipc_notification_create(uint32_t owner_context_id, uint32_t* out_endpoint);
int ipc_endpoint_owner(uint32_t endpoint, uint32_t* out_owner_context_id);
int ipc_endpoint_count(uint32_t endpoint, uint32_t* out_count);
int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t* message);
int ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t* out_message);
/*
 * ipc_recv_blocking_for — like ipc_recv_for but blocks via sched_event_wait.
 * Use for callers that want to sleep until a message arrives (e.g. WASM host
 * ipc_recv, kernel_init_runtime).
 * On spurious wake returns IPC_EMPTY; caller should retry.
 */
int ipc_recv_blocking_for(uint32_t receiver_context_id, uint32_t endpoint,
                          ipc_message_t* out_message);
int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint);
int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint);
/*
 * ipc_endpoint_wait_for — block until a MESSAGE endpoint is non-empty or
 * timeout_ms elapses (0 = forever), WITHOUT dequeuing. Lets native services
 * sleep at idle rather than yield-spinning; caller re-polls with ipc_recv_for.
 */
int ipc_endpoint_wait_for(uint32_t receiver_context_id, uint32_t endpoint, uint32_t timeout_ms);
int ipc_send(uint32_t endpoint, const ipc_message_t* message);
int ipc_recv(uint32_t endpoint, ipc_message_t* out_message);
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
int ipc_select_create(uint32_t owner_context_id, uint32_t* out_select_id);
int ipc_select_add(uint32_t select_id, uint32_t endpoint_id, uint32_t owner_context_id);
/* Block until a watched endpoint is ready, or timeout_ms elapses (0 = forever).
 * On timeout returns IPC_EMPTY. */
int ipc_select_wait(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_ready_ep,
                    uint32_t timeout_ms);
/* Create a select set watching endpoints[0..count). */
int ipc_select_listen(uint32_t owner_context_id, const uint32_t* endpoints, uint32_t count,
                      uint32_t* out_select_id);
/* Block until a watched endpoint has a message (or timeout_ms elapses; 0 =
 * forever), then dequeue it. Returns IPC_OK / IPC_EMPTY (spurious, timeout, or
 * lost race; loop) / error. */
int ipc_select_recv(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_endpoint,
                    ipc_message_t* out_message, uint32_t timeout_ms);
void ipc_select_destroy(uint32_t select_id, uint32_t owner_context_id);

struct ipc_select;
/* Called by poll_notify to signal a select set from the sender side. */
void ipc_select_signal(struct ipc_select* sel, uint32_t ep_id);

#ifdef WASMOS_IPC_TEST_SEAMS
/*
 * Host-test seams.  Not compiled into the kernel.  They exist because two
 * states are unreachable in a test otherwise: the endpoint-id counter wrapping
 * at IPC_ENDPOINT_NONE, and the notification counter saturating at UINT32_MAX,
 * both of which need ~2^32 operations to reach honestly.
 */
void ipc_test_set_next_endpoint_id(uint32_t next_id, int wrapped);
int ipc_test_set_notify_count(uint32_t endpoint, uint32_t value);
#endif

#endif
