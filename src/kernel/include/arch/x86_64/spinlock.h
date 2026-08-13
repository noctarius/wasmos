/* spinlock.h - Busy-wait mutual exclusion for SMP-safe kernel critical sections.
 * Callers must disable preemption (preempt_disable/cli) before acquiring a spinlock
 * to prevent priority inversion and deadlock on the same CPU. */
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

/* state == 0 means unlocked; state == 1 means locked. */
typedef struct {
    volatile uint32_t state;
} spinlock_t;

void spinlock_init(spinlock_t* lock);
/* Try to acquire the lock without spinning; returns 1 on success, 0 if already held. */
int spinlock_try_lock(spinlock_t* lock);
/* Spin until the lock is acquired.  Saves RFLAGS and calls cli (IF=0 while held). */
void spinlock_lock(spinlock_t* lock);
void spinlock_unlock(spinlock_t* lock);
/* Variants that do NOT disable hardware interrupts.  Use only for long-lived locks
 * (e.g. runtime_lock held for an entire WASM process timeslice) where cli would
 * permanently suppress device IRQ delivery on the holding CPU. */
void spinlock_lock_noirq(spinlock_t* lock);
void spinlock_unlock_noirq(spinlock_t* lock);

#endif
