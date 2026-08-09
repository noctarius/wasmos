#ifndef WASMOS_IPC_H
#define WASMOS_IPC_H

#include <stdint.h>

#include "wasmos_status.h" /* generated transport axis; see the asserts below */

#define IPC_QUEUE_DEPTH 32
#define IPC_ENDPOINT_TABLE_CHUNK 16u
#define IPC_CONTEXT_KERNEL 0u
#define IPC_ENDPOINT_NONE ((uint32_t)~0u)
#define IPC_SELECT_EPS_MAX 8u

/*
 * Each code names ONE failure. IPC_ERR_INVALID used to cover three unrelated
 * ones at once -- a caller passing NULL, an endpoint that does not exist, and
 * an operation applied to the wrong endpoint type -- so a caller could not tell
 * "I have a bug" from "that endpoint died" from "wrong kind of endpoint", and
 * had no basis for deciding whether to retry, re-resolve, or give up. Values
 * are the generated transport axis (see the assertions below).
 */
typedef enum {
    IPC_OK = 0,
    IPC_EMPTY = 1,            /* nothing queued / no signal pending; not an error */
    IPC_ERR_INVALID = -1,     /* malformed argument: NULL pointer, zero count */
    IPC_ERR_PERM = -2,        /* the caller does not own what it named */
    IPC_ERR_FULL = -3,        /* resource exhausted: queue, table, or watch slots */
    IPC_ERR_NOENT = -4,       /* no such endpoint or select set */
    IPC_ERR_TIMEOUT = -5,     /* a bounded wait elapsed without becoming ready */
    IPC_ERR_UNSUPPORTED = -7, /* wrong endpoint type for this operation */
    IPC_ERR_PEER_GONE = -8    /* the endpoint was destroyed while we waited on it */
} ipc_result_t;

/*
 * ipc_result_t predates the generated transport axis and is one of the two
 * taxonomies still to be folded into it (see
 * docs/architecture/34-abi-idl-and-error-model.md: the axis exists to
 * consolidate "today's scattered IPC_ERR_* / PROC_PM_ERR_* fragments", and
 * abi/errors.yaml already annotates FULL as "legacy IPC_ERR_FULL").
 *
 * Until that migration lands, the values coincide by intent rather than by
 * accident, and these assertions say so: anything that reads an ipc_result_t
 * through the generated vocabulary — wasmos_status_str(), a peer decoding a
 * reply — stays correct, and a drift in either definition breaks the build
 * rather than a decoder somewhere downstream.
 *
 * IPC_EMPTY has no counterpart: the transport axis is negative-on-error with
 * no "nothing was waiting" value, so it is deliberately absent below.
 */
_Static_assert((int)IPC_OK == (int)WASMOS_OK, "IPC_OK must match the transport axis");
_Static_assert((int)IPC_ERR_INVALID == (int)WASMOS_INVAL,
               "IPC_ERR_INVALID must match WASMOS_INVAL");
_Static_assert((int)IPC_ERR_PERM == (int)WASMOS_DENIED, "IPC_ERR_PERM must match WASMOS_DENIED");
_Static_assert((int)IPC_ERR_FULL == (int)WASMOS_FULL, "IPC_ERR_FULL must match WASMOS_FULL");
_Static_assert((int)IPC_ERR_NOENT == (int)WASMOS_NOENT, "IPC_ERR_NOENT must match WASMOS_NOENT");
_Static_assert((int)IPC_ERR_TIMEOUT == (int)WASMOS_TIMEOUT,
               "IPC_ERR_TIMEOUT must match WASMOS_TIMEOUT");
_Static_assert((int)IPC_ERR_UNSUPPORTED == (int)WASMOS_UNSUPPORTED,
               "IPC_ERR_UNSUPPORTED must match WASMOS_UNSUPPORTED");
_Static_assert((int)IPC_ERR_PEER_GONE == (int)WASMOS_PEER_GONE,
               "IPC_ERR_PEER_GONE must match WASMOS_PEER_GONE");

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
