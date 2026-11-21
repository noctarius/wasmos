/* cpu.h - per-CPU processor setup. Implemented by the architecture (x86_64:
 * arch/x86_64/cpu_x86_64.c). Every entry point acts on the CALLING CPU only. */
#ifndef WASMOS_CPU_H
#define WASMOS_CPU_H

#include <stdint.h>

/* Bring the boot CPU's processor state up -- exception and interrupt entry,
 * the per-CPU slot behind cpu_local(), and the early interrupt controller
 * (irq_init) -- with interrupts still masked. Call once, early; cpu_local()
 * must not be used before it returns. x86_64: NX in IA32_EFER, kernel SIMD,
 * the BSP's GDT/TSS, the shared IDT and the GS base. */
void cpu_init(void);

/* Re-point every processor structure the boot CPU reaches by address at its
 * higher-half alias, so none is reached through a low address once the low
 * identity map goes away. Boot CPU only, and idempotent. x86_64: GDTR/IDTR,
 * the TSS descriptor base, the TSS stack tops and the GS base. */
void cpu_relocate_tables_high(void);

/* Set the kernel stack top the calling CPU switches to on the next user ->
 * kernel transition. Called by the scheduler on every switch to a thread that
 * can enter user mode. A zero value is ignored, leaving the previous stack in
 * place. x86_64: TSS.rsp0. */
void cpu_set_kernel_stack(uint64_t rsp0);

#endif
