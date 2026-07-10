#include "arch/x86_64/spinlock.h"

void
spinlock_init(spinlock_t *lock)
{
    if (lock) {
        lock->state = 0u;
    }
}

int
spinlock_try_lock(spinlock_t *lock)
{
    if (!lock) {
        return 0;
    }
    return __sync_lock_test_and_set(&lock->state, 1u) == 0u;
}

void
spinlock_lock(spinlock_t *lock)
{
    if (!lock) {
        return;
    }
    while (!spinlock_try_lock(lock)) {
        __sync_synchronize();
    }
}

void
spinlock_unlock(spinlock_t *lock)
{
    if (lock) {
        __sync_lock_release(&lock->state);
    }
}

void
spinlock_lock_noirq(spinlock_t *lock)
{
    spinlock_lock(lock);
}

void
spinlock_unlock_noirq(spinlock_t *lock)
{
    spinlock_unlock(lock);
}
