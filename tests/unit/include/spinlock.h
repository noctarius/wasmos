#ifndef WASMOS_TEST_SPINLOCK_H
#define WASMOS_TEST_SPINLOCK_H

#ifdef WASMOS_TEST_USE_REAL_SPINLOCK_DECLS
#include "arch/x86_64/spinlock.h"
#else

#include <stdint.h>

typedef struct {
    volatile uint32_t state;
} spinlock_t;

static inline void spinlock_init(spinlock_t* lock) {
    if (lock) {
        lock->state = 0u;
    }
}

static inline int spinlock_try_lock(spinlock_t* lock) {
    if (!lock) {
        return 0;
    }
    return __sync_lock_test_and_set(&lock->state, 1u) == 0u;
}

static inline void spinlock_lock(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    while (!spinlock_try_lock(lock)) {
        __sync_synchronize();
    }
}

static inline void spinlock_unlock(spinlock_t* lock) {
    if (lock) {
        __sync_lock_release(&lock->state);
    }
}

static inline void spinlock_lock_noirq(spinlock_t* lock) {
    spinlock_lock(lock);
}

static inline void spinlock_unlock_noirq(spinlock_t* lock) {
    spinlock_unlock(lock);
}

#endif

#endif
