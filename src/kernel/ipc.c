#include "ipc.h"
#include "list.h"
#include "process.h"
#include "thread.h"
#include "spinlock.h"
#include "paging.h"

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
    uint32_t waiter_tid;
} ipc_endpoint_t;

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
    ep->waiter_tid = 0;
    spinlock_init(&ep->lock);
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
    ep->waiter_tid = 0;
    spinlock_init(&ep->lock);
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
    uint32_t waiter_tid = ep->waiter_tid;
    ep->waiter_tid = 0;
    spinlock_unlock(&ep->lock);
    /* Strict thread-targeted wake: payload remains queued if no waiter thread
     * is currently blocked on this endpoint. */
    if (waiter_tid != 0) {
        (void)process_wake_thread(waiter_tid);
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
        ep->waiter_tid = thread_current_tid();
        spinlock_unlock(&ep->lock);
        return IPC_EMPTY;
    }

    *out_message = ep->queue[ep->head];
    ep->head = (ep->head + 1u) % IPC_QUEUE_DEPTH;
    ep->count--;
    ep->waiter_tid = 0;
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
    uint32_t waiter_tid = ep->waiter_tid;
    ep->waiter_tid = 0;
    spinlock_unlock(&ep->lock);
    /* Strict thread-targeted wake: notification count is retained if no waiter
     * is currently blocked on this endpoint. */
    if (waiter_tid != 0) {
        (void)process_wake_thread(waiter_tid);
    }
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
        ep->waiter_tid = thread_current_tid();
        spinlock_unlock(&ep->lock);
        return IPC_EMPTY;
    }
    ep->notify_count--;
    ep->waiter_tid = 0;
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
                ep->waiter_tid = 0;
            }
            spinlock_unlock(&ep->lock);
            list_remove(&g_endpoint_table, ep);
        }
        ep = next;
    }
    spinlock_unlock(&g_endpoint_table_lock);
}
