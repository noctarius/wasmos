#ifndef WASMOS_SYNC_SPINLOCK_H
#define WASMOS_SYNC_SPINLOCK_H

#include "spinlock.h"

typedef spinlock_t ksync_spinlock_t;

static inline void
ksync_spinlock_init(ksync_spinlock_t *lock)
{
    spinlock_init(lock);
}

static inline int
ksync_spinlock_try_lock(ksync_spinlock_t *lock)
{
    return spinlock_try_lock(lock);
}

static inline void
ksync_spinlock_lock(ksync_spinlock_t *lock)
{
    spinlock_lock(lock);
}

static inline void
ksync_spinlock_unlock(ksync_spinlock_t *lock)
{
    spinlock_unlock(lock);
}

static inline void
ksync_spinlock_lock_noirq(ksync_spinlock_t *lock)
{
    spinlock_lock_noirq(lock);
}

static inline void
ksync_spinlock_unlock_noirq(ksync_spinlock_t *lock)
{
    spinlock_unlock_noirq(lock);
}

#endif
