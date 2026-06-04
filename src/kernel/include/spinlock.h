/* spinlock.h - Busy-wait mutual exclusion for SMP-safe kernel critical sections.
 * Callers must disable preemption (preempt_disable/cli) before acquiring a spinlock
 * to prevent priority inversion and deadlock on the same CPU. */
#ifndef WASMOS_SPINLOCK_H
#define WASMOS_SPINLOCK_H

#include <stdint.h>

/* state == 0 means unlocked; state == 1 means locked. */
typedef struct {
    volatile uint32_t state;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
/* Try to acquire the lock without spinning; returns 1 on success, 0 if already held. */
int spinlock_try_lock(spinlock_t *lock);
/* Spin until the lock is acquired. */
void spinlock_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock);

#endif
