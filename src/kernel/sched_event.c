#ifdef WASMOS_SCHED_THREADABLE

#include "sched_event.h"
#include "sched.h"
#include "thread.h"
#include "process.h"

/*
 * sched_event.c — unified blocking primitive for the threadable scheduler.
 *
 * A sched_event_t is a semaphore-style wait list.  Any number of threads
 * may block on an event; wakers pick them off one at a time or all at once.
 * The design mirrors Minos2's struct event / __wait_event / __wake_up_event_waiter.
 */

void
sched_event_init(sched_event_t *ev, sched_event_type_t type)
{
    spinlock_init(&ev->lock);
    list_head_init(&ev->wait_list);
    ev->cnt  = 0;
    ev->type = type;
}

void
sched_event_wait(sched_event_t *ev, uint32_t timeout_ms)
{
    (void)timeout_ms; /* TODO: wire up timer for non-zero timeout */

    thread_t *t = thread_get(thread_current_tid());
    if (!t) {
        spinlock_unlock(&ev->lock);
        return;
    }

    /* Ensure the thread's event_node is only in ONE wait_list at a time.
     * If a prior non-blocking ipc_recv_for registered it in a different
     * event's list, remove it first to prevent list corruption. */
    if (!list_head_empty(&t->event_node)) {
        list_head_del(&t->event_node);
    }
    if (t->wait_event && t->wait_event != ev) {
        /* blocking_transition may have been set by the prior registration;
         * reset it since we're re-registering on a different event. */
        __atomic_store_n(&t->blocking_transition, 0, __ATOMIC_RELEASE);
    }

    t->wait_event  = ev;
    t->pend_state  = SCHED_PEND_NONE;
    t->pend_data   = 0;

    /* Signal that a blocking transition is in progress.  sched_wake_thread
     * will spin until this clears (after context_switch_high saves state). */
    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_RELEASE);

    list_head_add_tail(&ev->wait_list, &t->event_node);
    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_EVENT);

    spinlock_unlock(&ev->lock);

    /* Yield back to scheduler; blocking_transition is cleared by the
     * PROCESS_RUN_BLOCKED handling in process_schedule_once_impl. */
    process_yield(PROCESS_RUN_BLOCKED);
}

thread_t *
sched_event_wake_one(sched_event_t *ev,
                     uint64_t data,
                     sched_pend_state_t pend)
{
    /* Caller holds ev->lock. */
    if (list_head_empty(&ev->wait_list)) {
        return 0;
    }
    thread_t *t = list_first_entry(&ev->wait_list, thread_t, event_node);
    list_head_del(&t->event_node);
    t->wait_event = 0;
    t->pend_state = (uint32_t)pend;
    t->pend_data  = data;
    sched_wake_thread(t);
    return t;
}

int
sched_event_wake_all(sched_event_t *ev,
                     uint64_t data,
                     sched_pend_state_t pend)
{
    /* Caller holds ev->lock. */
    int woken = 0;
    list_head_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &ev->wait_list) {
        thread_t *t = list_entry(pos, thread_t, event_node);
        list_head_del(&t->event_node);
        t->wait_event = 0;
        t->pend_state = (uint32_t)pend;
        t->pend_data  = data;
        sched_wake_thread(t);
        woken++;
    }
    return woken;
}

void
sched_event_abort_all(sched_event_t *ev)
{
    /* Caller holds ev->lock. */
    sched_event_wake_all(ev, 0, SCHED_PEND_ABORT);
}

#endif /* WASMOS_SCHED_THREADABLE */
