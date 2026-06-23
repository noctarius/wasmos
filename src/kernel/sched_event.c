#ifdef WASMOS_SCHED_THREADABLE

#include "sched_event.h"
#include "sched.h"
#include "thread.h"
#include "process.h"
#include "timer.h"

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

/* -------------------------------------------------------------------------
 * Timed waits.
 *
 * A thread that blocks with a non-zero timeout records a deadline tick in
 * thread->sched_timeout_tick (driven by the timer tick — PIC or LAPIC alike).
 * sched_timeout_check(), called from the scheduler each dispatch, wakes any
 * thread whose deadline has passed (as a SCHED_PEND_TIMEOUT wake, which the
 * waiter sees as "no data — retry").  g_sched_timeout_next is a lock-free
 * lower-bound hint so the common (nothing-due) case is a single compare.
 * ------------------------------------------------------------------------- */
static volatile uint64_t g_sched_timeout_next = (uint64_t)-1;

static void
sched_timeout_arm(thread_t *t, uint64_t deadline_tick)
{
    if (deadline_tick == 0) {
        deadline_tick = 1;   /* 0 is reserved for "no timeout" */
    }
    t->sched_timeout_tick = deadline_tick;
    /* Atomically lower the hint so it never sits above this deadline, even when
     * sched_timeout_check() publishes a recomputed bound concurrently. */
    uint64_t cur = __atomic_load_n(&g_sched_timeout_next, __ATOMIC_ACQUIRE);
    while (deadline_tick < cur) {
        if (__atomic_compare_exchange_n(&g_sched_timeout_next, &cur, deadline_tick,
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
}

/* Detach an expired-timeout thread from its wait event and mark it runnable.
 * Runs in scheduler context with no event/run-queue lock held on entry.
 *
 * Correctness against concurrent normal wakes is subtle: t->wait_event is read
 * unlocked here, but it can only be CLEARED (->0) or, after a wake, RE-SET to a
 * different event once the thread resumes and blocks again.  Clearing happens
 * under ev->lock (sched_event_wake_one); re-setting happens under the *new*
 * event's lock.  So we lock the event we observed and then RE-VALIDATE that the
 * thread is still waiting on exactly that event (t->wait_event == ev) before
 * touching t->event_node.  Because clearing wait_event from `ev` requires
 * ev->lock — which we now hold — no normal wake can transition the thread out
 * of `ev` while we hold the lock; and if one already did (wait_event != ev,
 * possibly already re-blocked on another event), we must NOT touch
 * t->event_node, since it may now live on a different event's wait_list that we
 * do not hold the lock for.  Skipping in that case is safe: the normal wake
 * already made the thread runnable. */
static void
sched_timeout_fire(thread_t *t)
{
    sched_event_t *ev = (sched_event_t *)__atomic_load_n(&t->wait_event, __ATOMIC_ACQUIRE);
    if (!ev) {
        return;   /* already woken by a normal waker */
    }
    spinlock_lock(&ev->lock);
    if (t->wait_event == ev && !list_head_empty(&t->event_node)) {
        list_head_del(&t->event_node);
        t->wait_event = 0;
        t->pend_state = SCHED_PEND_TIMEOUT;
        spinlock_unlock(&ev->lock);
        sched_wake_thread(t);
    } else {
        /* A normal wake beat us to it (or the thread re-blocked elsewhere). */
        spinlock_unlock(&ev->lock);
    }
}

void
sched_timeout_check(void)
{
    uint64_t now = timer_ticks();
    uint64_t observed = __atomic_load_n(&g_sched_timeout_next, __ATOMIC_ACQUIRE);
    if (now < observed) {
        return;   /* fast path: nothing armed is due */
    }
    uint64_t next = (uint64_t)-1;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t *t = thread_table_at(i);
        uint64_t d = t ? t->sched_timeout_tick : 0;
        if (d == 0) {
            continue;
        }
        if (t->state != THREAD_STATE_BLOCKED) {
            t->sched_timeout_tick = 0;   /* stale: already woken some other way */
            continue;
        }
        if (d <= now) {
            t->sched_timeout_tick = 0;
            sched_timeout_fire(t);
        } else if (d < next) {
            next = d;
        }
    }
    /* Publish the recomputed lower-bound, but only if no sched_timeout_arm()
     * lowered the hint while we scanned.  A blind store could raise the hint
     * above a deadline armed mid-scan (whose tick we may not have observed),
     * which would make the fast path skip a due timeout forever.  If the CAS
     * fails, an arm already installed a value <= its own deadline; leave it. */
    (void)__atomic_compare_exchange_n(&g_sched_timeout_next, &observed, next,
                                      0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void
sched_event_wait(sched_event_t *ev, uint32_t timeout_ms)
{
    thread_t *t = thread_get(thread_current_tid());
    if (!t) {
        spinlock_unlock(&ev->lock);
        return;
    }

    if (timeout_ms > 0) {
        sched_timeout_arm(t, timer_ticks() + timer_ms_to_ticks(timeout_ms));
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

    /* Resumed (woken by a waker, a timeout, or an abort): disarm any pending
     * timeout so a stale deadline can't fire on a future blocking transition. */
    t->sched_timeout_tick = 0;
}

/* Detach one waiter from its event and make it runnable.  Caller holds
 * ev->lock.  Shared by wake_one (first waiter) and wake_all (every waiter). */
static void
sched_event_detach_wake(thread_t *t, uint64_t data, sched_pend_state_t pend)
{
    list_head_del(&t->event_node);
    t->wait_event = 0;
    t->sched_timeout_tick = 0;   /* woken normally; cancel any armed timeout */
    t->pend_state = (uint32_t)pend;
    t->pend_data  = data;
    sched_wake_thread(t);
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
    sched_event_detach_wake(t, data, pend);
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
        sched_event_detach_wake(t, data, pend);
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
