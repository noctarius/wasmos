/* arch_cpu.h - x86_64 implementation of the per-CPU control primitives:
 * interrupt flag, halt, and the spin-wait hint. Included only through
 * arch/arch_cpu.h. Every function acts on the CALLING CPU only.
 *
 * The host unit-test build compiles this header for the host architecture
 * (x86_64 or arm64). Only arch_cpu_relax() is called there, so it alone carries
 * a non-x86 branch; the rest is never emitted on a host build, and its operand
 * constraints are generic so the header still parses on any host. */
#pragma once

#include <stdint.h>

/* Saved interrupt state as returned by arch_irq_save(): the full RFLAGS image.
 * Opaque to callers; only arch_irq_restore() interprets it. */
typedef uint64_t arch_irq_flags_t;

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
 * The aarch64 branch serves the host test harness on an arm64 host. An aarch64
 * kernel port provides its own arch_cpu.h and should revisit this against the
 * LDXR/WFE exclusive-monitor pattern, which is the architectural low-power
 * wait. */
static inline void arch_cpu_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("isb sy" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

/* Return the current interrupt state and mask interrupts. The read and the mask
 * are one uninterruptible sequence from the caller's point of view: no
 * interrupt is taken between them on this CPU. */
static inline arch_irq_flags_t arch_irq_save(void) {
    arch_irq_flags_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

/* Restore the interrupt state captured by arch_irq_save(). Re-enables interrupts
 * exactly when they were enabled at the matching save. */
static inline void arch_irq_restore(arch_irq_flags_t flags) {
    __asm__ volatile("push %0; popfq" ::"r"(flags) : "memory");
}

/* Unmask / mask interrupts unconditionally. Neither nests nor remembers a prior
 * state: a disable followed by an enable leaves interrupts on regardless of
 * what they were before. Code that must restore what it found uses
 * arch_irq_save() / arch_irq_restore(), or the spinlock irq-save helpers. */
static inline void arch_irq_enable(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline void arch_irq_disable(void) {
    __asm__ volatile("cli" ::: "memory");
}

/* Unmask interrupts and wait for one, as a single step: no interrupt can be
 * taken between the unmask and the wait, so a wake-up that arrives while the
 * caller is going idle is never lost. Returns after the waking interrupt has
 * been handled, with interrupts enabled. On x86_64 this is `sti; hlt`, relying
 * on the one-instruction interrupt shadow of `sti`. */
static inline void arch_wait_for_interrupt(void) {
    __asm__ volatile("sti; hlt" ::: "memory");
}

/* Ask every other online CPU to stop: each takes a non-maskable interrupt
 * (an NMI IPI on x86_64), which delivers its interrupted state to kpanic_nmi
 * and halts it. Reaches CPUs spinning with interrupts masked. SMP builds only;
 * implemented in arch/x86_64/smp.c. */
void arch_stop_other_cpus(void);

/* Mask interrupts and stop this CPU permanently. Maskable interrupts cannot
 * wake it; an NMI or SMI that returns lands back in the halt loop. */
static inline __attribute__((noreturn)) void arch_halt_forever(void) {
    for (;;) {
        __asm__ volatile("cli; hlt" ::: "memory");
    }
}
