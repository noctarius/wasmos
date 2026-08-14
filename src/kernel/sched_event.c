#include "sched_event.h"
#include "sched.h"
#include "thread.h"
#include "process.h"
#include "timer.h"

/*
 * sched_event.c — unified blocking primitive for the kernel scheduler.
 *
 * A sched_event_t is a semaphore-style wait list.  Any number of threads
 * may block on an event; wakers pick them off one at a time or all at once.
 * The design mirrors Minos2's struct event / __wait_event / __wake_up_event_waiter.
 */

/* Publishes an event with an empty wait list, a zero counter and no lock held.
 * `type` is carried purely for diagnostics — nothing in this file branches on it
 * — so the owning primitive picks the tag that identifies it in a dump.  ev->cnt
 * is left to the owner to interpret: the semaphore keeps its permit count there,
 * every other user ignores it.  Not safe against a live event: it re-initialises
 * the lock and drops the wait list head, stranding anyone already parked. */
void sched_event_init(sched_event_t* ev, sched_event_type_t type) {
    ksync_spinlock_init(&ev->lock);
    list_head_init(&ev->wait_list);
    ev->cnt = 0;
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
/* Bumped by every arm.  sched_timeout_check uses it to detect that ANY deadline
 * was installed while it was scanning -- see the publish step there. */
static volatile uint32_t g_sched_timeout_arm_seq = 0;

static void sched_timeout_arm(thread_t* t, uint64_t deadline_tick) {
    if (deadline_tick == 0) {
        deadline_tick = 1; /* 0 is reserved for "no timeout" */
    }
    __atomic_store_n(&t->sched_timeout_tick, deadline_tick, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_sched_timeout_arm_seq, 1u, __ATOMIC_ACQ_REL);
    /* Atomically lower the hint so it never sits above this deadline, even when
     * sched_timeout_check() publishes a recomputed bound concurrently. */
    uint64_t cur = __atomic_load_n(&g_sched_timeout_next, __ATOMIC_ACQUIRE);
    while (deadline_tick < cur) {
        if (__atomic_compare_exchange_n(&g_sched_timeout_next, &cur, deadline_tick, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
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
 * event's lock.  The observed event is therefore locked first and then
 * RE-VALIDATED: the thread must still be waiting on exactly that event
 * (t->wait_event == ev) before t->event_node is touched.  Because clearing
 * wait_event from `ev` requires ev->lock, no normal wake can transition the
 * thread out of `ev` while that lock is held; and if one already did
 * (wait_event != ev, possibly already re-blocked on another event),
 * t->event_node must NOT be touched, since it may live on a different event's
 * wait_list whose lock is not held here.  Skipping in that case is safe: the
 * normal wake already made the thread runnable. */
static void sched_timeout_fire(thread_t* t) {
    sched_event_t* ev = (sched_event_t*)__atomic_load_n(&t->wait_event, __ATOMIC_ACQUIRE);
    if (!ev) {
        return; /* already woken by a normal waker */
    }
    ksync_spinlock_lock(&ev->lock);
    if (t->wait_event == ev && !list_head_empty(&t->event_node)) {
        list_head_del(&t->event_node);
        __atomic_store_n(&t->wait_event, (sched_event_t*)0, __ATOMIC_RELEASE);
        t->pend_state = SCHED_PEND_TIMEOUT;
        ksync_spinlock_unlock(&ev->lock);
        sched_wake_thread(t);
    } else {
        /* A normal wake beat us to it (or the thread re-blocked elsewhere). */
        ksync_spinlock_unlock(&ev->lock);
    }
}

/* Sweeps every thread slot for an expired deadline and fires it.  Called from
 * scheduler context with no event or run-queue lock held, which is what lets it
 * take an arbitrary ev->lock and enqueue inside sched_timeout_fire.
 *
 * O(THREAD_MAX_COUNT) when it runs at all, but the g_sched_timeout_next hint
 * short-circuits it to a single compare whenever nothing armed is due — the
 * common case on every dispatch.  Also opportunistically clears the deadline of
 * any thread that is no longer BLOCKED, so a stale arm cannot fire against a
 * later, unrelated block. */
void sched_timeout_check(void) {
    uint64_t now = timer_ticks();
    uint64_t observed = __atomic_load_n(&g_sched_timeout_next, __ATOMIC_ACQUIRE);
    if (now < observed) {
        return; /* fast path: nothing armed is due */
    }
    uint32_t arm_seq = __atomic_load_n(&g_sched_timeout_arm_seq, __ATOMIC_ACQUIRE);
    uint64_t next = (uint64_t)-1;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* t = thread_table_at(i);
        uint64_t d = t ? __atomic_load_n(&t->sched_timeout_tick, __ATOMIC_ACQUIRE) : 0;
        if (d == 0) {
            continue;
        }
        if (t->state != THREAD_STATE_BLOCKED) {
            /* stale: already woken some other way */
            __atomic_store_n(&t->sched_timeout_tick, 0u, __ATOMIC_RELEASE);
            continue;
        }
        if (d <= now) {
            __atomic_store_n(&t->sched_timeout_tick, 0u, __ATOMIC_RELEASE);
            sched_timeout_fire(t);
        } else if (d < next) {
            next = d;
        }
    }
    /* Publish the recomputed lower bound, but only if NO arm happened during the
     * scan.  An arm can install a deadline in a slot this scan already passed,
     * so `next` would not include it and publishing would raise the hint above a
     * live deadline -- the fast path then skips it forever and that thread's
     * timed wait never expires.
     *
     * The value CAS alone cannot detect this.  sched_timeout_arm only LOWERS the
     * hint, so an arm whose deadline sits ABOVE the current hint leaves the value
     * untouched, the CAS succeeds, and the new deadline is orphaned.  The
     * generation counter catches every arm regardless of its deadline.  Losing
     * the race just leaves the old bound in place, which costs one extra scan and
     * is self-correcting. */
    uint32_t arm_seq_now = __atomic_load_n(&g_sched_timeout_arm_seq, __ATOMIC_ACQUIRE);
    if (arm_seq_now == arm_seq) {
        (void)__atomic_compare_exchange_n(&g_sched_timeout_next, &observed, next, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

/* Parks the calling thread on `ev`.  The caller MUST hold ev->lock; this
 * function releases it, and it is that hand-off — decide-to-block and join the
 * wait list under one lock — that closes the lost-wakeup window against a waker
 * holding the same lock.  timeout_ms == 0 means no deadline.
 *
 * Returns once the thread is resumed, by a waker, a timeout, or an abort;
 * t->pend_state distinguishes them (SCHED_PEND_OK / _TIMEOUT / _ABORT) and
 * t->pend_data carries the waker's payload.  A wake is not a guarantee that the
 * awaited condition holds, so every caller re-tests it in a loop.
 *
 * With no current thread (early boot, before the thread table is live) there is
 * nothing to park: the lock is released and the call returns immediately, which
 * turns the caller's wait loop into a spin. */
void sched_event_wait(sched_event_t* ev, uint32_t timeout_ms) {
    thread_t* t = thread_get(thread_current_tid());
    if (!t) {
        ksync_spinlock_unlock(&ev->lock);
        return;
    }

    if (timeout_ms > 0) {
        sched_timeout_arm(t, timer_ticks() + timer_ms_to_ticks(timeout_ms));
    }

    /* Ensure the thread's event_node is only in ONE wait_list at a time.
     *
     * No current path registers a thread on a second event -- ipc_recv_for must
     * not register a waiter at all. The guard exists because it is what makes a
     * wake that does NOT unlink survivable. thread_wake_if_blocked() is exactly such a primitive:
     * it flips BLOCKED -> READY while leaving event_node linked and wait_event set. Its only
     * out-of-band caller today is process_unpark_pid(), which targets a freshly spawned child that
     * has never run and therefore has an empty event_node -- so the state is not currently
     * reachable. Add one caller that unparks a thread already blocked on an endpoint and it is, and
     * without this unlink the re-add would splice the list around the node and silently drop every
     * waiter queued behind it. Covered by tests/unit/test_ipc.c M4 (moving between endpoints) and
     * M5 (re-blocking on the same one). */
    if (!list_head_empty(&t->event_node)) {
        list_head_del(&t->event_node);
    }
    if (t->wait_event && t->wait_event != ev) {
        /* blocking_transition may have been set by the prior registration;
         * reset it because this is a re-registration on a different event. */
        __atomic_store_n(&t->blocking_transition, 0, __ATOMIC_RELEASE);
    }

    /* Published atomically to pair with sched_timeout_fire's unlocked load. */
    __atomic_store_n(&t->wait_event, ev, __ATOMIC_RELEASE);
    t->pend_state = SCHED_PEND_NONE;
    t->pend_data = 0;

    /* Signal that a blocking transition is in progress.  sched_wake_thread
     * will spin until this clears (after context_switch_high saves state). */
    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_RELEASE);

    list_head_add_tail(&ev->wait_list, &t->event_node);
    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_EVENT);

    ksync_spinlock_unlock(&ev->lock);

    /* Yield back to scheduler; blocking_transition is cleared by the
     * PROCESS_RUN_BLOCKED handling in process_schedule_once_impl. */
    process_yield(PROCESS_RUN_BLOCKED);

    /* Resumed (woken by a waker, a timeout, or an abort): disarm any pending
     * timeout so a stale deadline can't fire on a future blocking transition.
     *
     * Atomic like every other access to this field. It has three writers that
     * share no lock -- the waker here holds ev->lock, this resume path holds
     * nothing, and sched_timeout_check scans it from another CPU -- so the
     * plain stores were a data race even though every one of them writes 0.
     * Found by the TSan arm of tests/unit/test_ipc_concurrency.c. */
    __atomic_store_n(&t->sched_timeout_tick, 0u, __ATOMIC_RELEASE);
}

/* Detach one waiter from its event and make it runnable.  Caller holds
 * ev->lock.  Shared by wake_one (first waiter) and wake_all (every waiter). */
static void sched_event_detach_wake(thread_t* t, uint64_t data, sched_pend_state_t pend) {
    list_head_del(&t->event_node);
    /* Released atomically: sched_timeout_fire loads wait_event WITHOUT the
     * event lock (it has to, to know which lock to take), so a plain store
     * here is a mixed-atomicity access to the same field. */
    __atomic_store_n(&t->wait_event, (sched_event_t*)0, __ATOMIC_RELEASE);
    /* woken normally; cancel any armed timeout */
    __atomic_store_n(&t->sched_timeout_tick, 0u, __ATOMIC_RELEASE);
    t->pend_state = (uint32_t)pend;
    t->pend_data = data;
    sched_wake_thread(t);
}

/* Detaches and wakes the LONGEST-waiting thread (the list is appended at the
 * tail), delivering `data` as its pend_data and `pend` as its pend_state.
 * Returns that thread, or 0 if nobody was waiting — which is not an error: it is
 * how a waker learns the condition it just published has no audience yet.  The
 * returned pointer is only meaningful under the caller's ev->lock; the thread is
 * already runnable and may be dispatched on another CPU the moment it drops.
 * Caller holds ev->lock. */
thread_t* sched_event_wake_one(sched_event_t* ev, uint64_t data, sched_pend_state_t pend) {
    /* Caller holds ev->lock. */
    if (list_head_empty(&ev->wait_list)) {
        return 0;
    }
    thread_t* t = list_first_entry(&ev->wait_list, thread_t, event_node);
    sched_event_detach_wake(t, data, pend);
    return t;
}

/* Drains the wait list, giving every waiter the same `data`/`pend`, and returns
 * how many were woken (0 if the list was empty).  Iterated with the _safe
 * variant because sched_event_detach_wake unlinks the node it is standing on.
 * Caller holds ev->lock. */
int sched_event_wake_all(sched_event_t* ev, uint64_t data, sched_pend_state_t pend) {
    /* Caller holds ev->lock. */
    int woken = 0;
    list_head_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &ev->wait_list) {
        thread_t* t = list_entry(pos, thread_t, event_node);
        sched_event_detach_wake(t, data, pend);
        woken++;
    }
    return woken;
}

/* Wakes every waiter with SCHED_PEND_ABORT, meaning "the thing you were waiting
 * on is going away".  Used by teardown paths (endpoint release, select-set
 * destroy) so a parked thread returns from sched_event_wait instead of waiting
 * on an object that no longer exists.  The waiters must NOT re-touch that
 * object; they re-resolve it by id or give up.  Caller holds ev->lock. */
void sched_event_abort_all(sched_event_t* ev) {
    /* Caller holds ev->lock. */
    sched_event_wake_all(ev, 0, SCHED_PEND_ABORT);
}
