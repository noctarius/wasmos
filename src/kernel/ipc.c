#include "idtable.h"
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
#include "kpanic.h"

/*
 * The kernel IPC layer keeps transport deliberately small: fixed-size endpoint
 * tables, bounded queues, context ownership checks, and optional notification
 * counters. Higher-level protocols are built entirely in drivers and services.
 */

typedef struct {
    /* First, as idtable requires: id, owner_context_id and in_use live here. */
    idtable_header_t header;
    ipc_endpoint_type_t type;
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
    /* First, as idtable requires: id, owner_context_id and in_use live here. */
    idtable_header_t header;
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

static idtable_t g_select_table;
static ksync_spinlock_t g_select_table_lock;

static idtable_t g_endpoint_table;
static ksync_spinlock_t g_endpoint_table_lock;

/* Describe the wait a blocked thread is sitting in, from its event alone.
 *
 * A thread's wait_event points INTO the object that owns it -- an endpoint's or
 * a select-set's embedded sched_event_t -- and sched_event_t::type says which,
 * so the owner is recoverable without walking the id table.  That matters
 * because the caller is the NMI diagnostic path, which must take no locks.
 *
 * Fills out_id with the endpoint or select id and out_count with the endpoint's
 * queued-message count (0 for a select).  For a select set, up to `watch_max`
 * watched endpoint ids and their queue counts are written to out_watch_ids /
 * out_watch_counts and the number written is returned through out_watched.
 *
 * Returns IPC_DIAG_WAIT_ENDPOINT, IPC_DIAG_WAIT_SELECT, or -1 when the event is
 * neither (a join, futex, mutex or timer wait).
 *
 * The point of the queue count: a blocked owner whose endpoint holds a message
 * is a lost wake -- the send happened and the receiver was never made runnable.
 * An empty queue everywhere means nobody sent, which is a deadlock instead. */
int ipc_diag_wait_info(const void* event, uint32_t* out_id, uint32_t* out_count,
                       uint32_t* out_owner, uint32_t watch_max, uint32_t* out_watch_ids,
                       uint32_t* out_watch_counts, uint32_t* out_watched) {
    const sched_event_t* ev = (const sched_event_t*)event;
    if (out_watched) {
        *out_watched = 0;
    }
    if (!ev) {
        return -1;
    }
    if (ev->type == SCHED_EVENT_TYPE_IPC) {
        const ipc_endpoint_t* ep =
            (const ipc_endpoint_t*)(const void*)((const char*)ev - offsetof(ipc_endpoint_t, event));
        if (out_id) {
            *out_id = ep->header.id;
        }
        if (out_count) {
            *out_count = ep->count;
        }
        if (out_owner) {
            *out_owner = ep->header.owner_context_id;
        }
        return IPC_DIAG_WAIT_ENDPOINT;
    }
    if (ev->type == SCHED_EVENT_TYPE_SELECT) {
        const ipc_select_t* sel =
            (const ipc_select_t*)(const void*)((const char*)ev - offsetof(ipc_select_t, event));
        if (out_id) {
            *out_id = sel->header.id;
        }
        if (out_count) {
            *out_count = 0;
        }
        if (out_owner) {
            *out_owner = sel->header.owner_context_id;
        }
        uint32_t written = 0;
        for (uint32_t i = 0; i < sel->ep_count && written < watch_max; ++i) {
            uint32_t id = sel->ep_ids[i];
            if (id == IPC_ENDPOINT_NONE) {
                continue;
            }
            const ipc_endpoint_t* ep = (const ipc_endpoint_t*)idtable_get(&g_endpoint_table, id);
            if (out_watch_ids) {
                out_watch_ids[written] = id;
            }
            if (out_watch_counts) {
                out_watch_counts[written] = ep ? ep->count : 0u;
            }
            written++;
        }
        if (out_watched) {
            *out_watched = written;
        }
        return IPC_DIAG_WAIT_SELECT;
    }
    return -1;
}

/*
 * Returns the endpoint with ep->lock held.  The caller must call
 * ksync_spinlock_unlock(&ep->lock) when done.  Lock order: g_endpoint_table_lock ->
 * ep->lock.  ep->lock is acquired under g_endpoint_table_lock so that
 * ipc_endpoints_release_owner cannot remove the endpoint between the lookup
 * and the caller's first use.
 */
static ipc_endpoint_t* ipc_endpoint_get(uint32_t endpoint_id) {
    if (endpoint_id == 0 || endpoint_id == IPC_ENDPOINT_NONE) {
        return 0;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t* ep = (ipc_endpoint_t*)idtable_get(&g_endpoint_table, endpoint_id);
    if (ep) {
        /* ep->lock is taken while the table lock is still held, which is the
         * ordering that stops a release from removing the endpoint between the
         * lookup and the caller's first use. idtable does not do this for us on
         * purpose: the order is this file's to decide. */
        ksync_spinlock_lock(&ep->lock);
    }
    ksync_spinlock_unlock(&g_endpoint_table_lock);
    return ep; /* returned with ep->lock held, or NULL */
}

/*
 * Reads the owner context ID of an endpoint under the table lock only.
 * Used by ipc_send_from for source permission checks that must not hold
 * ep->lock simultaneously (which would require a second nested endpoint lock).
 */
static uint32_t ipc_endpoint_owner_context(uint32_t endpoint_id) {
    if (endpoint_id == 0 || endpoint_id == IPC_ENDPOINT_NONE) {
        return 0;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t* ep = (ipc_endpoint_t*)idtable_get(&g_endpoint_table, endpoint_id);
    const uint32_t ctx = ep ? ep->header.owner_context_id : 0u;
    ksync_spinlock_unlock(&g_endpoint_table_lock);
    return ctx;
}

/* Brings both object tables up.  Call once, before any other entry point in this
 * file; a table that fails to initialise is a kpanic rather than an error return
 * because every IPC path below would otherwise dereference an unbuilt store. */
void ipc_init(void) {
    ksync_spinlock_init(&g_endpoint_table_lock);
    /* idtable owns id allocation (including skipping live ids after a wrap),
     * the per-context quota and release-by-owner; see
     * docs/architecture/35-kernel-object-tables.md. The table lock stays here,
     * because the ordering below it -- table lock, then ep->lock -- is this
     * file's to decide. */
    if (idtable_init(&g_endpoint_table,
                     (uint32_t)sizeof(ipc_endpoint_t),
                     IPC_ENDPOINT_TABLE_CHUNK,
                     IPC_ENDPOINT_PER_CONTEXT_MAX) != WASMOS_OK) {
        kpanic("ipc: endpoint table init failed",
               (uint64_t)sizeof(ipc_endpoint_t),
               IPC_ENDPOINT_TABLE_CHUNK);
    }
    ksync_spinlock_init(&g_select_table_lock);
    /* Grows on demand, so the number of parked services is bounded by memory and
     * the per-context quota rather than by a fixed table size. */
    if (idtable_init(&g_select_table,
                     (uint32_t)sizeof(ipc_select_t),
                     IPC_SELECT_TABLE_CHUNK,
                     IPC_SELECT_PER_CONTEXT_MAX) != WASMOS_OK) {
        kpanic("ipc: select table init failed",
               (uint64_t)sizeof(ipc_select_t),
               IPC_SELECT_TABLE_CHUNK);
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
        *out_rc = IPC_ERR_NOENT;
        return 0;
    }
    if (ep->type != type) {
        ksync_spinlock_unlock(&ep->lock);
        *out_rc = IPC_ERR_UNSUPPORTED;
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
    if (context_id != IPC_CONTEXT_KERNEL && ep->header.owner_context_id != context_id) {
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
    /* Both endpoint kinds come from this table, so the per-context quota covers
     * both and a context cannot get a second allowance by asking for
     * notifications. */
    int status = WASMOS_OK;
    ipc_endpoint_t* ep =
        (ipc_endpoint_t*)idtable_alloc(&g_endpoint_table, owner_context_id, &status);
    if (!ep) {
        ksync_spinlock_unlock(&g_endpoint_table_lock);
        return status == WASMOS_INVAL ? IPC_ERR_INVALID : IPC_ERR_FULL;
    }
    ep->type = type;
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->notify_count = 0;
    ksync_spinlock_init(&ep->lock);
    sched_event_init(&ep->event, SCHED_EVENT_TYPE_IPC);
    ep->poll_struct = 0;
    uint32_t id = ep->header.id;
    ksync_spinlock_unlock(&g_endpoint_table_lock);
    *out_endpoint = id;
    return IPC_OK;
}

int ipc_endpoint_create(uint32_t owner_context_id, uint32_t* out_endpoint) {
    return ipc_endpoint_create_typed(owner_context_id, IPC_ENDPOINT_TYPE_MESSAGE, out_endpoint);
}

int ipc_notification_create(uint32_t owner_context_id, uint32_t* out_endpoint) {
    return ipc_endpoint_create_typed(
        owner_context_id, IPC_ENDPOINT_TYPE_NOTIFICATION, out_endpoint);
}

/* Reports who owns `endpoint`.  IPC_OK with *out_owner_context_id set,
 * IPC_ERR_NOENT if the id does not resolve, IPC_ERR_INVALID for a NULL out
 * pointer.  Deliberately performs NO ownership check of its own — resolving an
 * endpoint's owner is how callers (syscall reply authentication, PM handlers)
 * establish identity, so refusing non-owners would defeat the purpose.  Unlike
 * the static ipc_endpoint_owner_context this takes ep->lock, so it must not be
 * called with an endpoint lock already held. */
int ipc_endpoint_owner(uint32_t endpoint, uint32_t* out_owner_context_id) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_NOENT;
    }
    if (!out_owner_context_id) {
        ksync_spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    *out_owner_context_id = ep->header.owner_context_id;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

/* Queue depth of `endpoint` right now, in messages (0..IPC_QUEUE_DEPTH).  A
 * snapshot: it is stale as soon as ep->lock is dropped, so acting on a non-zero
 * answer with a receive that assumes a message is there is a race — the receive
 * primitives already report IPC_EMPTY for that.  Type is not checked, so a
 * notification endpoint answers 0 (its counter is notify_count, not count). */
int ipc_endpoint_count(uint32_t endpoint, uint32_t* out_count) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_NOENT;
    }
    if (!out_count) {
        ksync_spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    *out_count = ep->count;
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

/* Enqueues a COPY of `message` on `endpoint` and wakes at most one parked
 * receiver plus every poll watcher.  Never blocks: a full queue is refused with
 * IPC_ERR_FULL rather than waiting, so back-pressure is the sender's problem.
 *
 * `message` is borrowed for the duration of the call.  message->destination is
 * overwritten with `endpoint` in the stored copy; message->source is the
 * caller's claimed reply endpoint and is what the permission check below
 * validates.  Any context may send to any message endpoint — send is the one
 * operation without an ownership requirement on the destination — so the
 * spoofing defence is entirely on the source side. */
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
     * freed watcher nodes.  The file-wide lock order is
     * g_select_table_lock -> g_endpoint_table_lock -> ep->lock -> an event lock
     * (ep->event.lock or, via poll_notify -> ipc_select_signal, sel->event.lock),
     * so holding ep->lock across the notify is consistent with every path. */
    if (ep->poll_struct) {
        poll_notify(ep->poll_struct, POLL_EV_IN, ep->header.id);
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
        /* Hand off from ep->lock to ep->event.lock without a gap, then block:
         * a sender must not be able to enqueue and wake between the emptiness
         * test and this thread joining the wait list. */
        ksync_spinlock_lock(&ep->event.lock);
        ksync_spinlock_unlock(&ep->lock);
        sched_event_wait(&ep->event, 0);
        ep = ipc_endpoint_get(endpoint);
        if (!ep) {
            /* Released while the caller was parked -- distinct from a bad
             * endpoint name: the handle WAS valid when the caller blocked. */
            return IPC_ERR_PEER_GONE;
        }
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
    ipc_endpoint_t* ep = ipc_endpoint_acquire_owned(
        endpoint, IPC_ENDPOINT_TYPE_NOTIFICATION, sender_context_id, &rc);
    if (!ep) {
        return rc;
    }

    if (ep->notify_count != UINT32_MAX) {
        ep->notify_count++;
    }
    /*
     * Signal the poll hub, exactly as a message send does.  ipc_select_add takes
     * any endpoint that resolves, so a set may watch a notification endpoint,
     * and this is the only path by which such a set is ever woken.
     *
     * Nothing can be parked on a notification endpoint's own sched_event: both
     * blocking receives demand a MESSAGE endpoint, and ipc_wait_for polls and
     * returns IPC_EMPTY rather than blocking.  A blocking notification wait, if
     * one is ever added, belongs alongside this notify -- not instead of it.
     *
     * Held under ep->lock for the same reason ipc_send_from does: notifying
     * after the unlock would race ipc_endpoints_release_owner freeing the hub.
     */
    if (ep->poll_struct) {
        poll_notify(ep->poll_struct, POLL_EV_IN, ep->header.id);
    }
    ksync_spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint) {
    int rc = IPC_OK;
    ipc_endpoint_t* ep = ipc_endpoint_acquire_owned(
        endpoint, IPC_ENDPOINT_TYPE_NOTIFICATION, receiver_context_id, &rc);
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

/*
 * What an endpoint owns, undone. Runs from idtable_release_owner with the table
 * lock held and the element still intact: waiters must be woken (they are
 * blocked on an endpoint that is about to stop existing) and the lazily
 * allocated poll hub returned.
 */
static void ipc_endpoint_teardown(void* elem, void* user) {
    ipc_endpoint_t* ep = (ipc_endpoint_t*)elem;
    (void)user;
    ksync_spinlock_lock(&ep->lock);
    ksync_spinlock_lock(&ep->event.lock);
    sched_event_abort_all(&ep->event);
    ksync_spinlock_unlock(&ep->event.lock);
    if (ep->poll_struct) {
        poll_struct_free(ep->poll_struct);
        ep->poll_struct = 0;
    }
    ksync_spinlock_unlock(&ep->lock);
}

/* Releases every endpoint owned by `owner_context_id`, running the teardown
 * above on each.  Called from the process reap path, so the owner is already
 * dead and cannot race its own endpoints.  Peers are not notified: a thread
 * blocked on one of these endpoints is aborted by the teardown and its
 * ipc_recv_blocking_for re-resolve then answers IPC_ERR_PEER_GONE.  Context 0 is
 * ignored, since it is the "no context" sentinel and matching on it would
 * release endpoints indiscriminately. */
void ipc_endpoints_release_owner(uint32_t owner_context_id) {
    if (owner_context_id == 0) {
        return;
    }
    ksync_spinlock_lock(&g_endpoint_table_lock);
    (void)idtable_release_owner(&g_endpoint_table, owner_context_id, ipc_endpoint_teardown, 0);
    ksync_spinlock_unlock(&g_endpoint_table_lock);
}

/* -------------------------------------------------------------------------
 * Select-set API
 * ------------------------------------------------------------------------- */

/*
 * Caller holds g_select_table_lock.  *out_rc separates the two reasons a lookup
 * fails: there is no such set (IPC_ERR_NOENT) versus there is one and it
 * belongs to somebody else (IPC_ERR_PERM).  The distinction is what tells a
 * caller whether re-resolving the handle could ever help.
 */
static ipc_select_t* ipc_select_find(uint32_t select_id, uint32_t owner_context_id, int* out_rc) {
    ipc_select_t* sel = (ipc_select_t*)idtable_get(&g_select_table, select_id);
    if (!sel) {
        *out_rc = IPC_ERR_NOENT;
        return 0;
    }
    if (owner_context_id != IPC_CONTEXT_KERNEL &&
        sel->header.owner_context_id != owner_context_id) {
        *out_rc = IPC_ERR_PERM;
        return 0;
    }
    *out_rc = IPC_OK;
    return sel;
}

/* Allocates an empty select set owned by `owner_context_id`.  IPC_OK with
 * *out_select_id set, IPC_ERR_INVALID for a NULL out pointer or a context the
 * store rejects, IPC_ERR_FULL when the per-context quota is reached or the store
 * cannot grow.  The set watches nothing until ipc_select_add is called, and a
 * wait on an empty set therefore blocks until its timeout.  The caller owns the
 * id and must ipc_select_destroy it; process teardown does not sweep select
 * sets the way it sweeps endpoints. */
int ipc_select_create(uint32_t owner_context_id, uint32_t* out_select_id) {
    if (!out_select_id) {
        return IPC_ERR_INVALID;
    }
    ksync_spinlock_lock(&g_select_table_lock);
    /* The per-context quota is the component's: a context at its ceiling is
     * refused before the store is asked to grow, so it cannot take the memory
     * another context would have used. */
    int status = WASMOS_OK;
    ipc_select_t* sel = (ipc_select_t*)idtable_alloc(&g_select_table, owner_context_id, &status);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return status == WASMOS_INVAL ? IPC_ERR_INVALID : IPC_ERR_FULL;
    }
    sel->ready_ep = IPC_ENDPOINT_NONE;
    sel->ep_count = 0;
    sched_event_init(&sel->event, SCHED_EVENT_TYPE_SELECT);
    *out_select_id = sel->header.id;
    ksync_spinlock_unlock(&g_select_table_lock);
    return IPC_OK;
}

int ipc_select_add(uint32_t select_id, uint32_t endpoint_id, uint32_t owner_context_id) {
    int rc = IPC_OK;
    ksync_spinlock_lock(&g_select_table_lock);
    ipc_select_t* sel = ipc_select_find(select_id, owner_context_id, &rc);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return rc;
    }
    /*
     * Adding an endpoint the set already watches is a no-op, not a second
     * registration.  Callers routinely build a set from several handles that
     * can legitimately coincide -- a service whose reply and event endpoints
     * are the same -- so refusing would fail them at startup for a harmless
     * call.  Registering twice is worse than refusing: it burns one of only
     * IPC_SELECT_EPS_MAX watch slots and leaves two poll watchers, so a single
     * send runs ipc_select_signal twice (invisibly, since ready_ep is a latch
     * that swallows the second signal).
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
     * An endpoint_id that does not resolve is refused rather than recorded, for
     * the same reason: recording it yields the same silent-never-ready set,
     * with the caller told the endpoint is watched.  Reachable from a guest --
     * WARP's hostcall shim casts a negative handle to IPC_ENDPOINT_NONE and
     * passes it through (tests/unit/test_hostcall_ipc.cpp covers it).
     */
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint_id);
    if (!ep) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_NOENT;
    }
    if (!ep->poll_struct) {
        ep->poll_struct = poll_struct_alloc();
    }
    if (!ep->poll_struct || poll_struct_add(ep->poll_struct, POLL_EV_IN, sel, 0) != 0) {
        ksync_spinlock_unlock(&ep->lock);
        ksync_spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_FULL;
    }
    ksync_spinlock_unlock(&ep->lock);

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

    int find_rc = IPC_OK;
    ksync_spinlock_lock(&g_select_table_lock);
    ipc_select_t* sel = ipc_select_find(select_id, owner_context_id, &find_rc);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return find_rc;
    }

    /* Acquire event.lock BEFORE releasing g_select_table_lock.  It does two
     * jobs at once, and both are required.
     *
     * It closes the SMP lost-wakeup window around the block: ipc_select_signal
     * holds event.lock while it writes ready_ep and calls
     * sched_event_wake_one, so any signal firing after this point must wait
     * until this thread is already in the wait_list inside sched_event_wait.
     *
     * It also puts the take-and-clear below on the same lock the signal writes
     * under.  Reading ready_ep under g_select_table_lock instead races a
     * concurrent signal and can overwrite the endpoint that signal just
     * published with IPC_ENDPOINT_NONE, dropping that readiness entirely: the set then blocks until
     * some *later* message arrives, and the queued one is never looked at. */
    ksync_spinlock_lock(&sel->event.lock);
    ksync_spinlock_unlock(&g_select_table_lock);

    if (ipc_select_take_ready(sel, out_ready_ep)) {
        ksync_spinlock_unlock(&sel->event.lock);
        return IPC_OK;
    }

    /* sched_event_wait releases sel->event.lock before yielding. A non-zero
     * timeout_ms wakes the caller after the deadline (returning IPC_EMPTY). */
    sched_event_wait(&sel->event, timeout_ms);

    /* `sel` cannot be trusted after the wake. A concurrent destroy wakes its
     * waiters and then RELEASES the set's storage, so the parked-on pointer may
     * be freed or reused. Re-resolve by id under the table lock; gone means
     * IPC_EMPTY, the same answer a timeout gives, because there is nothing to
     * report either way.
     *
     * Re-resolving by id goes through ipc_select_find, which re-checks the owner
     * context.  idtable only recycles a freed id after the id space has wrapped,
     * and even then the owner check is what guarantees this waiter cannot be
     * handed a set that now belongs to somebody else. */
    ksync_spinlock_lock(&g_select_table_lock);
    sel = ipc_select_find(select_id, owner_context_id, &find_rc);
    if (!sel) {
        ksync_spinlock_unlock(&g_select_table_lock);
        return IPC_EMPTY;
    }
    ksync_spinlock_lock(&sel->event.lock);
    ksync_spinlock_unlock(&g_select_table_lock);
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

/* Unregisters the set from every endpoint it watches, aborts anyone parked on
 * it, and frees the id.  Silent on failure by design — there is nothing a caller
 * could do about "the set was already gone" or "it is not yours", and both leave
 * the system in the state the caller wanted.
 *
 * Takes g_select_table_lock and then, per watched endpoint, ep->lock via
 * ipc_endpoint_get: the file-wide order (see ipc_send_from), so it must not be
 * called with an endpoint lock held.  A waiter woken by the abort here re-
 * resolves the id under the table lock and finds it gone; that re-resolve is why
 * the free is safe while a thread still holds a stale `sel` pointer. */
void ipc_select_destroy(uint32_t select_id, uint32_t owner_context_id) {
    int rc = IPC_OK;
    ksync_spinlock_lock(&g_select_table_lock);
    ipc_select_t* sel = ipc_select_find(select_id, owner_context_id, &rc);
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
    sel->ep_count = 0;
    (void)idtable_free(&g_select_table, select_id);
    ksync_spinlock_unlock(&g_select_table_lock);
}

#ifdef WASMOS_IPC_TEST_SEAMS
void ipc_test_set_next_endpoint_id(uint32_t next_id, int wrapped) {
    ksync_spinlock_lock(&g_endpoint_table_lock);
    idtable_test_set_next_id(&g_endpoint_table, next_id, wrapped);
    ksync_spinlock_unlock(&g_endpoint_table_lock);
}

int ipc_test_set_notify_count(uint32_t endpoint, uint32_t value) {
    ipc_endpoint_t* ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_NOENT;
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
     * ipc_select_wait()'s critical section.  A separate lock for ready_ep would
     * reopen the SMP lost-wakeup window described on the struct. */
    ksync_spinlock_lock(&sel->event.lock);
    sel->ready_ep = ep_id;
    sched_event_wake_one(&sel->event, ep_id, SCHED_PEND_OK);
    ksync_spinlock_unlock(&sel->event.lock);
}
