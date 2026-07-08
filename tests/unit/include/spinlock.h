#ifndef WASMOS_TEST_SPINLOCK_H
#define WASMOS_TEST_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t state;
} spinlock_t;

static inline void spinlock_init(spinlock_t *lock) {
    if (lock) {
        lock->state = 0;
    }
}

static inline int spinlock_try_lock(spinlock_t *lock) {
    if (!lock || lock->state != 0) {
        return 0;
    }
    lock->state = 1;
    return 1;
}

static inline void spinlock_lock(spinlock_t *lock) {
    if (lock) {
        lock->state = 1;
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    if (lock) {
        lock->state = 0;
    }
}

static inline void spinlock_lock_noirq(spinlock_t *lock) {
    spinlock_lock(lock);
}

static inline void spinlock_unlock_noirq(spinlock_t *lock) {
    spinlock_unlock(lock);
}

#endif
