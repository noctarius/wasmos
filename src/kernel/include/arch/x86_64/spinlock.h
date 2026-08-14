/* spinlock.h - Busy-wait mutual exclusion for SMP-safe kernel critical sections.
 *
 * spinlock_lock() establishes its own IRQ and preemption state -- callers do
 * NOT wrap it in preempt_disable()/cli. The IRQ-disable depth is per CPU, so
 * nesting different locks is fine.
 *
 * Same-CPU recursive acquisition of one lock is not supported and deadlocks.
 * That is the intended failure mode: for a lock taken via spinlock_lock() the
 * holder runs with IF=0, so an IRQ handler cannot reenter it on this CPU, and
 * any reentry that does happen is a bug worth failing loudly on. */
#ifndef WASMOS_ARCH_X86_64_SPINLOCK_H
#define WASMOS_ARCH_X86_64_SPINLOCK_H

#include <stdint.h>

/* Spin-loop relax hint: signals to the core that this is a busy-wait, so it can
 * back off instead of burning the pipeline at full rate.
 *
 * x86_64 uses PAUSE, the architectural spin-wait hint.
 *
 * aarch64 has no PAUSE equivalent, and YIELD is NOT one despite the widespread
 * claim: the ARM ARM defines it as a hint that this task may be swapped out, for
 * hardware multithreading, and it is permitted to (and does) execute as a NOP on
 * cores without SMT.  ISB SY is what actually produces a comparable delay -- it
 * stalls the pipeline.  It is an *instruction* synchronization barrier, not a
 * data barrier like DMB/DSB, so it adds no memory ordering and cannot mask a
 * missing acquire/release in code being tested on an aarch64 host.
 *
 * aarch64 is reachable only through the host test harness; the kernel itself is
 * x86_64-only.  A real aarch64 port should revisit this against the LDXR/WFE
 * exclusive-monitor pattern, which is the architectural low-power wait. */
static inline void cpu_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("isb sy" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

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
 * for a NULL lock). This is the bare exchange: it does NOT save RFLAGS, clear
 * IF, or disable preemption, so an acquisition made through it must be released
 * with spinlock_unlock_noirq(). Releasing it with spinlock_unlock() would
 * decrement a preempt depth this acquisition never raised. */
int spinlock_try_lock(spinlock_t* lock);

/* Spin until acquired. Saves RFLAGS, clears IF, and disables preemption, so the
 * lock is held with interrupts off on this CPU; the matching spinlock_unlock()
 * reverses both. Interrupts are re-enabled between failed attempts, so IF is
 * only forced low once the lock is actually held. */
void spinlock_lock(spinlock_t* lock);
void spinlock_unlock(spinlock_t* lock);

/* Variants that do NOT disable hardware interrupts or preemption.  Use only for
 * long-lived locks (e.g. the runtime lock held for an entire WASM process
 * timeslice) where cli would suppress device IRQ delivery on the holding CPU
 * for that whole span. Safe only when no interrupt handler on this CPU can ever
 * try to acquire the same lock. */
void spinlock_lock_noirq(spinlock_t* lock);
void spinlock_unlock_noirq(spinlock_t* lock);

#endif
