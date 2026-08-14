#include "arch/x86_64/spinlock.h"
#include "process.h"
#include "arch/x86_64/smp.h"

/*
 * Interrupt-safe spinlock implementation for WASMOS.
 *
 * spinlock_lock() saves RFLAGS and clears IF (cli) on first entry;
 * spinlock_unlock() restores RFLAGS on last exit.  A lock taken through that
 * pair is therefore held with IF=0: no hardware interrupt can fire on the same
 * CPU while it is held, eliminating the classic single-CPU IRQ deadlock where
 * an IRQ handler tries to acquire a lock already held by the interrupted
 * thread.  The _noirq pair and spinlock_try_lock deliberately opt out of that
 * guarantee; see their own notes below.
 *
 * The IRQ-disable depth counter lives in cpu_local_t so each CPU tracks its
 * own interrupt state independently under SMP.
 *
 * preempt_disable()/preempt_enable() are intentionally separate: wasm_driver.c
 * wraps wasm3 execution in preempt_disable(), and coupling that to IRQ-disable
 * would block interrupt delivery for the whole WASM process lifetime.
 *
 * Same-CPU recursive acquisition is not supported and will deadlock.  For a
 * lock taken via spinlock_lock() the holder runs with IF=0, so an IRQ handler
 * cannot interrupt it on the same CPU and accidental same-CPU reentry is
 * structurally impossible for the IRQ-driven send paths.  Any reentry that does
 * occur is a bug and the deadlock is the correct loud failure mode.
 */

/*
 * Save RFLAGS (capturing IF) and disable interrupts.  On the first nesting
 * level (depth 0→1) the saved RFLAGS are stored for later restoration.
 * Subsequent nested calls (depth already ≥1) do cli but discard the flags
 * because IF is already 0 and the saved original value must be preserved.
 *
 * The pushfq+cli sequence is performed BEFORE checking/incrementing the depth
 * counter to close the window where IF=1 but depth>0.
 */
static inline void spinlock_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    cpu_local_t* cpu = cpu_local();
    if (cpu->irq_disable_depth++ == 0) {
        cpu->irq_saved_flags = flags;
    }
}

/*
 * Decrement the IRQ-disable depth.  On the final exit (depth 1→0) restore
 * RFLAGS, which re-enables interrupts if they were enabled before the first
 * spinlock_irq_save().
 */
static inline void spinlock_irq_restore(void) {
    cpu_local_t* cpu = cpu_local();
    if (cpu->irq_disable_depth == 0) {
        return;
    }
    if (--cpu->irq_disable_depth == 0) {
        __asm__ volatile("push %0; popfq" ::"r"(cpu->irq_saved_flags) : "memory");
    }
}

/* Publishes the lock as free.  Not atomic and not safe to run against a live
 * lock: it is for a freshly allocated or embedded lock only, before any other
 * CPU can reach it.  A NULL lock is ignored, as everywhere else in this file. */
void spinlock_init(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    lock->state = 0;
}

/* Returns non-zero if the lock was acquired.  This is the bare exchange: it does
 * NOT run spinlock_irq_save() or preempt_disable(), so a caller that acquires
 * through it must release with spinlock_unlock_noirq().  Releasing with
 * spinlock_unlock() would decrement a preempt depth this acquisition never
 * raised.  (spinlock_lock and spinlock_lock_noirq call it after establishing
 * their own IRQ/preempt state.) */
int spinlock_try_lock(spinlock_t* lock) {
    if (!lock) {
        return 0;
    }
    return __sync_lock_test_and_set(&lock->state, 1u) == 0u;
}

/* Spins until acquired; returns holding the lock with IF=0 and preemption
 * disabled one level deeper.  Must be paired with spinlock_unlock.
 *
 * The IRQ/preempt state is established and then RELEASED again on every failed
 * attempt rather than held across the whole wait.  Backing out is what keeps the
 * wait harmless: the pause loop otherwise runs with interrupts masked for as
 * long as a remote holder takes, delaying every device IRQ on this CPU, and
 * leaves the preempt depth raised so this CPU cannot even be rescheduled onto
 * other work.  A NULL lock returns immediately WITHOUT acquiring anything, so a
 * caller that ignores it and calls spinlock_unlock(NULL) stays balanced. */
void spinlock_lock(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    for (;;) {
        spinlock_irq_save();
        preempt_disable();
        if (spinlock_try_lock(lock)) {
            return;
        }
        preempt_enable();
        spinlock_irq_restore();
        __asm__ volatile("pause");
    }
}

/* Undoes spinlock_lock in the exact reverse order: the lock word is released
 * first, then the preempt depth, then RFLAGS.  Releasing the word last would
 * leave a window in which this CPU has already re-enabled interrupts (and can
 * take an IRQ, or be preempted) while still holding the lock. */
void spinlock_unlock(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    __sync_lock_release(&lock->state);
    preempt_enable();
    spinlock_irq_restore();
}

/* No-IRQ variants: acquire/release WITHOUT touching the irq_disable_depth counter
 * or calling cli/sti.  Safe only when no interrupt handler on this CPU will ever
 * try to acquire the same lock (which is the case for runtime_lock, held across an
 * entire WASM process timeslice — using the regular spinlock_lock would keep cli
 * active for the whole timeslice and permanently suppress keyboard/mouse IRQs). */
void spinlock_lock_noirq(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    for (;;) {
        if (spinlock_try_lock(lock)) {
            return;
        }
        __asm__ volatile("pause");
    }
}

void spinlock_unlock_noirq(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    __sync_lock_release(&lock->state);
}
