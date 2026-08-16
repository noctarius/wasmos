#ifndef WASMOS_IPC_H
#define WASMOS_IPC_H

#include <stdint.h>

#include "wasmos_status.h" /* generated transport axis; see the asserts below */

/*
 * Kernel IPC transport.
 *
 * LOCK ORDER, file-wide and mandatory for anything reaching into ipc.c:
 *   g_select_table_lock -> g_endpoint_table_lock -> ep->lock -> an event lock
 *   (ep->event.lock, or sel->event.lock via poll_notify -> ipc_select_signal).
 * No two endpoint locks are ever held at once, which is why the send path
 * checks the sender's ownership through the table lock alone.
 *
 * BLOCKING. Everything here returns immediately except ipc_recv_blocking_for,
 * ipc_endpoint_wait_for, ipc_select_wait and ipc_select_recv, which park the
 * calling thread through sched_event_wait. A parked caller may resume
 * spuriously and must loop.
 */

/* Messages an endpoint can hold before ipc_send_from starts refusing with
 * IPC_ERR_FULL. There is no backpressure short of that: the sender is never
 * blocked, so a slow receiver turns into dropped sends at its peers. */
#define IPC_QUEUE_DEPTH 32
/* Endpoints per kmem chunk of the endpoint table. A growth granularity, not a
 * ceiling -- see IPC_ENDPOINT_PER_CONTEXT_MAX for the actual bound. */
#define IPC_ENDPOINT_TABLE_CHUNK 16u
/* The kernel's own context id. Passing it as the sender/receiver context
 * bypasses every ownership check, so it must never be used on behalf of a
 * guest; pass that guest's context id instead. */
#define IPC_CONTEXT_KERNEL 0u
/* Reserved id meaning "no endpoint". Never allocated, and rejected by every
 * lookup, so it is safe as an uninitialised value. */
#define IPC_ENDPOINT_NONE ((uint32_t)~0u)
/* Endpoints one select set may watch. A fixed array in the set, so a ninth
 * distinct endpoint is refused with IPC_ERR_FULL (re-adding one already watched
 * is not, and stays a no-op even on a full set). */
#define IPC_SELECT_EPS_MAX 8u

/*
 * Each code names ONE failure. A single code covering a NULL argument, a
 * non-existent endpoint and an operation applied to the wrong endpoint type
 * leaves the caller unable to tell a bug in its own code from a dead peer from
 * a type mismatch, and so with no basis for deciding whether to retry,
 * re-resolve, or give up. Values are the generated transport axis (see the
 * assertions below).
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
    IPC_ERR_PEER_GONE = -8    /* the endpoint was destroyed while the caller waited on it */
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

/* An endpoint's type is fixed at creation and selects which half of the API
 * applies to it. MESSAGE endpoints carry ipc_message_t payloads through
 * send/recv and may be sent to by any context. NOTIFICATION endpoints carry
 * only a counter through notify/wait, and only their owner (or the kernel) may
 * raise one. Applying the wrong half returns IPC_ERR_UNSUPPORTED. */
typedef enum {
    IPC_ENDPOINT_TYPE_MESSAGE = 0,
    IPC_ENDPOINT_TYPE_NOTIFICATION = 1
} ipc_endpoint_type_t;

/* The entire message: 32 bytes, copied by value into and out of the queue, with
 * no out-of-line payload. Anything larger travels in a transfer buffer whose id
 * is carried in one of the args.
 *
 * `type` is the protocol opcode (abi/opcodes.yaml). `source` is the endpoint a
 * reply should be sent to and is verified against the sender's context by
 * ipc_send_from -- a non-kernel sender cannot name an endpoint it does not own.
 * `destination` is OVERWRITTEN by the send with the endpoint actually delivered
 * to, so a receiver can trust it. `request_id` correlates a reply with its
 * request; the remaining four words are opcode-specific. */
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

/* Create the endpoint and select tables. Panics if either cannot be
 * initialised, since nothing in the system works without them. Call once,
 * early, before any endpoint is created. */
void ipc_init(void);
/* Create a MESSAGE / NOTIFICATION endpoint owned by owner_context_id and return
 * its id in *out_endpoint. Both kinds come from the same table and share one
 * per-context quota, so asking for a notification does not buy a second
 * allowance. Returns IPC_OK, IPC_ERR_INVALID for a NULL out pointer, or
 * IPC_ERR_FULL at the quota or when the table cannot grow. */
int ipc_endpoint_create(uint32_t owner_context_id, uint32_t* out_endpoint);
int ipc_notification_create(uint32_t owner_context_id, uint32_t* out_endpoint);
/* Owning context of an endpoint, for either kind. Returns IPC_OK,
 * IPC_ERR_NOENT if no such endpoint exists, or IPC_ERR_INVALID for a NULL out
 * pointer. The workhorse of PM-side authorisation: "who owns the endpoint this
 * request claims as its source?". */
int ipc_endpoint_owner(uint32_t endpoint, uint32_t* out_owner_context_id);
/* Print the last IPC events -- who sent what to whom, and the result -- oldest
 * first.  For the NMI diagnostic path: takes no locks, and a torn entry costs a
 * confusing line rather than a fault.  It is what distinguishes a request that
 * was never sent from one that was sent and lost, which the thread dump alone
 * cannot. */
void ipc_diag_dump_trace(void);

/* ipc_diag_wait_info's return values: which kind of object a blocked thread's
 * event belongs to. */
#define IPC_DIAG_WAIT_ENDPOINT 0
#define IPC_DIAG_WAIT_SELECT 1

/* Describe the wait a blocked thread is in, from its sched_event_t alone and
 * WITHOUT taking any lock -- it is called from the NMI diagnostic path, where a
 * lock may be held by a CPU that will never release it.  See the definition in
 * ipc.c for the fields; returns IPC_DIAG_WAIT_ENDPOINT, IPC_DIAG_WAIT_SELECT,
 * or -1 for any other kind of wait.  Values are read racily by design. */
int ipc_diag_wait_info(const void* event, uint32_t* out_id, uint32_t* out_count,
                       uint32_t* out_owner, uint32_t watch_max, uint32_t* out_watch_ids,
                       uint32_t* out_watch_counts, uint32_t* out_watched);

/* Queued message count. A snapshot taken under the endpoint lock and stale as
 * soon as it is returned. Returns IPC_OK / IPC_ERR_NOENT / IPC_ERR_INVALID. */
int ipc_endpoint_count(uint32_t endpoint, uint32_t* out_count);
/*
 * Enqueue a copy of *message on a MESSAGE endpoint and wake one waiter plus any
 * select sets watching it. Never blocks and never overwrites: a full queue is
 * refused with IPC_ERR_FULL and the caller decides what to do.
 *
 * Any context may send to any message endpoint -- send is the one operation
 * without an owner check. What IS checked is message->source: a non-kernel
 * sender must own it, else IPC_ERR_PERM. Also IPC_ERR_INVALID for a NULL
 * message, IPC_ERR_NOENT for an unknown endpoint, IPC_ERR_UNSUPPORTED for a
 * notification endpoint.
 */
int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t* message);
/* Non-blocking dequeue: IPC_OK with *out_message filled, or IPC_EMPTY when the
 * queue is empty. Only the endpoint's owner may receive from it. */
int ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t* out_message);
/*
 * ipc_recv_blocking_for — like ipc_recv_for but blocks via sched_event_wait.
 * Use for callers that want to sleep until a message arrives (e.g. WASM host
 * ipc_recv, kernel_init_runtime).
 * On spurious wake returns IPC_EMPTY; caller should retry.
 * Waits without a timeout, so a caller that is never sent to never returns.
 * Also returns IPC_ERR_PEER_GONE if the endpoint was destroyed while parked --
 * distinct from IPC_ERR_NOENT, because the handle WAS valid at the block.
 */
int ipc_recv_blocking_for(uint32_t receiver_context_id, uint32_t endpoint,
                          ipc_message_t* out_message);
/* Raise a NOTIFICATION endpoint's counter by one and signal any select set
 * watching it. Unlike a message send this is owner-only (IPC_ERR_PERM
 * otherwise). The counter SATURATES at UINT32_MAX rather than wrapping, so
 * notifications past that point are silently coalesced instead of being read as
 * zero. Never blocks. */
int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint);
/* Consume one pending notification. Despite the name it does NOT block:
 * IPC_OK when a notification was consumed, IPC_EMPTY when the counter is zero.
 * Owner-only; IPC_ERR_UNSUPPORTED on a message endpoint. */
int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint);
/*
 * ipc_endpoint_wait_for — block until a MESSAGE endpoint is non-empty or
 * timeout_ms elapses (0 = forever), WITHOUT dequeuing. Lets native services
 * sleep at idle rather than yield-spinning; caller re-polls with ipc_recv_for.
 */
int ipc_endpoint_wait_for(uint32_t receiver_context_id, uint32_t endpoint, uint32_t timeout_ms);
/* Kernel-side shorthands: the *_for/_from variants with sender/receiver fixed to
 * IPC_CONTEXT_KERNEL, which bypasses the owner checks. Not for anything acting
 * on a guest's behalf -- pass that guest's context id explicitly instead. */
int ipc_send(uint32_t endpoint, const ipc_message_t* message);
int ipc_recv(uint32_t endpoint, ipc_message_t* out_message);
int ipc_notify(uint32_t endpoint);
int ipc_wait(uint32_t endpoint);
/* Destroy every endpoint owned by owner_context_id, aborting its waiters
 * (SCHED_PEND_ABORT) and freeing its poll hub first, so nothing is left parked
 * on an endpoint that no longer exists. Called from process teardown; a
 * context id of 0 (the kernel) is ignored, since kernel endpoints outlive any
 * one process. */
void ipc_endpoints_release_owner(uint32_t owner_context_id);

/*
 * Select-set API — multi-endpoint blocking wait.
 *
 * A select set watches up to IPC_SELECT_EPS_MAX endpoints simultaneously.
 * ipc_select_wait blocks until any of them has a message or notification
 * ready, then returns the ready endpoint ID.  The caller then calls
 * ipc_recv_for / ipc_wait_for to consume the payload.
 */
/* Select sets per kmem chunk. The select table, like the endpoint table, grows
 * on demand out of kmem, so this is a growth granularity and not a ceiling: the
 * number of parked services is bounded by memory and by the per-context quota
 * below. */
#define IPC_SELECT_TABLE_CHUNK 32u

/*
 * Per-context ceilings on the two shared IPC tables.
 *
 * Both tables are global and both grow out of kernel memory, so without a
 * per-context cap one context can consume all of either and starve every other
 * one: a service that cannot create a select set cannot park, and one that
 * cannot create an endpoint cannot be reached at all. The caps sit well above
 * what any component uses (the busiest has four endpoint-create sites and two
 * select-create sites); they bound a runaway rather than ration normal use.
 */
#define IPC_SELECT_PER_CONTEXT_MAX 8u
#define IPC_ENDPOINT_PER_CONTEXT_MAX 64u

/* Create an empty select set owned by owner_context_id. Returns IPC_OK,
 * IPC_ERR_INVALID for a NULL out pointer, or IPC_ERR_FULL at the per-context
 * quota. */
int ipc_select_create(uint32_t owner_context_id, uint32_t* out_select_id);
/* Watch endpoint_id with this set (either endpoint kind). Only the set's owner
 * may add, and the endpoint must resolve -- an unknown id is REFUSED with
 * IPC_ERR_NOENT rather than recorded, because a recorded-but-unwatchable
 * endpoint yields a set that blocks forever while the caller believes it is
 * watched. Adding an endpoint already in the set is a successful no-op, even
 * when the set is full. Otherwise IPC_ERR_PERM, or IPC_ERR_FULL at
 * IPC_SELECT_EPS_MAX or when the watcher cannot be allocated. */
int ipc_select_add(uint32_t select_id, uint32_t endpoint_id, uint32_t owner_context_id);
/* Block until a watched endpoint is ready, or timeout_ms elapses (0 = forever).
 * On timeout returns IPC_EMPTY.
 *
 * Readiness is a single-slot LATCH, not a queue: two endpoints becoming ready
 * before the waiter runs report only the later one, so the caller must treat
 * the returned endpoint as a hint and be willing to poll the others. IPC_EMPTY
 * also covers a spurious wake and a set destroyed underneath the waiter -- all
 * three mean "loop". Owner-only (IPC_ERR_PERM); IPC_ERR_NOENT for an unknown
 * set, IPC_ERR_INVALID for a NULL out pointer. */
int ipc_select_wait(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_ready_ep,
                    uint32_t timeout_ms);
/* Create a select set watching endpoints[0..count). All-or-nothing: if any add
 * fails the partially built set is destroyed and that error is returned, so
 * *out_select_id is written only on IPC_OK. count 0 or a NULL pointer gives
 * IPC_ERR_INVALID. */
int ipc_select_listen(uint32_t owner_context_id, const uint32_t* endpoints, uint32_t count,
                      uint32_t* out_select_id);
/* Block until a watched endpoint has a message (or timeout_ms elapses; 0 =
 * forever), then dequeue it. Returns IPC_OK / IPC_EMPTY (spurious, timeout, or
 * lost race; loop) / error. */
int ipc_select_recv(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_endpoint,
                    ipc_message_t* out_message, uint32_t timeout_ms);
/* Unregister the set's watchers from every endpoint it watches, abort its
 * waiters (they see IPC_EMPTY), and free it. Silent no-op for an unknown set or
 * a non-owner -- there is no return value, so a caller cannot tell the two
 * apart from a successful destroy. */
void ipc_select_destroy(uint32_t select_id, uint32_t owner_context_id);

struct ipc_select;
/* Called by poll_notify to signal a select set from the sender side.
 * Latches ep_id as the ready endpoint (overwriting any previous one) and wakes
 * one waiter, both under the set's event lock -- the single authority that
 * makes the check-then-block in ipc_select_wait lost-wakeup free. Runs with the
 * signalling endpoint's lock held, which the file-wide lock order permits. */
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
