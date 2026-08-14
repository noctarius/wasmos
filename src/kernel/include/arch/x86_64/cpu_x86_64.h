/* cpu_x86_64.h - x86_64 CPU state: GDT, IDT, TSS, and per-CPU data declarations. */
#ifndef WASMOS_ARCH_X86_64_CPU_X86_64_H
#define WASMOS_ARCH_X86_64_CPU_X86_64_H

#include <stdint.h>

/* Per-CPU GDT length: null, kernel code, kernel data, user code, user data, then two
 * slots for the 16-byte 64-bit TSS descriptor, which occupies two 8-byte entries (Intel
 * SDM, "System Descriptor Types" / "TSS Descriptor in 64-bit mode"). */
#define GDT_ENTRY_COUNT 7
/* Bytes per interrupt stack: both the TSS's rsp0 (ring-3 -> ring-0 transitions) and the
 * IST1 stack used by the timer gate.  Each stack's lowest 8 bytes hold a canary that
 * cpu_isr.S checks on entry, so an overflow is detected instead of corrupting whatever
 * precedes the stack. */
#define CPU_IST_STACK_SIZE 16384u

/* 64-bit Task State Segment, laid out exactly as the hardware reads it (Intel SDM, "Task
 * Management in 64-bit Mode" / "64-Bit TSS Format"): it is not a task-switch structure
 * here, only the place the CPU finds a stack pointer for a privilege or IST transition.
 * rsp0 is the stack loaded on a ring-3 -> ring-0 transition; rsp1/rsp2 are unused because
 * rings 1 and 2 are not.  ist1..ist7 are the interrupt-stack-table entries an IDT gate can
 * select by index; only IST1 is populated.  All of these are kernel virtual addresses of
 * the stack TOP, since the stack grows down.  `iopb` is the offset to the I/O permission
 * bitmap and is set to sizeof(tss_t), i.e. past the end of the segment, which denies all
 * port access from ring 3.  The reserved fields must stay zero. */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
} tss_t;

/* Bring the bootstrap processor's CPU state up: enable NX in IA32_EFER and kernel SIMD,
 * fill the BSP's per-CPU slot, install its GDT/TSS and the shared IDT with the exception
 * and IRQ gates, and load the GS base that makes cpu_local() usable.  Call once, early;
 * cpu_local() must not be used before it returns. */
void x86_cpu_init(void);

/* Point the calling CPU's TSS.rsp0 at `rsp0`, the kernel stack top the CPU will switch to
 * on the next ring-3 -> ring-0 transition.  Called by the scheduler on every switch to a
 * thread that can enter ring 3.  A zero rsp0 is ignored, leaving the previous stack in
 * place. */
void x86_cpu_set_kernel_stack(uint64_t rsp0);

/* Re-point GDTR/IDTR, the TSS descriptor base, the TSS stack tops and the GS base at the
 * higher-half aliases of the same objects, so no descriptor table or interrupt stack is
 * reached through a low address once the low identity map goes away.  BSP only, and
 * idempotent: addresses already in the higher half are left unchanged. */
void x86_cpu_relocate_tables_high(void);

/* STI / CLI on the calling CPU.  These do not nest — a disable followed by two enables
 * leaves interrupts on — so a region that must survive nesting needs the caller's own
 * saved-flags discipline. */
void x86_cpu_enable_interrupts(void);
void x86_cpu_disable_interrupts(void);

#if WASMOS_SMP
struct cpu_local;
/* Initialise the AP cpu_local slot: copy GDT template, zero TSS, set stack
 * tops, encode TSS into GDT.  Called from the BSP before sending SIPI. */
void x86_cpu_prepare_ap(struct cpu_local* cpu, uint64_t ist1_top, uint64_t rsp0_top);
/* Load the per-CPU GDT, IDT, TSS, and GS base on the calling AP.
 * Must be called from the AP itself after it reaches 64-bit long mode. */
void x86_ap_cpu_init(uint32_t cpu_id);
#endif

#endif
