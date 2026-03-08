#include "spinlock.h"

void spinlock_init(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    lock->state = 0;
}

int spinlock_try_lock(spinlock_t *lock) {
    if (!lock) {
        return 0;
    }
    return __sync_lock_test_and_set(&lock->state, 1u) == 0u;
}

void spinlock_lock(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    while (!spinlock_try_lock(lock)) {
        __asm__ volatile("pause");
    }
}

void spinlock_unlock(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    __sync_lock_release(&lock->state);
}
