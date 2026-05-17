#include "ipc.h"
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

static ipc_endpoint_t g_endpoints[IPC_MAX_ENDPOINTS];

static inline uintptr_t
ipc_kernel_alias_addr(uintptr_t addr)
{
    uint64_t base = KERNEL_HIGHER_HALF_BASE;
    if ((uint64_t)addr < base) {
        return (uintptr_t)((uint64_t)addr + base);
    }
    return addr;
}

static inline ipc_endpoint_t *
ipc_endpoint_table(void)
{
    return (ipc_endpoint_t *)(void *)ipc_kernel_alias_addr((uintptr_t)&g_endpoints[0]);
}

static ipc_endpoint_t *ipc_endpoint_get(uint32_t endpoint) {
    ipc_endpoint_t *table = ipc_endpoint_table();
    if (endpoint >= IPC_MAX_ENDPOINTS) {
        return 0;
    }
    if (!table[endpoint].in_use) {
        return 0;
    }
    return &table[endpoint];
}

void ipc_init(void) {
    ipc_endpoint_t *table = ipc_endpoint_table();
    for (uint32_t i = 0; i < IPC_MAX_ENDPOINTS; ++i) {
        table[i].in_use = 0;
        table[i].type = IPC_ENDPOINT_TYPE_MESSAGE;
        table[i].owner_context_id = 0;
        spinlock_init(&table[i].lock);
        table[i].head = 0;
        table[i].tail = 0;
        table[i].count = 0;
        table[i].notify_count = 0;
        table[i].waiter_tid = 0;
    }
}

int ipc_endpoint_create(uint32_t owner_context_id, uint32_t *out_endpoint) {
    ipc_endpoint_t *table = ipc_endpoint_table();
    if (!out_endpoint) {
        return IPC_ERR_INVALID;
    }
    for (uint32_t i = 0; i < IPC_MAX_ENDPOINTS; ++i) {
        if (!table[i].in_use) {
            table[i].in_use = 1;
            table[i].type = IPC_ENDPOINT_TYPE_MESSAGE;
            table[i].owner_context_id = owner_context_id;
            table[i].head = 0;
            table[i].tail = 0;
            table[i].count = 0;
            table[i].notify_count = 0;
            table[i].waiter_tid = 0;
            *out_endpoint = i;
            return IPC_OK;
        }
    }
    return IPC_ERR_FULL;
}

int ipc_notification_create(uint32_t owner_context_id, uint32_t *out_endpoint) {
    ipc_endpoint_t *table = ipc_endpoint_table();
    if (!out_endpoint) {
        return IPC_ERR_INVALID;
    }
    for (uint32_t i = 0; i < IPC_MAX_ENDPOINTS; ++i) {
        if (!table[i].in_use) {
            table[i].in_use = 1;
            table[i].type = IPC_ENDPOINT_TYPE_NOTIFICATION;
            table[i].owner_context_id = owner_context_id;
            table[i].head = 0;
            table[i].tail = 0;
            table[i].count = 0;
            table[i].notify_count = 0;
            table[i].waiter_tid = 0;
            *out_endpoint = i;
            return IPC_OK;
        }
    }
    return IPC_ERR_FULL;
}

int ipc_endpoint_owner(uint32_t endpoint, uint32_t *out_owner_context_id) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep || !out_owner_context_id) {
        return IPC_ERR_INVALID;
    }
    *out_owner_context_id = ep->owner_context_id;
    return IPC_OK;
}

int ipc_endpoint_count(uint32_t endpoint, uint32_t *out_count) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep || !out_count) {
        return IPC_ERR_INVALID;
    }
    *out_count = ep->count;
    return IPC_OK;
}

int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t *message) {
    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint);
    if (!ep || !message) {
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_MESSAGE) {
        return IPC_ERR_INVALID;
    }

    /*
     * Endpoint ownership is the current capability boundary. A non-kernel sender
     * may only speak from endpoints owned by its context; the kernel is allowed
     * to originate messages without that restriction.
     */
    if (sender_context_id != IPC_CONTEXT_KERNEL) {
        if (message->source == IPC_ENDPOINT_NONE) {
            return IPC_ERR_PERM;
        }
        ipc_endpoint_t *source_ep = ipc_endpoint_get(message->source);
        if (!source_ep || source_ep->owner_context_id != sender_context_id) {
            return IPC_ERR_PERM;
        }
    }

    spinlock_lock(&ep->lock);
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
        return IPC_ERR_INVALID;
    }
    if (ep->type != IPC_ENDPOINT_TYPE_MESSAGE) {
        return IPC_ERR_INVALID;
    }

    if (receiver_context_id != IPC_CONTEXT_KERNEL &&
        ep->owner_context_id != receiver_context_id) {
        return IPC_ERR_PERM;
    }

    spinlock_lock(&ep->lock);
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
        return IPC_ERR_INVALID;
    }
    if (sender_context_id != IPC_CONTEXT_KERNEL &&
        sender_context_id != ep->owner_context_id) {
        return IPC_ERR_PERM;
    }

    spinlock_lock(&ep->lock);
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
        return IPC_ERR_INVALID;
    }
    if (receiver_context_id != IPC_CONTEXT_KERNEL &&
        ep->owner_context_id != receiver_context_id) {
        return IPC_ERR_PERM;
    }

    spinlock_lock(&ep->lock);
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
    ipc_endpoint_t *table = ipc_endpoint_table();
    if (owner_context_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < IPC_MAX_ENDPOINTS; ++i) {
        ipc_endpoint_t *ep = &table[i];
        if (!ep->in_use || ep->owner_context_id != owner_context_id) {
            continue;
        }
        spinlock_lock(&ep->lock);
        ep->in_use = 0;
        ep->type = IPC_ENDPOINT_TYPE_MESSAGE;
        ep->owner_context_id = 0;
        ep->head = 0;
        ep->tail = 0;
        ep->count = 0;
        ep->notify_count = 0;
        ep->waiter_tid = 0;
        spinlock_unlock(&ep->lock);
    }
}
