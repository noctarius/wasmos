#include "ipc.h"
#include "list.h"
#include "process.h"
#include "thread.h"
#include "spinlock.h"
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
    spinlock_t lock;
    ipc_message_t queue[IPC_QUEUE_DEPTH];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t notify_count;
    sched_event_t event;       /* supports N waiters */
    poll_struct_t *poll_struct; /* lazily allocated; notified on send */
} ipc_endpoint_t;

/* Select-set: watches up to IPC_SELECT_EPS_MAX endpoints simultaneously. */
typedef struct ipc_select {
    uint32_t id;
    uint8_t  in_use;
    uint32_t owner_context_id;
    sched_event_t event;
    /* ready_ep is protected by event.lock, not a separate lock.  Keeping
     * event.lock as the single authority for both the wait_list and ready_ep
     * prevents the SMP lost-wakeup: ipc_select_signal() sets ready_ep and
     * calls sched_event_wake_one() under the same event.lock that
     * ipc_select_wait() holds across the "check ready_ep → add to wait_list"
     * critical section, so a signal can never slip between the check and the
     * enqueue. */
    uint32_t ready_ep;         /* endpoint that triggered the wake */
    uint32_t ep_ids[IPC_SELECT_EPS_MAX];
    uint32_t ep_count;
} ipc_select_t;

#define IPC_SELECT_TABLE_SIZE 32u
static ipc_select_t g_select_table[IPC_SELECT_TABLE_SIZE];
static spinlock_t   g_select_table_lock;

static list_t g_endpoint_table;
static spinlock_t g_endpoint_table_lock;
static uint32_t g_next_endpoint_id;

/*
 * Returns the endpoint with ep->lock held.  The caller must call
 * spinlock_unlock(&ep->lock) when done.  Lock order: g_endpoint_table_lock ->
 * ep->lock.  ep->lock is acquired under g_endpoint_table_lock so that
 * ipc_endpoints_release_owner cannot remove the endpoint between the lookup
 * and the caller's first use.
 */
static ipc_endpoint_t *
ipc_endpoint_get(uint32_t endpoint_id)
{
    list_iter_t it;
    if (endpoint_id == 0 || endpoint_id == IPC_ENDPOINT_NONE) {
        return 0;
    }
    spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t *ep = (ipc_endpoint_t *)list_first(&g_endpoint_table, &it);
    while (ep) {
        if (ep->id == endpoint_id && ep->in_use) {
            spinlock_lock(&ep->lock);
            spinlock_unlock(&g_endpoint_table_lock);
            return ep; /* returned with ep->lock held */
        }
        ep = (ipc_endpoint_t *)list_next(&it);
    }
    spinlock_unlock(&g_endpoint_table_lock);
    return 0;
}

/*
 * Reads the owner context ID of an endpoint under the table lock only.
 * Used by ipc_send_from for source permission checks that must not hold
 * ep->lock simultaneously (which would require a second nested endpoint lock).
 */
static uint32_t
ipc_endpoint_owner_context(uint32_t endpoint_id)
{
    list_iter_t it;
    if (endpoint_id == 0 || endpoint_id == IPC_ENDPOINT_NONE) {
        return 0;
    }
    spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t *ep = (ipc_endpoint_t *)list_first(&g_endpoint_table, &it);
    while (ep) {
        if (ep->id == endpoint_id && ep->in_use) {
            uint32_t ctx = ep->owner_context_id;
            spinlock_unlock(&g_endpoint_table_lock);
            return ctx;
        }
        ep = (ipc_endpoint_t *)list_next(&it);
    }
    spinlock_unlock(&g_endpoint_table_lock);
    return 0;
}

void ipc_init(void) {
    spinlock_init(&g_endpoint_table_lock);
    g_next_endpoint_id = 1;
    if (list_init(&g_endpoint_table, (uint32_t)sizeof(ipc_endpoint_t),
                  LIST_IMPL_ARRAY_CHUNK, IPC_ENDPOINT_TABLE_CHUNK) != 0) {
        for (;;) {}
    }
    spinlock_init(&g_select_table_lock);
    memset(g_select_table, 0, sizeof(g_select_table));
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        sched_event_init(&g_select_table[i].event, SCHED_EVENT_TYPE_SELECT);
    }
}

int ipc_endpoint_create(uint32_t owner_context_id, uint32_t *out_endpoint) {
    if (!out_endpoint) {
        return IPC_ERR_INVALID;
    }
    spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t *ep = (ipc_endpoint_t *)list_alloc(&g_endpoint_table);
    if (!ep) {
        spinlock_unlock(&g_endpoint_table_lock);
        return IPC_ERR_FULL;
    }
    ep->id = g_next_endpoint_id++;
    if (g_next_endpoint_id == IPC_ENDPOINT_NONE) {
        g_next_endpoint_id = 1;
    }
    ep->in_use = 1;
    ep->type = IPC_ENDPOINT_TYPE_MESSAGE;
    ep->owner_context_id = owner_context_id;
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->notify_count = 0;
    spinlock_init(&ep->lock);
    sched_event_init(&ep->event, SCHED_EVENT_TYPE_IPC);
    ep->poll_struct = 0;
    uint32_t id = ep->id;
    spinlock_unlock(&g_endpoint_table_lock);
    *out_endpoint = id;
    return IPC_OK;
}

int ipc_notification_create(uint32_t owner_context_id, uint32_t *out_endpoint) {
    if (!out_endpoint) {
        return IPC_ERR_INVALID;
    }
    spinlock_lock(&g_endpoint_table_lock);
    ipc_endpoint_t *ep = (ipc_endpoint_t *)list_alloc(&g_endpoint_table);
    if (!ep) {
        spinlock_unlock(&g_endpoint_table_lock);
        return IPC_ERR_FULL;
    }
    ep->id = g_next_endpoint_id++;
    if (g_next_endpoint_id == IPC_ENDPOINT_NONE) {
        g_next_endpoint_id = 1;
    }
    ep->in_use = 1;
    ep->type = IPC_ENDPOINT_TYPE_NOTIFICATION;
    ep->owner_context_id = owner_context_id;
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->notify_count = 0;
    spinlock_init(&ep->lock);
    sched_event_init(&ep->event, SCHED_EVENT_TYPE_IPC);
    ep->poll_struct = 0;
    uint32_t id = ep->id;
    spinlock_unlock(&g_endpoint_table_lock);
    *out_endpoint = id;
    return IPC_OK;
}

int ipc_endpoint_owner(uint32_t endpoint, uint32_t *out_owner_context_id) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (!out_owner_context_id) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    *out_owner_context_id = ep->owner_context_id;
    spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_endpoint_count(uint32_t endpoint, uint32_t *out_count) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (!out_count) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    *out_count = ep->count;
    spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t *message) {
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

    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_MESSAGE) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }

    if (ep->count >= IPC_QUEUE_DEPTH) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_FULL;
    }

    ipc_message_t msg = *message;
    msg.destination = endpoint;
    ep->queue[ep->tail] = msg;
    ep->tail = (ep->tail + 1u) % IPC_QUEUE_DEPTH;
    ep->count++;
    spinlock_lock(&ep->event.lock);
    sched_event_wake_one(&ep->event, 0, SCHED_PEND_OK);
    spinlock_unlock(&ep->event.lock);
    poll_struct_t *ps = ep->poll_struct;
    uint32_t ep_id = ep->id;
    spinlock_unlock(&ep->lock);
    if (ps) {
        poll_notify(ps, POLL_EV_IN, ep_id);
    }
    return IPC_OK;
}

int ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep || !out_message) {
        if (ep) {
            spinlock_unlock(&ep->lock);
        }
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_MESSAGE) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }

    if (receiver_context_id != IPC_CONTEXT_KERNEL &&
        ep->owner_context_id != receiver_context_id) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_PERM;
    }

    if (ep->count == 0) {
        /* Non-blocking poll must not register a waiter. On SMP a sender can
         * otherwise "wake" a thread that never actually blocked, turning a
         * still-running thread back into READY on another CPU. */
        spinlock_unlock(&ep->lock);
        return IPC_EMPTY;
    }

    *out_message = ep->queue[ep->head];
    ep->head = (ep->head + 1u) % IPC_QUEUE_DEPTH;
    ep->count--;
    spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_recv_blocking_for(uint32_t receiver_context_id, uint32_t endpoint,
                          ipc_message_t *out_message)
{
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep || !out_message) {
        if (ep) spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_MESSAGE) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    if (receiver_context_id != IPC_CONTEXT_KERNEL &&
        ep->owner_context_id != receiver_context_id) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_PERM;
    }
    if (ep->count == 0) {
        /* Block until a sender enqueues a message and wakes us. */
        spinlock_lock(&ep->event.lock);
        spinlock_unlock(&ep->lock);
        sched_event_wait(&ep->event, 0);
        ep = ipc_endpoint_get(endpoint);
        if (!ep) return IPC_ERR_INVALID;
        if (ep->count == 0) {
            spinlock_unlock(&ep->lock);
            return IPC_EMPTY;  /* spurious wake; caller should retry */
        }
    }
    *out_message = ep->queue[ep->head];
    ep->head = (ep->head + 1u) % IPC_QUEUE_DEPTH;
    ep->count--;
    spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_notify_from(uint32_t sender_context_id, uint32_t endpoint) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_NOTIFICATION) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    if (sender_context_id != IPC_CONTEXT_KERNEL &&
        sender_context_id != ep->owner_context_id) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_PERM;
    }

    if (ep->notify_count != UINT32_MAX) {
        ep->notify_count++;
    }
    spinlock_lock(&ep->event.lock);
    sched_event_wake_one(&ep->event, 0, SCHED_PEND_OK);
    spinlock_unlock(&ep->event.lock);
    spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_wait_for(uint32_t receiver_context_id, uint32_t endpoint) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep) {
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_NOTIFICATION) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_INVALID;
    }
    if (receiver_context_id != IPC_CONTEXT_KERNEL &&
        ep->owner_context_id != receiver_context_id) {
        spinlock_unlock(&ep->lock);
        return IPC_ERR_PERM;
    }

    if (ep->notify_count == 0) {
        /* Non-blocking notify poll must not arm event wake state for a thread
         * that is still running. */
        spinlock_unlock(&ep->lock);
        return IPC_EMPTY;
    }
    ep->notify_count--;
    spinlock_unlock(&ep->lock);
    return IPC_OK;
}

int ipc_send(uint32_t endpoint, const ipc_message_t *message) {
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, message);
}

int ipc_recv(uint32_t endpoint, ipc_message_t *out_message) {
    return ipc_recv_for(IPC_CONTEXT_KERNEL, endpoint, out_message);
}

int ipc_notify(uint32_t endpoint) {
    return ipc_notify_from(IPC_CONTEXT_KERNEL, endpoint);
}

int ipc_wait(uint32_t endpoint) {
    return ipc_wait_for(IPC_CONTEXT_KERNEL, endpoint);
}

void
ipc_endpoints_release_owner(uint32_t owner_context_id)
{
    if (owner_context_id == 0) {
        return;
    }
    spinlock_lock(&g_endpoint_table_lock);
    list_iter_t it;
    ipc_endpoint_t *ep = (ipc_endpoint_t *)list_first(&g_endpoint_table, &it);
    while (ep) {
        ipc_endpoint_t *next = (ipc_endpoint_t *)list_next(&it);
        if (ep->in_use && ep->owner_context_id == owner_context_id) {
            spinlock_lock(&ep->lock);
            if (ep->in_use && ep->owner_context_id == owner_context_id) {
                ep->in_use = 0;
                spinlock_lock(&ep->event.lock);
                sched_event_abort_all(&ep->event);
                spinlock_unlock(&ep->event.lock);
                if (ep->poll_struct) {
                    poll_struct_free(ep->poll_struct);
                    ep->poll_struct = 0;
                }
            }
            spinlock_unlock(&ep->lock);
            list_remove(&g_endpoint_table, ep);
        }
        ep = next;
    }
    spinlock_unlock(&g_endpoint_table_lock);
}

/* -------------------------------------------------------------------------
 * Select-set API
 * ------------------------------------------------------------------------- */

static ipc_select_t *
ipc_select_find(uint32_t select_id, uint32_t owner_context_id)
{
    /* Caller holds g_select_table_lock. */
    if (select_id == 0 || select_id > IPC_SELECT_TABLE_SIZE) {
        return 0;
    }
    ipc_select_t *sel = &g_select_table[select_id - 1u];
    if (!sel->in_use) {
        return 0;
    }
    if (owner_context_id != IPC_CONTEXT_KERNEL &&
        sel->owner_context_id != owner_context_id) {
        return 0;
    }
    return sel;
}

int
ipc_select_create(uint32_t owner_context_id, uint32_t *out_select_id)
{
    if (!out_select_id) {
        return IPC_ERR_INVALID;
    }
    spinlock_lock(&g_select_table_lock);
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        ipc_select_t *sel = &g_select_table[i];
        if (!sel->in_use) {
            sel->in_use           = 1;
            sel->owner_context_id = owner_context_id;
            sel->ready_ep         = IPC_ENDPOINT_NONE;
            sel->ep_count         = 0;
            sched_event_init(&sel->event, SCHED_EVENT_TYPE_SELECT);
            *out_select_id = i + 1u;
            spinlock_unlock(&g_select_table_lock);
            return IPC_OK;
        }
    }
    spinlock_unlock(&g_select_table_lock);
    return IPC_ERR_FULL;
}

int
ipc_select_add(uint32_t select_id, uint32_t endpoint_id,
               uint32_t owner_context_id)
{
    spinlock_lock(&g_select_table_lock);
    ipc_select_t *sel = ipc_select_find(select_id, owner_context_id);
    if (!sel) {
        spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_INVALID;
    }
    if (sel->ep_count >= IPC_SELECT_EPS_MAX) {
        spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_FULL;
    }
    sel->ep_ids[sel->ep_count++] = endpoint_id;

    /* Register push watcher on the endpoint. */
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint_id);
    if (ep) {
        if (!ep->poll_struct) {
            ep->poll_struct = poll_struct_alloc();
        }
        if (ep->poll_struct) {
            poll_struct_add(ep->poll_struct, POLL_EV_IN, sel, 0);
        }
        spinlock_unlock(&ep->lock);
    }

    spinlock_unlock(&g_select_table_lock);
    return IPC_OK;
}

int
ipc_select_wait(uint32_t select_id, uint32_t owner_context_id,
                uint32_t *out_ready_ep, uint32_t timeout_ms)
{
    if (!out_ready_ep) {
        return IPC_ERR_INVALID;
    }

    spinlock_lock(&g_select_table_lock);
    ipc_select_t *sel = ipc_select_find(select_id, owner_context_id);
    if (!sel) {
        spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_INVALID;
    }

    if (sel->ready_ep != IPC_ENDPOINT_NONE) {
        *out_ready_ep = sel->ready_ep;
        sel->ready_ep = IPC_ENDPOINT_NONE;
        spinlock_unlock(&g_select_table_lock);
        return IPC_OK;
    }

    /* Acquire event.lock BEFORE releasing g_select_table_lock.
     * ipc_select_signal() also holds event.lock when it writes ready_ep and
     * calls sched_event_wake_one(), so acquiring it here closes the SMP
     * lost-wakeup window: any signal that fires after this point is forced to
     * wait until we are already in the wait_list (inside sched_event_wait).
     * Without this ordering, signal() could set ready_ep and find an empty
     * wait_list in the gap between the ready_ep check above and the
     * sched_event_wait() call below. */
    spinlock_lock(&sel->event.lock);
    spinlock_unlock(&g_select_table_lock);

    /* Re-check under event.lock: if signal() fired between the ready_ep check
     * above and our acquisition of event.lock, ready_ep is already set. */
    if (sel->ready_ep != IPC_ENDPOINT_NONE) {
        *out_ready_ep = sel->ready_ep;
        sel->ready_ep = IPC_ENDPOINT_NONE;
        spinlock_unlock(&sel->event.lock);
        return IPC_OK;
    }

    /* sched_event_wait releases sel->event.lock before yielding. A non-zero
     * timeout_ms wakes us after the deadline (returning IPC_EMPTY). */
    sched_event_wait(&sel->event, timeout_ms);

    /* After wake: re-check under event.lock (same lock as ipc_select_signal). */
    spinlock_lock(&sel->event.lock);
    if (sel->ready_ep == IPC_ENDPOINT_NONE) {
        spinlock_unlock(&sel->event.lock);
        return IPC_EMPTY;  /* spurious wake; caller must retry */
    }
    *out_ready_ep = sel->ready_ep;
    sel->ready_ep = IPC_ENDPOINT_NONE;
    spinlock_unlock(&sel->event.lock);
    return IPC_OK;
}

/* Convenience: create a select set watching `endpoints[0..count)`. Avoids each
 * in-kernel service repeating the create + add-loop boilerplate. On failure the
 * partially-built set is destroyed. */
int
ipc_select_listen(uint32_t owner_context_id, const uint32_t *endpoints,
                  uint32_t count, uint32_t *out_select_id)
{
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
int
ipc_select_recv(uint32_t select_id, uint32_t owner_context_id,
                uint32_t *out_endpoint, ipc_message_t *out_message,
                uint32_t timeout_ms)
{
    uint32_t ready = IPC_ENDPOINT_NONE;
    int rc;
    if (!out_message) {
        return IPC_ERR_INVALID;
    }
    rc = ipc_select_wait(select_id, owner_context_id, &ready, timeout_ms);
    if (rc != IPC_OK) {
        return rc;   /* IPC_EMPTY (spurious) or error */
    }
    if (out_endpoint) {
        *out_endpoint = ready;
    }
    return ipc_recv_for(owner_context_id, ready, out_message);
}

void
ipc_select_destroy(uint32_t select_id, uint32_t owner_context_id)
{
    spinlock_lock(&g_select_table_lock);
    ipc_select_t *sel = ipc_select_find(select_id, owner_context_id);
    if (!sel) {
        spinlock_unlock(&g_select_table_lock);
        return;
    }
    /* Remove push watchers from all watched endpoints. */
    for (uint32_t i = 0; i < sel->ep_count; i++) {
        ipc_endpoint_t *ep = ipc_endpoint_get(sel->ep_ids[i]);
        if (ep) {
            if (ep->poll_struct) {
                poll_struct_remove(ep->poll_struct, sel);
            }
            spinlock_unlock(&ep->lock);
        }
    }
    /* Wake any blocked waiter with ABORT. */
    spinlock_lock(&sel->event.lock);
    sched_event_abort_all(&sel->event);
    spinlock_unlock(&sel->event.lock);
    sel->in_use   = 0;
    sel->ep_count = 0;
    spinlock_unlock(&g_select_table_lock);
}

void
ipc_select_signal(struct ipc_select *sel, uint32_t ep_id)
{
    if (!sel) {
        return;
    }
    /* event.lock protects both ready_ep and the wait_list, matching
     * ipc_select_wait()'s critical section.  The old sel->lock is removed:
     * it was a separate spinlock that created the SMP lost-wakeup race. */
    spinlock_lock(&sel->event.lock);
    sel->ready_ep = ep_id;
    sched_event_wake_one(&sel->event, ep_id, SCHED_PEND_OK);
    spinlock_unlock(&sel->event.lock);
}
