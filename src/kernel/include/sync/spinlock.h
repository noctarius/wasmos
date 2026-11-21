#ifndef WASMOS_SYNC_SPINLOCK_H
#define WASMOS_SYNC_SPINLOCK_H

/* Busy-wait mutual exclusion for SMP-safe kernel critical sections.
 *
 * spinlock_lock() establishes its own IRQ and preemption state -- callers do
 * NOT wrap it in preempt_disable() or an interrupt mask. The IRQ-disable depth
 * is per CPU, so nesting different locks is fine.
 *
 * Same-CPU recursive acquisition of one lock is not supported and deadlocks.
 * That is the intended failure mode: for a lock taken via spinlock_lock() the
 * holder runs with interrupts masked, so an IRQ handler cannot reenter it on
 * this CPU, and any reentry that does happen is a bug worth failing loudly on.
 *
 * The ksync_* wrappers below are the names kernel code uses. The one thing worth
 * repeating, because getting it wrong corrupts the preempt depth rather than
 * failing loudly: an acquisition must be released by its own counterpart.
 * ksync_spinlock_lock (interrupts masked, preemption disabled) pairs with
 * ksync_spinlock_unlock; ksync_spinlock_lock_noirq and, notably,
 * ksync_spinlock_try_lock -- which is the bare exchange and raises no depth --
 * both pair with ksync_spinlock_unlock_noirq. */
#include <stdint.h>

/* state == 0 means unlocked; state == 1 means locked. All-zero storage is
 * therefore a valid unlocked lock, so static instances need no explicit init. */
typedef struct {
    volatile uint32_t state;
} spinlock_t;

/* Put a lock into the unlocked state.  Redundant for zero-initialised storage, and
 * required for a lock built on a stack or in a heap block whose bytes are undefined.
 * Resetting a lock that is currently held silently releases it. */
void spinlock_init(spinlock_t* lock);

/* Try to acquire without spinning; returns 1 on success, 0 if already held (and
 * for a NULL lock). This is the bare exchange: it does NOT save the interrupt
 * state, mask interrupts, or disable preemption, so an acquisition made through
 * it must be released with spinlock_unlock_noirq(). Releasing it with spinlock_unlock() would
 * decrement a preempt depth this acquisition never raised. */
int spinlock_try_lock(spinlock_t* lock);

/* Spin until acquired. Saves the interrupt state, masks interrupts, and disables
 * preemption, so the lock is held with interrupts off on this CPU; the matching
 * spinlock_unlock() reverses both. Interrupts are re-enabled between failed
 * attempts, so they are only masked once the lock is actually held. */
void spinlock_lock(spinlock_t* lock);
void spinlock_unlock(spinlock_t* lock);

/* Variants that do NOT disable hardware interrupts or preemption.  Use only for
 * long-lived locks (e.g. the runtime lock held for an entire WASM process
 * timeslice) where masking interrupts would suppress device IRQ delivery on the
 * holding CPU for that whole span. Safe only when no interrupt handler on this CPU can ever
 * try to acquire the same lock. */
void spinlock_lock_noirq(spinlock_t* lock);
void spinlock_unlock_noirq(spinlock_t* lock);

/* All-zero storage is a valid unlocked lock, so a static or zeroed-struct
 * instance needs no explicit ksync_spinlock_init. */
typedef spinlock_t ksync_spinlock_t;

static inline void ksync_spinlock_init(ksync_spinlock_t* lock) {
    spinlock_init(lock);
}

static inline int ksync_spinlock_try_lock(ksync_spinlock_t* lock) {
    return spinlock_try_lock(lock);
}

static inline void ksync_spinlock_lock(ksync_spinlock_t* lock) {
    spinlock_lock(lock);
}

static inline void ksync_spinlock_unlock(ksync_spinlock_t* lock) {
    spinlock_unlock(lock);
}

static inline void ksync_spinlock_lock_noirq(ksync_spinlock_t* lock) {
    spinlock_lock_noirq(lock);
}

static inline void ksync_spinlock_unlock_noirq(ksync_spinlock_t* lock) {
    spinlock_unlock_noirq(lock);
}

#endif
