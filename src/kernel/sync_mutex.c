#include "sync/mutex.h"
#include "sync/spinlock.h"

#include "thread.h"

static uint32_t
ksync_mutex_current_owner_tid(void)
{
    uint32_t tid = thread_current_tid();
    return tid != 0 ? tid : 0xFFFFFFFFu;
}

void
ksync_mutex_init(ksync_mutex_t *mutex)
{
    if (!mutex) {
        return;
    }
    sched_event_init(&mutex->event, SCHED_EVENT_TYPE_MUTEX);
    mutex->owner_tid = 0u;
    mutex->locked = 0u;
}

int
ksync_mutex_try_lock(ksync_mutex_t *mutex)
{
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

int
ksync_mutex_lock(ksync_mutex_t *mutex)
{
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

int
ksync_mutex_unlock(ksync_mutex_t *mutex)
{
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
