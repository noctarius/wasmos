#include "spinlock.h"
#include "process.h"
#include "arch/x86_64/smp.h"

/*
 * Interrupt-safe spinlock implementation for WASMOS.
 *
 * Problem: ipc_send_from() is called from hardware interrupt context (IRQ
 * handler, IF=0) and needs ep->lock, which ipc_recv_for() may hold while
 * running with IF=1.  The IRQ handler would spin forever waiting for the
 * interrupted thread to release the lock — a classic single-CPU IRQ deadlock.
 *
 * Fix: maintain a separate per-CPU IRQ-disable depth counter alongside the
 * existing preempt-disable counter.  spinlock_lock() saves RFLAGS and clears
 * IF on first entry (depth 0→1); spinlock_unlock() restores RFLAGS on last
 * exit (depth 1→0).  This guarantees that every spinlock is held with IF=0,
 * so no hardware interrupt can observe a held spinlock.
 *
 * The counters live in cpu_local_t (per-CPU) so that under SMP each CPU's
 * interrupt-disable state is independent.  On a non-SMP build cpu_local()
 * returns &g_cpus[0] so behaviour is identical to the old single-global form.
 *
 * preempt_disable()/preempt_enable() are NOT changed: wasm_driver.c wraps the
 * entire wasm3 execution in preempt_disable(), and making that also disable
 * interrupts would block IRQ delivery for the whole WASM process lifetime.
 * The two depth counters are intentionally independent.
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
static inline void spinlock_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    cpu_local_t *cpu = cpu_local();
    if (cpu->irq_disable_depth++ == 0) {
        cpu->irq_saved_flags = flags;
    }
}

/*
 * Decrement the IRQ-disable depth.  On the final exit (depth 1→0) restore
 * RFLAGS, which re-enables interrupts if they were enabled before the first
 * spinlock_irq_save().
 */
static inline void spinlock_irq_restore(void)
{
    cpu_local_t *cpu = cpu_local();
    if (cpu->irq_disable_depth == 0) {
        return;
    }
    if (--cpu->irq_disable_depth == 0) {
        __asm__ volatile("push %0; popfq" :: "r"(cpu->irq_saved_flags) : "memory");
    }
}

void spinlock_init(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    lock->state = 0;
    lock->owner_cpu = 0xFFFFFFFFu;
    lock->recursion_depth = 0;
}

int spinlock_try_lock(spinlock_t *lock) {
    if (!lock) {
        return 0;
    }
    if (__sync_lock_test_and_set(&lock->state, 1u) == 0u) {
        lock->owner_cpu = cpu_local()->cpu_id;
        lock->recursion_depth = 1;
        return 1;
    }
    return 0;
}

void spinlock_lock(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    for (;;) {
        spinlock_irq_save();
        preempt_disable();
        /* TODO(smp): remove recursive same-CPU acquisition once the endpoint
         * table reentry path is fully eliminated. This is a debugging-time
         * liveness hardening guard, not the desired long-term lock model. */
        if (lock->state != 0u && lock->owner_cpu == cpu_local()->cpu_id) {
            lock->recursion_depth++;
            return;
        }
        if (spinlock_try_lock(lock)) {
            return;
        }
        preempt_enable();
        spinlock_irq_restore();
        __asm__ volatile("pause");
    }
}

void spinlock_unlock(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    if (lock->state != 0u &&
        lock->owner_cpu == cpu_local()->cpu_id &&
        lock->recursion_depth > 1u) {
        lock->recursion_depth--;
        preempt_enable();
        spinlock_irq_restore();
        return;
    }
    lock->recursion_depth = 0;
    lock->owner_cpu = 0xFFFFFFFFu;
    __sync_lock_release(&lock->state);
    preempt_enable();
    spinlock_irq_restore();
}
