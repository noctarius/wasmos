/* Spinlocks for the host tests: definitions for the declarations in
 * arch/x86_64/spinlock.h, which the real kernel implements in
 * arch/x86_64/spinlock.c.
 *
 * A plain compare-and-swap spin. The kernel's version also saves RFLAGS and
 * clears IF for the duration, which a host process has neither the ability nor
 * the need to do -- so lock/lock_noirq are the same code here and a test cannot
 * observe the interrupt-state half of the real contract. Mutual exclusion
 * between the pthreads that stand in for CPUs is preserved, which is the part
 * the tests exercise.
 *
 * Contract notes for the family. Every entry point tolerates a NULL lock:
 * spinlock_try_lock reports failure, the others do nothing, so a NULL lock is
 * silent rather than fatal. spinlock_try_lock returns 1 on acquisition and 0
 * when the lock is already held or the pointer is NULL -- non-zero is success,
 * as in the real declaration. spinlock_lock spins on that same exchange and does
 * not return until it holds the lock, and is not recursive: a second acquisition
 * from the same pthread hangs, which is the real lock's failure mode too.
 *
 * Two divergences a suite can be misled by. The spin here uses a full memory
 * barrier where the kernel uses the PAUSE spin hint, so contention costs more
 * host cycles and the timing profile of a contended section is not the target's.
 * And since lock and lock_noirq are the same code, so are the two releases -- an
 * acquisition taken with one can be released with the other and nothing here
 * notices, whereas on target only spinlock_unlock lowers the per-CPU preempt
 * depth that spinlock_lock raised. A mispaired acquire/release passes on the
 * host. */
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
