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
