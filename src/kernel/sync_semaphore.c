#include "sync/semaphore.h"
#include "sync/spinlock.h"

/*
 * Counting semaphore layered on sched_event_t: the count lives in event.cnt and
 * event.lock guards both it and the wait list, so a blocking acquire hands off
 * to sched_event_wait() with that lock held and cannot miss a release.
 *
 * The count saturates: release() refuses at UINT32_MAX rather than wrapping to
 * 0, which would report a full semaphore as empty and strand every waiter.
 * Entry points return KSYNC_SEMAPHORE_OK / KSYNC_SEMAPHORE_BUSY, or -1 for a
 * NULL semaphore or that overflow refusal.
 */

/* Publishes the semaphore with `initial_count` permits and an empty wait list.
 * Any initial_count is accepted, including 0 (every acquire blocks until a
 * release) and UINT32_MAX (already saturated: the next release is refused).  Not
 * safe against a live semaphore — it drops the wait list without waking it. */
void ksync_semaphore_init(ksync_semaphore_t* sem, uint32_t initial_count) {
    if (!sem) {
        return;
    }
    sched_event_init(&sem->event, SCHED_EVENT_TYPE_SEMAPHORE);
    sem->event.cnt = initial_count;
}

int ksync_semaphore_try_acquire(ksync_semaphore_t* sem) {
    int rc = KSYNC_SEMAPHORE_BUSY;

    if (!sem) {
        return -1;
    }

    ksync_spinlock_lock(&sem->event.lock);
    if (sem->event.cnt != 0u) {
        sem->event.cnt--;
        rc = KSYNC_SEMAPHORE_OK;
    }
    ksync_spinlock_unlock(&sem->event.lock);
    return rc;
}

/* Blocks until a permit is taken; only ever returns KSYNC_SEMAPHORE_OK or -1 for
 * a NULL semaphore, never BUSY.  The loop re-tests the count after each wake for
 * the same reason the mutex does: release() increments and then wakes, so a
 * third thread can consume the permit before the woken waiter runs.  Waiters are
 * therefore not FIFO-fair with respect to the count. */
int ksync_semaphore_acquire(ksync_semaphore_t* sem) {
    if (!sem) {
        return -1;
    }

    for (;;) {
        ksync_spinlock_lock(&sem->event.lock);
        if (sem->event.cnt != 0u) {
            sem->event.cnt--;
            ksync_spinlock_unlock(&sem->event.lock);
            return KSYNC_SEMAPHORE_OK;
        }
        sched_event_wait(&sem->event, 0);
    }
}

int ksync_semaphore_release(ksync_semaphore_t* sem) {
    if (!sem) {
        return -1;
    }

    ksync_spinlock_lock(&sem->event.lock);
    if (sem->event.cnt == 0xFFFFFFFFu) {
        ksync_spinlock_unlock(&sem->event.lock);
        return -1;
    }
    sem->event.cnt++;
    (void)sched_event_wake_one(&sem->event, 0, SCHED_PEND_OK);
    ksync_spinlock_unlock(&sem->event.lock);
    return KSYNC_SEMAPHORE_OK;
}

/* Snapshot of the permit count, taken under event.lock so it is never torn.  It
 * is stale the instant the lock is dropped, so it is a diagnostic only: deciding
 * to acquire based on it reintroduces exactly the check-then-act race that
 * ksync_semaphore_try_acquire exists to avoid.  A NULL semaphore reads 0, which
 * is indistinguishable from a genuinely empty one. */
uint32_t ksync_semaphore_count(ksync_semaphore_t* sem) {
    uint32_t count = 0;

    if (!sem) {
        return 0;
    }

    ksync_spinlock_lock(&sem->event.lock);
    count = sem->event.cnt;
    ksync_spinlock_unlock(&sem->event.lock);
    return count;
}
