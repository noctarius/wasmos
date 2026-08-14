#include "sync/mutex.h"
#include "sync/spinlock.h"

#include "thread.h"

/*
 * Non-recursive kernel mutex layered on sched_event_t: mutex->event.lock is both
 * the state lock and the wait-list lock, so a blocking lock() hands off to
 * sched_event_wait() with that lock already held and cannot lose a wake.
 *
 * All three entry points return KSYNC_MUTEX_OK on success and -1 for a NULL
 * mutex or a rejected state.  The rejected states are deliberately distinct
 * from KSYNC_MUTEX_BUSY: re-locking a mutex this thread already owns is a
 * deadlock, so it is refused with -1 rather than blocking forever, and
 * unlocking one this thread does not own is refused the same way.
 */
/* owner_tid 0 means "unlocked", so a caller with no current thread (early boot,
 * before the thread table is live) is given the reserved pseudo-tid 0xFFFFFFFF
 * instead.  Every such caller shares that identity. */
static uint32_t ksync_mutex_current_owner_tid(void) {
    uint32_t tid = thread_current_tid();
    return tid != 0 ? tid : 0xFFFFFFFFu;
}

/* Publishes the mutex unlocked with an empty wait list.  Not safe against a live
 * mutex: it re-initialises event.lock and drops the wait list head without
 * waking anyone, so any thread already parked on it would never be woken. */
void ksync_mutex_init(ksync_mutex_t* mutex) {
    if (!mutex) {
        return;
    }
    sched_event_init(&mutex->event, SCHED_EVENT_TYPE_MUTEX);
    mutex->owner_tid = 0u;
    mutex->locked = 0u;
}

int ksync_mutex_try_lock(ksync_mutex_t* mutex) {
    uint32_t owner_tid = 0;
    int rc = KSYNC_MUTEX_BUSY;

    if (!mutex) {
        return -1;
    }

    owner_tid = ksync_mutex_current_owner_tid();
    ksync_spinlock_lock(&mutex->event.lock);
    if (!mutex->locked) {
        mutex->locked = 1u;
        mutex->owner_tid = owner_tid;
        rc = KSYNC_MUTEX_OK;
    } else if (mutex->owner_tid == owner_tid) {
        rc = -1;
    }
    ksync_spinlock_unlock(&mutex->event.lock);
    return rc;
}

/* Blocks (via sched_event_wait, which releases event.lock before yielding) until
 * the mutex is acquired.
 *
 * The loop re-tests `locked` after every wake because a wake is NOT a handoff:
 * ksync_mutex_unlock clears the owner and then wakes one waiter, so a third
 * thread calling try_lock/lock in that gap can take the mutex first.  The woken
 * waiter simply finds it locked again and re-parks.  The re-test also absorbs
 * the abort and spurious wakes sched_event_wait can return. */
int ksync_mutex_lock(ksync_mutex_t* mutex) {
    uint32_t owner_tid = 0;

    if (!mutex) {
        return -1;
    }

    owner_tid = ksync_mutex_current_owner_tid();
    for (;;) {
        ksync_spinlock_lock(&mutex->event.lock);
        if (!mutex->locked) {
            mutex->locked = 1u;
            mutex->owner_tid = owner_tid;
            ksync_spinlock_unlock(&mutex->event.lock);
            return KSYNC_MUTEX_OK;
        }
        if (mutex->owner_tid == owner_tid) {
            ksync_spinlock_unlock(&mutex->event.lock);
            return -1;
        }
        sched_event_wait(&mutex->event, 0);
    }
}

/* Releases the mutex and wakes at most one waiter, all under event.lock so the
 * clear and the wake cannot be split by a racing lock().  The woken thread is
 * only made runnable, not made the owner — see the handoff note on
 * ksync_mutex_lock.  Waking exactly one is what makes the wait list a queue
 * rather than a thundering herd. */
int ksync_mutex_unlock(ksync_mutex_t* mutex) {
    uint32_t owner_tid = 0;

    if (!mutex) {
        return -1;
    }

    owner_tid = ksync_mutex_current_owner_tid();
    ksync_spinlock_lock(&mutex->event.lock);
    if (!mutex->locked || mutex->owner_tid != owner_tid) {
        ksync_spinlock_unlock(&mutex->event.lock);
        return -1;
    }
    mutex->locked = 0u;
    mutex->owner_tid = 0u;
    (void)sched_event_wake_one(&mutex->event, 0, SCHED_PEND_OK);
    ksync_spinlock_unlock(&mutex->event.lock);
    return KSYNC_MUTEX_OK;
}
