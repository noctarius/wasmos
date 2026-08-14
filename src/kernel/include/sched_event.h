#ifndef WASMOS_SCHED_EVENT_H
#define WASMOS_SCHED_EVENT_H

#include <stdint.h>
#include "sync/spinlock.h"
#include "sched_list.h"

/* What an event belongs to. Purely descriptive -- it changes no behaviour and
 * exists so a parked thread can be attributed when debugging. */
typedef enum {
    SCHED_EVENT_TYPE_IPC = 0,
    SCHED_EVENT_TYPE_JOIN = 1,
    SCHED_EVENT_TYPE_PROCESS = 2,
    SCHED_EVENT_TYPE_SELECT = 3,
    SCHED_EVENT_TYPE_FUTEX = 4,
    SCHED_EVENT_TYPE_TIMER = 5,
    SCHED_EVENT_TYPE_MUTEX = 6,
    SCHED_EVENT_TYPE_SEMAPHORE = 7,
} sched_event_type_t;

/* Why a waiter was resumed, left in thread_t::pend_state by the waker and read
 * by the waiter after sched_event_wait returns. A waiter that does not inspect
 * it must still re-test the condition it slept on, since none of these
 * guarantees the condition holds. */
typedef enum {
    SCHED_PEND_NONE = 0,    /* set on entry to the wait; still set = spurious wake */
    SCHED_PEND_OK = 1,      /* a normal wake, with pend_data from the waker */
    SCHED_PEND_TIMEOUT = 2, /* the timed-wait deadline passed */
    SCHED_PEND_ABORT = 3,   /* the object being waited on is being destroyed */
} sched_pend_state_t;

/* One wait queue plus the lock that guards both it and whatever state the
 * waiters test. Holding ev->lock across the test-then-block sequence is what
 * makes a wake impossible to lose. */
typedef struct {
    ksync_spinlock_t lock;
    list_head_t wait_list; /* thread_t.event_node members */
    uint32_t cnt;          /* permit count; used only by ksync_semaphore_t */
    sched_event_type_t type;
} sched_event_t;

struct thread;

/* Initialise an event struct. */
void sched_event_init(sched_event_t* ev, sched_event_type_t type);

/*
 * Block the calling thread on ev.  Caller must hold ev->lock on entry;
 * the lock is released before yielding and must NOT be held on return.
 * timeout_ms == 0 means no timeout.
 *
 * Holding ev->lock across the caller's condition test and this call is what
 * makes the wake impossible to lose. Returns once the thread is dispatched
 * again -- from a wake, a timeout or an abort, all three indistinguishable
 * except through thread_t::pend_state, and none of them a guarantee that the
 * condition now holds: always re-test in a loop. If a thread was already
 * registered on another event it is unlinked from it first. Releases ev->lock
 * even in the degenerate case where there is no current thread (nothing is
 * blocked and it returns at once).
 */
void sched_event_wait(sched_event_t* ev, uint32_t timeout_ms);

/*
 * Wake the first waiter on ev with the given pend state and data.
 * Caller must hold ev->lock.  Returns the woken thread or NULL.
 * FIFO: the longest-waiting thread goes first. `data` lands in the waiter's
 * pend_data. Marks the thread runnable and enqueues it (subject to the
 * wake/block handshake) before returning, so the returned pointer names a
 * thread that may already be running on another CPU.
 */
struct thread* sched_event_wake_one(sched_event_t* ev, uint64_t data, sched_pend_state_t pend);

/*
 * Wake all waiters.  Caller must hold ev->lock.
 * Returns the number of threads woken.
 * Every waiter gets the same data and pend state, and the wait list is empty on
 * return.
 */
int sched_event_wake_all(sched_event_t* ev, uint64_t data, sched_pend_state_t pend);

/*
 * Abort all waiters (SCHED_PEND_ABORT).  Used when an endpoint or
 * select set is destroyed.  Caller must hold ev->lock.
 */
void sched_event_abort_all(sched_event_t* ev);

/*
 * Wake any thread whose timed-wait deadline (sched_event_wait timeout_ms) has
 * passed.  Called from the scheduler each dispatch; cheap when nothing is armed.
 * Must run in scheduler context with no event or run-queue lock held: it takes
 * the lock of each event it detaches a thread from, and enqueues the woken
 * threads. The fast path is a single compare against a global lower-bound hint,
 * so a scan happens only when something may actually be due.
 */
void sched_timeout_check(void);

#endif /* WASMOS_SCHED_EVENT_H */
