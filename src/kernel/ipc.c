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

/* --- Select set table ---
 *
 * No back-links are stored in ipc_endpoint_t to keep the hot-path struct
 * small.  Instead, senders check g_active_select_count first; if non-zero,
 * they scan the (small, fixed) select table for sets that watch the endpoint
 * that was just written.  g_active_select_count == 0 ⟹ fast return. */

#define IPC_SELECT_TABLE_SIZE 32

typedef struct {
    uint32_t   id;               /* unique ID; 0 = slot unused */
    uint8_t    in_use;
    uint32_t   owner_context_id;
    spinlock_t lock;
    uint32_t   waiter_tid;       /* thread blocked in ipc_select_wait, 0 if none */
    uint32_t   ready_ep;         /* endpoint that triggered the wake */
    uint8_t    ready;            /* set by ipc_select_signal before waking waiter */
    uint32_t   ep_ids[IPC_SELECT_EPS_MAX];
    uint32_t   ep_count;
} ipc_select_t;

/* Endpoint table lives first to preserve its BSS addresses (and therefore
 * cache-line layout) relative to the kernel's original ipc.c object file.
 * The select-set globals are appended after so the hot endpoint path is
 * unaffected by the added 2 KB of select-set data. */
static list_t g_endpoint_table;
static spinlock_t g_endpoint_table_lock;
static uint32_t g_next_endpoint_id;

static ipc_select_t g_select_table[IPC_SELECT_TABLE_SIZE];
static spinlock_t   g_select_table_lock;
static uint32_t     g_next_select_id;
/* Count of currently-alive select sets.  Senders skip the table scan when
 * this is 0, keeping the common no-select path at a single atomic load. */
static volatile uint32_t g_active_select_count;

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

/* --- Select set helpers (used by endpoint send/notify paths) --- */

/* Find a select set by ID and return it with sel->lock held. */
static ipc_select_t *
ipc_select_find_locked(uint32_t select_id)
{
    if (select_id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        if (g_select_table[i].id == select_id) {
            spinlock_lock(&g_select_table[i].lock);
            if (g_select_table[i].id == select_id && g_select_table[i].in_use) {
                return &g_select_table[i];
            }
            spinlock_unlock(&g_select_table[i].lock);
        }
    }
    return 0;
}

/* Scan all active select sets for those watching ep_id and wake their
 * waiters.  Called from ipc_send_from / ipc_notify_from after releasing
 * ep->lock.  Only called when g_active_select_count > 0. */
static void
ipc_select_signal_ep(uint32_t ep_id)
{
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        /* Fast skip without lock: if in_use is 0 there is nothing to do.
         * The lock re-check inside handles any transient race. */
        if (!g_select_table[i].in_use) {
            continue;
        }
        spinlock_lock(&g_select_table[i].lock);
        ipc_select_t *sel = &g_select_table[i];
        if (!sel->in_use) {
            spinlock_unlock(&sel->lock);
            continue;
        }
        /* Check if this select set is watching ep_id. */
        uint8_t watching = 0;
        for (uint32_t j = 0; j < sel->ep_count; j++) {
            if (sel->ep_ids[j] == ep_id) {
                watching = 1;
                break;
            }
        }
        if (!watching) {
            spinlock_unlock(&sel->lock);
            continue;
        }
        if (!sel->ready) {
            sel->ready = 1;
            sel->ready_ep = ep_id;
        }
        uint32_t wtid = sel->waiter_tid;
        sel->waiter_tid = 0;
        spinlock_unlock(&sel->lock);
        if (wtid != 0) {
            (void)process_wake_thread(wtid);
        }
    }
}

void ipc_init(void) {
    spinlock_init(&g_endpoint_table_lock);
    g_next_endpoint_id = 1;
    spinlock_init(&g_select_table_lock);
    g_next_select_id = 1;
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        spinlock_init(&g_select_table[i].lock);
    }
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
    uint32_t ep_id_val = ep->id;
    spinlock_unlock(&ep->lock);
    /* Strict thread-targeted wake: payload remains queued if no waiter thread
     * is currently blocked on this endpoint. */
    if (waiter_tid != 0) {
        (void)process_wake_thread(waiter_tid);
    }
    /* Signal any select sets watching this endpoint.  The active-count guard
     * keeps the common no-select path at a single atomic load. */
    if (__atomic_load_n(&g_active_select_count, __ATOMIC_ACQUIRE) > 0) {
        ipc_select_signal_ep(ep_id_val);
    }
    return IPC_OK;
}

static int
ipc_recv_for_impl(uint32_t receiver_context_id,
                  uint32_t endpoint,
                  ipc_message_t *out_message,
                  uint8_t arm_waiter)
{
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
        if (arm_waiter) {
            ep->waiter_tid = thread_current_tid();
        }
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

int
ipc_try_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message)
{
    return ipc_recv_for_impl(receiver_context_id, endpoint, out_message, 0);
}

int
ipc_recv_for(uint32_t receiver_context_id, uint32_t endpoint, ipc_message_t *out_message)
{
    return ipc_recv_for_impl(receiver_context_id, endpoint, out_message, 1);
}

int
ipc_recv_blocking_for(uint32_t receiver_context_id,
                      uint32_t endpoint,
                      ipc_message_t *out_message)
{
    for (;;) {
        /* Fast path: poll without arming or blocking. */
        int rc = ipc_try_recv_for(receiver_context_id, endpoint, out_message);
        if (rc != IPC_EMPTY) {
            return rc;
        }

        process_t *proc = process_get(process_current_pid());
        if (!proc) {
            return IPC_ERR_INVALID;
        }

        /*
         * Arm the waiter AND mark the process BLOCKED while ep->lock is held.
         * Lock order: g_endpoint_table_lock → ep->lock → g_thread_table_lock.
         *
         * The fast poll above released ep->lock before returning.  We re-lock
         * here explicitly — acquiring ep->lock while the table lock is still
         * held (then releasing the table lock) — so that ipc_endpoints_release_owner
         * cannot free the entry between the lookup and our lock.
         *
         * Fix: call process_block_on_ipc() before releasing ep->lock.  Any
         * sender that acquires ep->lock after this point sees both waiter_tid
         * set and the thread already BLOCKED, so process_wake_thread() is
         * guaranteed to succeed.  process_block_on_ipc() is safe to call
         * here: its hot path only touches per-CPU fields then calls
         * thread_set_state, which acquires g_thread_table_lock — a leaf lock
         * that nothing holds before taking ep->lock, so no cycle is possible.
         */
        ipc_endpoint_t *ep = 0;
        {
            list_iter_t eit;
            spinlock_lock(&g_endpoint_table_lock);
            ipc_endpoint_t *cur = (ipc_endpoint_t *)list_first(&g_endpoint_table, &eit);
            while (cur) {
                if (cur->id == endpoint && cur->in_use) {
                    ep = cur;
                    break;
                }
                cur = (ipc_endpoint_t *)list_next(&eit);
            }
            if (ep) {
                spinlock_lock(&ep->lock);
            }
            spinlock_unlock(&g_endpoint_table_lock);
        }
        if (!ep) {
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
        if (ep->count > 0) {
            /* Message arrived between the poll above and now. */
            *out_message = ep->queue[ep->head];
            ep->head = (ep->head + 1u) % IPC_QUEUE_DEPTH;
            ep->count--;
            ep->waiter_tid = 0;
            spinlock_unlock(&ep->lock);
            return IPC_OK;
        }
        /* Queue empty: arm and block atomically under the lock. */
        uint32_t my_tid = thread_current_tid();
        thread_t *thread = thread_get(my_tid);
        ep->waiter_tid = my_tid;
        /*
         * Set blocking_transition BEFORE calling process_block_on_ipc.
         * Once process_block_on_ipc marks the thread BLOCKED, a sender on
         * another CPU may immediately call process_wake_thread, enqueuing this
         * thread as READY.  Without the flag, a second CPU could dequeue and
         * start restoring the saved context before this CPU has finished
         * context_switch_high inside process_yield — running two CPUs on the
         * same kernel stack.  The scheduler's ready_queue_dequeue skips and
         * re-enqueues any READY thread with blocking_transition set; the flag
         * is cleared in the PROCESS_RUN_BLOCKED handler after the context save
         * is complete.
         */
        if (thread) {
            __atomic_store_n(&thread->blocking_transition, 1, __ATOMIC_RELEASE);
        }
        process_block_on_ipc(proc);
        spinlock_unlock(&ep->lock);

        /*
         * If a sender fired process_wake_thread() in the window between the
         * ep->lock release and here, our state is already READY — loop back
         * to collect the message instead of yielding.
         */
        if (proc->state != PROCESS_STATE_BLOCKED ||
            (thread && thread->state != THREAD_STATE_BLOCKED)) {
            if (thread) {
                __atomic_store_n(&thread->blocking_transition, 0, __ATOMIC_RELEASE);
            }
            continue;
        }
        process_yield(PROCESS_RUN_BLOCKED);
        /* blocking_transition cleared by the PROCESS_RUN_BLOCKED scheduler handler */
    }
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
    uint32_t ep_id_val = ep->id;
    spinlock_unlock(&ep->lock);
    /* Strict thread-targeted wake: notification count is retained if no waiter
     * is currently blocked on this endpoint. */
    if (waiter_tid != 0) {
        (void)process_wake_thread(waiter_tid);
    }
    if (__atomic_load_n(&g_active_select_count, __ATOMIC_ACQUIRE) > 0) {
        ipc_select_signal_ep(ep_id_val);
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

/*
 * SMP-MED-07: ipc_wait_blocking_for — arm + block + re-check atomically.
 *
 * ipc_wait_for arms ep->waiter_tid then releases ep->lock and returns
 * IPC_EMPTY, leaving the caller to block separately.  A sender that fires in
 * the window between the lock release and the caller's process_block_on_ipc
 * delivers its notification to a still-RUNNING thread; the wake is a no-op
 * and the thread sleeps forever.
 *
 * Fix: mirror ipc_recv_blocking_for.  Arm the waiter and call
 * process_block_on_ipc while ep->lock is still held so no sender can fire
 * between arming and blocking.  Re-check notify_count under the lock on every
 * iteration to consume notifications that arrived before we could yield.
 */
int
ipc_wait_blocking_for(uint32_t receiver_context_id, uint32_t endpoint)
{
    for (;;) {
        process_t *proc = process_get(process_current_pid());
        if (!proc) {
            return IPC_ERR_INVALID;
        }

        ipc_endpoint_t *ep = 0;
        {
            list_iter_t eit;
            spinlock_lock(&g_endpoint_table_lock);
            ipc_endpoint_t *cur = (ipc_endpoint_t *)list_first(&g_endpoint_table, &eit);
            while (cur) {
                if (cur->id == endpoint && cur->in_use) {
                    ep = cur;
                    break;
                }
                cur = (ipc_endpoint_t *)list_next(&eit);
            }
            if (ep) {
                spinlock_lock(&ep->lock);
            }
            spinlock_unlock(&g_endpoint_table_lock);
        }
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
        if (ep->notify_count > 0) {
            ep->notify_count--;
            ep->waiter_tid = 0;
            spinlock_unlock(&ep->lock);
            return IPC_OK;
        }
        /* Queue empty: arm and block atomically under ep->lock. */
        uint32_t my_tid = thread_current_tid();
        thread_t *thread = thread_get(my_tid);
        ep->waiter_tid = my_tid;
        if (thread) {
            __atomic_store_n(&thread->blocking_transition, 1, __ATOMIC_RELEASE);
        }
        process_block_on_ipc(proc);
        spinlock_unlock(&ep->lock);

        if (proc->state != PROCESS_STATE_BLOCKED ||
            (thread && thread->state != THREAD_STATE_BLOCKED)) {
            if (thread) {
                __atomic_store_n(&thread->blocking_transition, 0, __ATOMIC_RELEASE);
            }
            continue;
        }
        process_yield(PROCESS_RUN_BLOCKED);
    }
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

int ipc_wait_blocking(uint32_t endpoint) {
    return ipc_wait_blocking_for(IPC_CONTEXT_KERNEL, endpoint);
}

/* --- Select set implementation --- */

int
ipc_select_create(uint32_t owner_context_id, uint32_t *out_id)
{
    if (!out_id) {
        return IPC_ERR_INVALID;
    }
    spinlock_lock(&g_select_table_lock);
    ipc_select_t *slot = 0;
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        if (!g_select_table[i].in_use) {
            slot = &g_select_table[i];
            break;
        }
    }
    if (!slot) {
        spinlock_unlock(&g_select_table_lock);
        return IPC_ERR_FULL;
    }
    uint32_t id = g_next_select_id++;
    if (g_next_select_id == 0) {
        g_next_select_id = 1;
    }
    slot->id = id;
    slot->in_use = 1;
    slot->owner_context_id = owner_context_id;
    slot->waiter_tid = 0;
    slot->ready_ep = 0;
    slot->ready = 0;
    slot->ep_count = 0;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX; i++) {
        slot->ep_ids[i] = 0;
    }
    spinlock_unlock(&g_select_table_lock);
    __atomic_fetch_add(&g_active_select_count, 1, __ATOMIC_RELEASE);
    *out_id = id;
    return IPC_OK;
}

int
ipc_select_add(uint32_t select_id, uint32_t endpoint_id)
{
    /* Validate endpoint ownership before touching the select set.
     * ipc_endpoint_owner_context uses only g_endpoint_table_lock. */
    uint32_t ep_owner = ipc_endpoint_owner_context(endpoint_id);
    if (ep_owner == 0) {
        return IPC_ERR_INVALID;
    }

    ipc_select_t *sel = ipc_select_find_locked(select_id);
    if (!sel) {
        return IPC_ERR_INVALID;
    }
    /* Endpoint must be owned by the same context as the select set. */
    if (ep_owner != sel->owner_context_id) {
        spinlock_unlock(&sel->lock);
        return IPC_ERR_PERM;
    }
    if (sel->ep_count >= IPC_SELECT_EPS_MAX) {
        spinlock_unlock(&sel->lock);
        return IPC_ERR_FULL;
    }
    /* Guard against duplicate add. */
    for (uint32_t i = 0; i < sel->ep_count; i++) {
        if (sel->ep_ids[i] == endpoint_id) {
            spinlock_unlock(&sel->lock);
            return IPC_OK;
        }
    }
    sel->ep_ids[sel->ep_count++] = endpoint_id;
    spinlock_unlock(&sel->lock);
    return IPC_OK;
}

int
ipc_select_wait(uint32_t select_id, uint32_t *out_ready_ep)
{
    if (!out_ready_ep) {
        return IPC_ERR_INVALID;
    }
    ipc_select_t *sel = ipc_select_find_locked(select_id);
    if (!sel) {
        return IPC_ERR_INVALID;
    }
    if (sel->ep_count == 0) {
        spinlock_unlock(&sel->lock);
        return IPC_ERR_INVALID;
    }
    spinlock_unlock(&sel->lock);

    uint32_t my_tid = thread_current_tid();
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return IPC_ERR_INVALID;
    }

    for (;;) {
        /* Arm: set waiter BEFORE scanning so any concurrent sender sets
         * ready=1 and we catch it in the post-scan check. */
        spinlock_lock(&sel->lock);
        if (!sel->in_use) {
            spinlock_unlock(&sel->lock);
            return IPC_ERR_INVALID;
        }
        sel->waiter_tid = my_tid;
        sel->ready = 0;
        sel->ready_ep = 0;
        uint32_t ep_count = sel->ep_count;
        uint32_t ep_ids[IPC_SELECT_EPS_MAX];
        for (uint32_t i = 0; i < ep_count; i++) {
            ep_ids[i] = sel->ep_ids[i];
        }
        spinlock_unlock(&sel->lock);

        /* Non-blocking scan: if any endpoint already has data, take it. */
        for (uint32_t i = 0; i < ep_count; i++) {
            ipc_endpoint_t *ep = ipc_endpoint_get(ep_ids[i]);
            if (!ep) {
                continue;
            }
            int has_data =
                (ep->type == IPC_ENDPOINT_TYPE_MESSAGE      && ep->count > 0) ||
                (ep->type == IPC_ENDPOINT_TYPE_NOTIFICATION && ep->notify_count > 0);
            uint32_t ep_id = ep->id;
            spinlock_unlock(&ep->lock);
            if (has_data) {
                spinlock_lock(&sel->lock);
                sel->waiter_tid = 0;
                sel->ready = 0;
                spinlock_unlock(&sel->lock);
                *out_ready_ep = ep_id;
                return IPC_OK;
            }
        }

        /* Post-scan check + block atomically under sel->lock.
         *
         * A sender that fired between the arm and the scan set ready=1.
         * If ready is set, take the result without blocking.  Otherwise
         * call process_block_on_ipc while still holding sel->lock so no
         * sender can call process_wake_thread before the thread is BLOCKED
         * (the same race-closing pattern as ipc_recv_blocking_for). */
        spinlock_lock(&sel->lock);
        if (!sel->in_use) {
            spinlock_unlock(&sel->lock);
            return IPC_ERR_INVALID;
        }
        if (sel->ready) {
            uint32_t rep = sel->ready_ep;
            sel->waiter_tid = 0;
            sel->ready = 0;
            spinlock_unlock(&sel->lock);
            *out_ready_ep = rep;
            return IPC_OK;
        }
        /* Re-arm in case a sender cleared waiter_tid but ready is still 0
         * (can happen if sender saw no existing waiter between arm and scan). */
        sel->waiter_tid = my_tid;
        thread_t *thread = thread_get(my_tid);
        if (thread) {
            __atomic_store_n(&thread->blocking_transition, 1, __ATOMIC_RELEASE);
        }
        process_block_on_ipc(proc);
        spinlock_unlock(&sel->lock);

        if (proc->state != PROCESS_STATE_BLOCKED ||
            (thread && thread->state != THREAD_STATE_BLOCKED)) {
            if (thread) {
                __atomic_store_n(&thread->blocking_transition, 0, __ATOMIC_RELEASE);
            }
            continue;
        }
        process_yield(PROCESS_RUN_BLOCKED);
        /* blocking_transition is cleared by the PROCESS_RUN_BLOCKED scheduler handler */

        /* Collect result written by ipc_select_signal. */
        spinlock_lock(&sel->lock);
        if (!sel->in_use) {
            spinlock_unlock(&sel->lock);
            return IPC_ERR_INVALID;
        }
        uint32_t rep = sel->ready_ep;
        sel->waiter_tid = 0;
        sel->ready = 0;
        spinlock_unlock(&sel->lock);
        *out_ready_ep = rep;
        return IPC_OK;
    }
}

void
ipc_select_destroy(uint32_t select_id)
{
    ipc_select_t *sel = ipc_select_find_locked(select_id);
    if (!sel) {
        return;
    }
    uint32_t wtid = sel->waiter_tid;
    sel->waiter_tid = 0;
    sel->in_use = 0;
    sel->ready = 0;
    sel->ep_count = 0;
    spinlock_unlock(&sel->lock);
    __atomic_fetch_sub(&g_active_select_count, 1, __ATOMIC_RELEASE);
    /* Wake any blocked waiter so it returns IPC_ERR_INVALID. */
    if (wtid != 0) {
        (void)process_wake_thread(wtid);
    }
}

void
ipc_selects_release_owner(uint32_t owner_context_id)
{
    if (owner_context_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < IPC_SELECT_TABLE_SIZE; i++) {
        spinlock_lock(&g_select_table[i].lock);
        if (g_select_table[i].in_use &&
            g_select_table[i].owner_context_id == owner_context_id) {
            uint32_t wtid = g_select_table[i].waiter_tid;
            g_select_table[i].waiter_tid = 0;
            g_select_table[i].in_use = 0;
            g_select_table[i].ready = 0;
            g_select_table[i].ep_count = 0;
            spinlock_unlock(&g_select_table[i].lock);
            __atomic_fetch_sub(&g_active_select_count, 1, __ATOMIC_RELEASE);
            if (wtid != 0) {
                (void)process_wake_thread(wtid);
            }
        } else {
            spinlock_unlock(&g_select_table[i].lock);
        }
    }
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
        if (ep->in_use && ep->owner_context_id == owner_context_id) {
            spinlock_lock(&ep->lock);
            if (ep->in_use && ep->owner_context_id == owner_context_id) {
                ep->in_use = 0;
                ep->waiter_tid = 0;
            }
            spinlock_unlock(&ep->lock);
        }
        ep = (ipc_endpoint_t *)list_next(&it);
    }
    spinlock_unlock(&g_endpoint_table_lock);
}
