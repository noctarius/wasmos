/* Host shadow of src/kernel/include/arch/x86_64/spinlock.h for a test that
 * includes "spinlock.h" directly.
 *
 * The lock is a plain compare-and-swap spin: a host process has no RFLAGS/IF
 * state to save, so the _noirq variants are the same code here, whereas the
 * real x86_64 lock differs between them by disabling interrupts while held.
 * WASMOS_TEST_USE_REAL_SPINLOCK_DECLS instead forwards to the real
 * declarations, for a test that links its own definitions
 * (tests/unit/test_kernel_sync_primitives.c defines the whole family).
 *
 * TODO: nothing includes this header -- kernel sources reach the arch header
 * through "sync/spinlock.h" -- so neither the shadow nor the
 * WASMOS_TEST_USE_REAL_SPINLOCK_DECLS switch has any effect on a build. */
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
