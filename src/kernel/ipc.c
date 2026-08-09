#include "ipc.h"
#include "list.h"
#include "process.h"
#include "thread.h"
#include "sync/spinlock.h"
#include "paging.h"
#include "stdlib.h"
#include "string.h"

#include "sched_event.h"
#include "poll.h"

/*
 * The kernel IPC layer keeps transport deliberately small: fixed-size endpoint
 * tables, bounded queues, context ownership checks, and optional notification
 * counters. Higher-level protocols are built entirely in drivers and services.
 */

typedef struct {
    uint32_t id;
    uint32_t in_use;
    ipc_endpoint_type_t type;
    uint32_t owner_context_id;
    ksync_spinlock_t lock;
    ipc_message_t queue[IPC_QUEUE_DEPTH];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t notify_count;
    sched_event_t event;        /* supports N waiters */
    poll_struct_t* poll_struct; /* lazily allocated; notified on send */
} ipc_endpoint_t;

/* Select-set: watches up to IPC_SELECT_EPS_MAX endpoints simultaneously. */
typedef struct ipc_select {
    uint32_t id;
    uint8_t in_use;
    uint32_t owner_context_id;
    sched_event_t event;
    /* ready_ep is protected by event.lock, not a separate lock.  Keeping
     * event.lock as the single authority for both the wait_list and ready_ep
     * prevents the SMP lost-wakeup: ipc_select_signal() sets ready_ep and
     * calls sched_event_wake_one() under the same event.lock that
     * ipc_select_wait() holds across the "check ready_ep → add to wait_list"
     * critical section, so a signal can never slip between the check and the
     * enqueue. */
    uint32_t ready_ep; /* endpoint that triggered the wake */
    uint32_t ep_ids[IPC_SELECT_EPS_MAX];
    uint32_t ep_count;
} ipc_select_t;

#define IPC_SELECT_TABLE_SIZE 32u
static ipc_select_t g_select_table[IPC_SELECT_TABLE_SIZE];
static ksync_spinlock_t g_select_table_lock;

static list_t g_endpoint_table;
static ksync_spinlock_t g_endpoint_table_lock;
static uint32_t g_next_endpoint_id;

/*
 * Returns the endpoint with ep->lock held.  The caller must call
 * ksync_spinlock_unlock(&ep->lock) when done.  Lock order: g_endpoint_table_lock ->
 * ep->lock.  ep->lock is acquired under g_endpoint_table_lock so that
 * ipc_endpoints_release_owner cannot remove the endpoint between the lookup
 * and the caller's first use.
 */
static ipc_endpoint_t* ipc_endpoint_get(uint32_t endpoint_id) {
    list_iter_t it;
    if (endpoint_id == 0 || endpoint_id == IPC_ENDPOINT_NONE) {
        return 0;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t* ep = (ipc_endpoint_t*)list_first(&g_endpoint_table, &it);
    while (ep) {
        if (ep->id == endpoint_id && ep->in_use) {
            ksync_spinlock_lock(&ep->lock);
            ksync_spinlock_unlock(&g_endpoint_table_lock);
            return ep; /* returned with ep->lock held */
        }
        ep = (ipc_endpoint_t*)list_next(&it);
    }
    ksync_spinlock_unlock(&g_endpoint_table_lock);
    return 0;
}

/*
 * Reads the owner context ID of an endpoint under the table lock only.
 * Used by ipc_send_from for source permission checks that must not hold
 * ep->lock simultaneously (which would require a second nested endpoint lock).
 */
static uint32_t ipc_endpoint_owner_context(uint32_t endpoint_id) {
    list_iter_t it;
    if (endpoint_id == 0 || endpoint_id == IPC_ENDPOINT_NONE) {
        return 0;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t* ep = (ipc_endpoint_t*)list_first(&g_endpoint_table, &it);
    while (ep) {
        if (ep->id == endpoint_id && ep->in_use) {
            uint32_t ctx = ep->owner_context_id;
            ksync_spinlock_unlock(&g_endpoint_table_lock);
            return ctx;
        }
        ep = (ipc_endpoint_t*)list_next(&it);
    }
    ksync_spinlock_unlock(&g_endpoint_table_lock);
    return 0;
}

/*
 * Hand out the next endpoint id.  Caller holds g_endpoint_table_lock.
 *
 * The counter wraps back to 1 at IPC_ENDPOINT_NONE, so past the first wrap a
 * fresh id can collide with an endpoint that is still live -- and
 * ipc_endpoint_get returns the FIRST match, so the newer endpoint would
 * silently steal the older one's traffic.  After a wrap has happened, skip ids
 * that are still in use.  The scan is gated on g_endpoint_id_wrapped so the
 * ordinary case stays a post-increment.
 */
static uint32_t g_endpoint_id_wrapped;

static uint32_t ipc_alloc_endpoint_id(void) {
    for (;;) {
        uint32_t id = g_next_endpoint_id++;
        if (g_next_endpoint_id == IPC_ENDPOINT_NONE) {
            g_next_endpoint_id = 1;
            g_endpoint_id_wrapped = 1;
        }
        if (!g_endpoint_id_wrapped) {
            return id;
        }
        list_iter_t it;
        int taken = 0;
        ipc_endpoint_t* ep = (ipc_endpoint_t*)list_first(&g_endpoint_table, &it);
        while (ep) {
            if (ep->in_use && ep->id == id) {
                taken = 1;
                break;
            }
            ep = (ipc_endpoint_t*)list_next(&it);
        }
        if (!taken) {
            return id;
        }
    }
}

void ipc_init(void) {
    ksync_spinlock_init(&g_endpoint_table_lock);
    g_next_endpoint_id = 1;
    g_endpoint_id_wrapped = 0;
    if (list_init(&g_endpoint_table, (uint32_t)sizeof(ipc_endpoint_t), LIST_IMPL_ARRAY_CHUNK,
                  IPC_ENDPOINT_TABLE_CHUNK) != 0) {
        for (;;) {
        }
    }
    ksync_spinlock_init(&g_select_table_lock);
    memset(g_select_table, 0, sizeof(g_select_table));
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        sched_event_init(&g_select_table[i].event, SCHED_EVENT_TYPE_SELECT);
    }
}

/*
 * Look up `endpoint` and check it is of `type`.  Returns it with ep->lock HELD,
 * or 0 with *out_rc set and no lock held.  Every operation that names an
 * endpoint opens with exactly this, so it lives here once.
 */
static ipc_endpoint_t* ipc_endpoint_acquire(uint32_t endpoint, ipc_endpoint_type_t type,
                                            int* out_rc) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        *out_rc = IPC_ERR_INVALID;
        return 0;
    }
    if (ep->type != type) {
        ksync_spinlock_unlock(&ep->lock);
        *out_rc = IPC_ERR_INVALID;
        return 0;
    }
    *out_rc = IPC_OK;
    return ep;
}

/*
 * As above, plus the ownership check every operation other than send performs:
 * only the owning context -- or the kernel -- may receive from, wait on, or
 * signal an endpoint.  Send is the exception (any context may send to any
 * message endpoint), which is why it stays on the plain acquire.
 */
static ipc_endpoint_t* ipc_endpoint_acquire_owned(uint32_t endpoint, ipc_endpoint_type_t type,
                                                  uint32_t context_id, int* out_rc) {
    ipc_endpoint_t* ep = ipc_endpoint_acquire(endpoint, type, out_rc);
    if (!ep) {
        return 0;
    }
    if (context_id != IPC_CONTEXT_KERNEL && ep->owner_context_id != context_id) {
        ksync_spinlock_unlock(&ep->lock);
        *out_rc = IPC_ERR_PERM;
        return 0;
    }
    return ep;
}

/* The two endpoint kinds differ only in ep->type; everything else -- id
 * allocation, the zeroed queue state, the embedded event -- is identical. */
static int ipc_endpoint_create_typed(uint32_t owner_context_id, ipc_endpoint_type_t type,
                                     uint32_t* out_endpoint) {
    if (!out_endpoint) {
        return IPC_ERR_INVALID;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t* ep = (ipc_endpoint_t*)list_alloc(&g_endpoint_table);
    if (!ep) {
        ksync_spinlock_unlock(&g_endpoint_table_lock);
        return IPC_ERR_FULL;
    }
    ep->id = ipc_alloc_endpoint_id();
    ep->in_use = 1;
    ep->type = type;
    ep->owner_context_id = owner_context_id;
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->notify_count = 0;
    ksync_spinlock_init(&ep->lock);
    sched_event_init(&ep->event, SCHED_EVENT_TYPE_IPC);
    ep->poll_struct = 0;
    uint32_t id = ep->id;
    ksync_spinlock_unlock(&g_endpoint_table_lock);
    *out_endpoint = id;
    return IPC_OK;
}

int ipc_endpoint_create(uint32_t owner_context_id, uint32_t* out_endpoint) {
    return ipc_endpoint_create_typed(owner_context_id, IPC_ENDPOINT_TYPE_MESSAGE, out_endpoint);
}

int ipc_notification_create(uint32_t owner_context_id, uint32_t* out_endpoint) {
    return ipc_endpoint_create_typed(owner_context_id, IPC_ENDPOINT_TYPE_NOTIFICATION,
                                     out_endpoint);
}

int ipc_endpoint_owner(uint32_t endpoint, uint32_t* out_owner_context_id) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (!out_owner_context_id) {
        ksync_spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    *out_owner_context_id = ep->owner_context_id;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_endpoint_count(uint32_t endpoint, uint32_t* out_count) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (!out_count) {
        ksync_spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    *out_count = ep->count;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t* message) {
    if (!message) {
        return IPC_ERR_INVALID;
    }

    /*
     * Source permission check is done before acquiring ep->lock to avoid
     * holding two endpoint locks at once.  ipc_endpoint_owner_context uses
     * g_endpoint_table_lock only, which is compatible with the lock order.
     */
    if (sender_context_id != IPC_CONTEXT_KERNEL) {
        if (message->source == IPC_ENDPOINT_NONE) {
            return IPC_ERR_PERM;
        }
        uint32_t src_owner = ipc_endpoint_owner_context(message->source);
        if (src_owner == 0 || src_owner != sender_context_id) {
            return IPC_ERR_PERM;
        }
    }

    int rc = IPC_OK;
    ipc_endpoint_t* ep = ipc_endpoint_acquire(endpoint, IPC_ENDPOINT_TYPE_MESSAGE, &rc);
    if (!ep) {
        return rc;
    }

    if (ep->count >= IPC_QUEUE_DEPTH) {
        ksync_spinlock_unlock(&ep->lock);
        return IPC_ERR_FULL;
    }

    ipc_message_t msg = *message;
    msg.destination = endpoint;
    ep->queue[ep->tail] = msg;
    ep->tail = (ep->tail + 1u) % IPC_QUEUE_DEPTH;
    ep->count++;
    ksync_spinlock_lock(&ep->event.lock);
    sched_event_wake_one(&ep->event, 0, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ep->event.lock);
    /* poll_notify runs under ep->lock.  Reading ep->poll_struct here and
     * notifying after the unlock would race ipc_endpoints_release_owner, which
     * takes ep->lock and frees the poll_struct -- the notify would then walk
     * freed watcher nodes.  Lock order is g_select_table_lock -> ep->lock ->
     * sel->event.lock, and poll_notify only ever takes the last of those, so
     * holding ep->lock across it is consistent with every other path. */
    if (ep->poll_struct) {
        poll_notify(ep->poll_struct, POLL_EV_IN, ep->id);
    }
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t* out_message) {
    int rc = IPC_OK;
    if (!out_message) {
        return IPC_ERR_INVALID;
    }
    ipc_endpoint_t* ep =
        ipc_endpoint_acquire_owned(endpoint, IPC_ENDPOINT_TYPE_MESSAGE, receiver_context_id, &rc);
    if (!ep) {
        return rc;
    }

    if (ep->count == 0) {
        /* Non-blocking poll must not register a waiter. On SMP a sender can
         * otherwise "wake" a thread that never actually blocked, turning a
         * still-running thread back into READY on another CPU. */
        ksync_spinlock_unlock(&ep->lock);
        return IPC_EMPTY;
    }

    *out_message = ep->queue[ep->head];
    ep->head = (ep->head + 1u) % IPC_QUEUE_DEPTH;
    ep->count--;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_recv_blocking_for(uint32_t receiver_context_id, uint32_t endpoint,
                          ipc_message_t* out_message) {
    int rc = IPC_OK;
    if (!out_message) {
        return IPC_ERR_INVALID;
    }
    ipc_endpoint_t* ep =
        ipc_endpoint_acquire_owned(endpoint, IPC_ENDPOINT_TYPE_MESSAGE, receiver_context_id, &rc);
    if (!ep) {
        return rc;
    }
    if (ep->count == 0) {
        /* Block until a sender enqueues a message and wakes us. */
        ksync_spinlock_lock(&ep->event.lock);
        ksync_spinlock_unlock(&ep->lock);
        sched_event_wait(&ep->event, 0);
        ep = ipc_endpoint_get(endpoint);
        if (!ep)
            return IPC_ERR_INVALID;
        if (ep->count == 0) {
            ksync_spinlock_unlock(&ep->lock);
            return IPC_EMPTY; /* spurious wake; caller should retry */
        }
    }
    *out_message = ep->queue[ep->head];
    ep->head = (ep->head + 1u) % IPC_QUEUE_DEPTH;
    ep->count--;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

/* Block until `endpoint` has at least one queued message, or timeout_ms elapses
 * (0 = wait forever). Does NOT dequeue — the caller drains with ipc_recv_for
 * afterwards. Lets a service sleep at idle instead of yield-spinning its poll
 * loop. Returns IPC_OK once woken (message may already have been drained by a
 * racing waiter, so the caller must re-poll and tolerate an empty read). */
int ipc_endpoint_wait_for(uint32_t receiver_context_id, uint32_t endpoint, uint32_t timeout_ms) {
    int rc = IPC_OK;
    ipc_endpoint_t* ep =
        ipc_endpoint_acquire_owned(endpoint, IPC_ENDPOINT_TYPE_MESSAGE, receiver_context_id, &rc);
    if (!ep) {
        return rc;
    }
    if (ep->count != 0) {
        ksync_spinlock_unlock(&ep->lock);
        return IPC_OK; /* already readable */
    }
    /* Arm and block under event.lock (single authority), mirroring
     * ipc_recv_blocking_for's lost-wakeup-safe handoff. */
    ksync_spinlock_lock(&ep->event.lock);
    ksync_spinlock_unlock(&ep->lock);
    sched_event_wait(&ep->event, timeout_ms);
    return IPC_OK;
}

int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint) {
    int rc = IPC_OK;
    /* Only the owner (or the kernel) may raise a notification -- unlike a
     * message send, which any context may perform against any endpoint. */
    ipc_endpoint_t* ep = ipc_endpoint_acquire_owned(endpoint, IPC_ENDPOINT_TYPE_NOTIFICATION,
                                                    sender_context_id, &rc);
    if (!ep) {
        return rc;
    }

    if (ep->notify_count != UINT32_MAX) {
        ep->notify_count++;
    }
    ksync_spinlock_lock(&ep->event.lock);
    sched_event_wake_one(&ep->event, 0, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ep->event.lock);
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint) {
    int rc = IPC_OK;
    ipc_endpoint_t* ep = ipc_endpoint_acquire_owned(endpoint, IPC_ENDPOINT_TYPE_NOTIFICATION,
                                                    receiver_context_id, &rc);
    if (!ep) {
        return rc;
    }

    if (ep->notify_count == 0) {
        /* Non-blocking notify poll must not arm event wake state for a thread
         * that is still running. */
        ksync_spinlock_unlock(&ep->lock);
        return IPC_EMPTY;
    }
    ep->notify_count--;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_send(uint32_t endpoint, const ipc_message_t* message) {
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, message);
}

int ipc_recv(uint32_t endpoint, ipc_message_t* out_message) {
    return ipc_recv_for(IPC_CONTEXT_KERNEL, endpoint, out_message);
}

int ipc_notify(uint32_t endpoint) {
    return ipc_notify_from(IPC_CONTEXT_KERNEL, endpoint);
}

int ipc_wait(uint32_t endpoint) {
    return ipc_wait_for(IPC_CONTEXT_KERNEL, endpoint);
}

void ipc_endpoints_release_owner(uint32_t owner_context_id) {
    if (owner_context_id == 0) {
        return;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    list_iter_t it;
    ipc_endpoint_t* ep = (ipc_endpoint_t*)list_first(&g_endpoint_table, &it);
    while (ep) {
        ipc_endpoint_t* next = (ipc_endpoint_t*)list_next(&it);
        if (ep->in_use && ep->owner_context_id == owner_context_id) {
            ksync_spinlock_lock(&ep->lock);
            if (ep->in_use && ep->owner_context_id == owner_context_id) {
                ep->in_use = 0;
                ksync_spinlock_lock(&ep->event.lock);
                sched_event_abort_all(&ep->event);
                ksync_spinlock_unlock(&ep->event.lock);
                if (ep->poll_struct) {
                    poll_struct_free(ep->poll_struct);
                    ep->poll_struct = 0;
                }
            }
            ksync_spinlock_unlock(&ep->lock);
            list_remove(&g_endpoint_table, ep);
        }
        ep = next;
    }
    ksync_spinlock_unlock(&g_endpoint_table_lock);
}

/* -------------------------------------------------------------------------
 * Select-set API
 * ------------------------------------------------------------------------- */

static ipc_select_t* ipc_select_find(uint32_t select_id, uint32_t owner_context_id) {
    /* Caller holds g_select_table_lock. */
    if (select_id == 0 || select_id > IPC_SELECT_TABLE_SIZE) {
        return 0;
    }
    ipc_select_t* sel = &g_select_table[select_id - 1u];
    if (!sel->in_use) {
        return 0;
    }
    if (owner_context_id != IPC_CONTEXT_KERNEL && sel->owner_context_id != owner_context_id) {
        return 0;
    }
    return sel;
}

int ipc_select_create(uint32_t owner_context_id, uint32_t* out_select_id) {
    if (!out_select_id) {
        return IPC_ERR_INVALID;
    }
    ksync_spinlock_lock(&g_select_table_lock);
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        ipc_select_t* sel = &g_select_table[i];
        if (!sel->in_use) {
            sel->in_use = 1;
            sel->owner_context_id = owner_context_id;
            sel->ready_ep = IPC_ENDPOINT_NONE;
            sel->ep_count = 0;
            sched_event_init(&sel->event, SCHED_EVENT_TYPE_SELECT);
            *out_select_id = i + 1u;
            ksync_spinlock_unlock(&g_select_table_lock);
            return IPC_OK;
        }
    }
    ksync_spinlock_unlock(&g_select_table_lock);
    return IPC_ERR_FULL;
}

int ipc_select_add(uint32_t select_id, uint32_t endpoint_id, uint32_t owner_context_id) {
    ksync_spinlock_lock(&g_select_table_lock);
    ipc_select_t* sel = ipc_select_find(select_id, owner_context_id);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_INVALID;
    }
    /*
     * Adding an endpoint the set already watches is a no-op, not a second
     * registration.  Callers routinely build a set from several handles that
     * can legitimately coincide -- a service whose reply and event endpoints
     * are the same -- so refusing would fail them at startup for a harmless
     * call.  Registering twice was worse: it burned one of only
     * IPC_SELECT_EPS_MAX watch slots and left two poll watchers, so a single
     * send ran ipc_select_signal twice.  That stayed invisible only because
     * ready_ep is a latch that swallows the second signal.
     *
     * Checked before the capacity test on purpose: re-adding an endpoint to a
     * full set is still correct, because it is already being watched.
     */
    for (uint32_t i = 0; i < sel->ep_count; ++i) {
        if (sel->ep_ids[i] == endpoint_id) {
            ksync_spinlock_unlock(&g_select_table_lock);
            return IPC_OK;
        }
    }
    if (sel->ep_count >= IPC_SELECT_EPS_MAX) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_FULL;
    }
    /*
     * Register the push watcher on the endpoint.  A failure here has to be
     * reported: without a watcher the endpoint can never signal this set, so
     * reporting IPC_OK would hand the caller a set that blocks forever on an
     * endpoint it believes it is watching.
     *
     * FIXME: an endpoint_id that does not resolve is still accepted into
     * ep_ids -- it simply never signals.  Callers add endpoints they have just
     * created, so this has not bitten, but it is the same silent-never-ready
     * failure as above and should become IPC_ERR_INVALID once every caller is
     * known to add only live endpoints.
     */
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint_id);
    if (ep) {
        if (!ep->poll_struct) {
            ep->poll_struct = poll_struct_alloc();
        }
        if (!ep->poll_struct || poll_struct_add(ep->poll_struct, POLL_EV_IN, sel, 0) != 0) {
            ksync_spinlock_unlock(&ep->lock);
            ksync_spinlock_unlock(&g_select_table_lock);
            return IPC_ERR_FULL;
        }
        ksync_spinlock_unlock(&ep->lock);
    }

    sel->ep_ids[sel->ep_count++] = endpoint_id;
    ksync_spinlock_unlock(&g_select_table_lock);
    return IPC_OK;
}

/* Consume the readiness latch.  Caller holds sel->event.lock, which the struct
 * comment names as the single authority for ready_ep.  Returns 1 if an
 * endpoint was taken. */
static int ipc_select_take_ready(ipc_select_t* sel, uint32_t* out_ready_ep) {
    if (sel->ready_ep == IPC_ENDPOINT_NONE) {
        return 0;
    }
    *out_ready_ep = sel->ready_ep;
    sel->ready_ep = IPC_ENDPOINT_NONE;
    return 1;
}

int ipc_select_wait(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_ready_ep,
                    uint32_t timeout_ms) {
    if (!out_ready_ep) {
        return IPC_ERR_INVALID;
    }

    ksync_spinlock_lock(&g_select_table_lock);
    ipc_select_t* sel = ipc_select_find(select_id, owner_context_id);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_INVALID;
    }

    /* Acquire event.lock BEFORE releasing g_select_table_lock.  It does two
     * jobs at once, and both are required.
     *
     * It closes the SMP lost-wakeup window around the block: ipc_select_signal
     * holds event.lock while it writes ready_ep and calls
     * sched_event_wake_one, so any signal firing after this point must wait
     * until we are already in the wait_list inside sched_event_wait.
     *
     * It also puts the take-and-clear below on the same lock the signal writes
     * under.  A fast path that read ready_ep under g_select_table_lock instead
     * -- as this did -- races a concurrent signal and can overwrite the
     * endpoint it just published with IPC_ENDPOINT_NONE, dropping that
     * readiness entirely: the set then blocks until some *later* message
     * arrives, and the queued one is never looked at. */
    ksync_spinlock_lock(&sel->event.lock);
    ksync_spinlock_unlock(&g_select_table_lock);

    if (ipc_select_take_ready(sel, out_ready_ep)) {
        ksync_spinlock_unlock(&sel->event.lock);
        return IPC_OK;
    }

    /* sched_event_wait releases sel->event.lock before yielding. A non-zero
     * timeout_ms wakes us after the deadline (returning IPC_EMPTY). */
    sched_event_wait(&sel->event, timeout_ms);

    /* After wake: re-check under event.lock (same lock as ipc_select_signal). */
    ksync_spinlock_lock(&sel->event.lock);
    int got = ipc_select_take_ready(sel, out_ready_ep);
    ksync_spinlock_unlock(&sel->event.lock);
    return got ? IPC_OK : IPC_EMPTY; /* EMPTY = spurious/timeout; caller retries */
}

/* Convenience: create a select set watching `endpoints[0..count)`. Avoids each
 * in-kernel service repeating the create + add-loop boilerplate. On failure the
 * partially-built set is destroyed. */
int ipc_select_listen(uint32_t owner_context_id, const uint32_t* endpoints, uint32_t count,
                      uint32_t* out_select_id) {
    uint32_t sel = 0;
    int rc;
    if (!endpoints || !out_select_id || count == 0) {
        return IPC_ERR_INVALID;
    }
    rc = ipc_select_create(owner_context_id, &sel);
    if (rc != IPC_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < count; ++i) {
        rc = ipc_select_add(sel, endpoints[i], owner_context_id);
        if (rc != IPC_OK) {
            ipc_select_destroy(sel, owner_context_id);
            return rc;
        }
    }
    *out_select_id = sel;
    return IPC_OK;
}

/* Convenience: block until any endpoint in the set has a message, then dequeue
 * it.  Returns IPC_OK with *out_endpoint / *out_message set, IPC_EMPTY on a
 * spurious wake or a lost race for the message (caller should loop), or an
 * error code.  This is the standard select-style receive loop body for
 * in-kernel services (memory-service, process-manager, ...). */
int ipc_select_recv(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_endpoint,
                    ipc_message_t* out_message, uint32_t timeout_ms) {
    uint32_t ready = IPC_ENDPOINT_NONE;
    int rc;
    if (!out_message) {
        return IPC_ERR_INVALID;
    }
    rc = ipc_select_wait(select_id, owner_context_id, &ready, timeout_ms);
    if (rc != IPC_OK) {
        return rc; /* IPC_EMPTY (spurious) or error */
    }
    if (out_endpoint) {
        *out_endpoint = ready;
    }
    return ipc_recv_for(owner_context_id, ready, out_message);
}

void ipc_select_destroy(uint32_t select_id, uint32_t owner_context_id) {
    ksync_spinlock_lock(&g_select_table_lock);
    ipc_select_t* sel = ipc_select_find(select_id, owner_context_id);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return;
    }
    /* Remove push watchers from all watched endpoints. */
    for (uint32_t i = 0; i < sel->ep_count; i++) {
        ipc_endpoint_t* ep = ipc_endpoint_get(sel->ep_ids[i]);
        if (ep) {
            if (ep->poll_struct) {
                poll_struct_remove(ep->poll_struct, sel);
            }
            ksync_spinlock_unlock(&ep->lock);
        }
    }
    /* Wake any blocked waiter with ABORT. */
    ksync_spinlock_lock(&sel->event.lock);
    sched_event_abort_all(&sel->event);
    ksync_spinlock_unlock(&sel->event.lock);
    sel->in_use = 0;
    sel->ep_count = 0;
    ksync_spinlock_unlock(&g_select_table_lock);
}

#ifdef WASMOS_IPC_TEST_SEAMS
void ipc_test_set_next_endpoint_id(uint32_t next_id, int wrapped) {
    ksync_spinlock_lock(&g_endpoint_table_lock);
    g_next_endpoint_id = next_id;
    g_endpoint_id_wrapped = wrapped ? 1u : 0u;
    ksync_spinlock_unlock(&g_endpoint_table_lock);
}

int ipc_test_set_notify_count(uint32_t endpoint, uint32_t value) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    ep->notify_count = value;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}
#endif

void ipc_select_signal(struct ipc_select* sel, uint32_t ep_id) {
    if (!sel) {
        return;
    }
    /* event.lock protects both ready_ep and the wait_list, matching
     * ipc_select_wait()'s critical section.  The old sel->lock is removed:
     * it was a separate spinlock that created the SMP lost-wakeup race. */
    ksync_spinlock_lock(&sel->event.lock);
    sel->ready_ep = ep_id;
    sched_event_wake_one(&sel->event, ep_id, SCHED_PEND_OK);
    ksync_spinlock_unlock(&sel->event.lock);
}
