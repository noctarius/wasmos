/* Spinlocks for the host tests: definitions for the declarations in
 * arch/x86_64/spinlock.h, which the real kernel implements in
 * arch/x86_64/spinlock.c.
 *
 * A plain compare-and-swap spin. The kernel's version also saves RFLAGS and
 * clears IF for the duration, which a host process has neither the ability nor
 * the need to do -- so lock/lock_noirq are the same code here and a test cannot
 * observe the interrupt-state half of the real contract. Mutual exclusion
 * between the pthreads that stand in for CPUs is preserved, which is the part
 * the tests exercise. */
#include "arch/x86_64/spinlock.h"

void spinlock_init(spinlock_t* lock) {
    if (lock) {
        lock->state = 0u;
    }
}

int spinlock_try_lock(spinlock_t* lock) {
    if (!lock) {
        return 0;
    }
    return __sync_lock_test_and_set(&lock->state, 1u) == 0u;
}

void spinlock_lock(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    while (!spinlock_try_lock(lock)) {
        __sync_synchronize();
    }
}

void spinlock_unlock(spinlock_t* lock) {
    if (lock) {
        __sync_lock_release(&lock->state);
    }
}

void spinlock_lock_noirq(spinlock_t* lock) {
    spinlock_lock(lock);
}

void spinlock_unlock_noirq(spinlock_t* lock) {
    spinlock_unlock(lock);
}
