/* arch_cpu.h - aarch64 implementation of the per-CPU control primitives:
 * interrupt masking (PSTATE.DAIF), wait-for-interrupt, halt and the spin-wait
 * hint. Included only through arch/arch_cpu.h. Every function acts on the
 * CALLING CPU only. "Interrupts" here are IRQs (DAIF.I); FIQ is unused and
 * stays masked from entry.S on. */
#pragma once

#include <stdint.h>

/* ELF e_machine of code this kernel can run natively (EM_AARCH64): native
 * drivers and ring-3 native images must carry it. */
#define ARCH_ELF_MACHINE 183u

/* Saved interrupt state as returned by arch_irq_save(): the DAIF register.
 * Opaque to callers; only arch_irq_restore() interprets it. */
typedef uint64_t arch_irq_flags_t;

/* Spin-loop relax hint. ISB stalls the pipeline for a comparable delay to x86
 * PAUSE; YIELD is a NOP on cores without SMT (see the x86_64 header). It adds
 * no memory ordering. */
static inline void arch_cpu_relax(void) {
    __asm__ volatile("isb sy" ::: "memory");
}

/* Return the current interrupt state and mask IRQs, as one step: no IRQ is
 * taken between the read and the mask. */
static inline arch_irq_flags_t arch_irq_save(void) {
    arch_irq_flags_t flags;
    __asm__ volatile("mrs %0, daif\n"
                     "msr daifset, #2"
                     : "=r"(flags)::"memory");
    return flags;
}

/* Restore the interrupt state captured by arch_irq_save(). */
static inline void arch_irq_restore(arch_irq_flags_t flags) {
    __asm__ volatile("msr daif, %0" ::"r"(flags) : "memory");
}

/* Unmask / mask IRQs unconditionally; neither nests. */
static inline void arch_irq_enable(void) {
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void arch_irq_disable(void) {
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/* Wait for an interrupt and take it, with no lost-wakeup window. WFI wakes on
 * a pending IRQ even while PSTATE.I masks it (ARM ARM D1.6.2), so the wait
 * runs masked and the unmask afterwards takes the waking IRQ: an IRQ that
 * arrives between the caller's decision to idle and the WFI is still pending
 * at the WFI and ends it at once. Returns with IRQs enabled. */
static inline void arch_wait_for_interrupt(void) {
    __asm__ volatile("msr daifset, #2\n"
                     "dsb sy\n"
                     "wfi\n"
                     "msr daifclr, #2" ::
                         : "memory");
}

/* Ask every other online CPU to stop (SGI-based on aarch64). SMP builds only;
 * implemented out of line. */
void arch_stop_other_cpus(void);

/* Mask every exception class and stop this CPU permanently. */
static inline __attribute__((noreturn)) void arch_halt_forever(void) {
    for (;;) {
        __asm__ volatile("msr daifset, #0xf\n"
                         "wfi" ::
                             : "memory");
    }
}
