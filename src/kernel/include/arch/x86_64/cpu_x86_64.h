#ifndef WASMOS_ARCH_X86_64_CPU_X86_64_H
#define WASMOS_ARCH_X86_64_CPU_X86_64_H

#include <stdint.h>

#define GDT_ENTRY_COUNT    7
#define CPU_IST_STACK_SIZE 16384u

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

void x86_cpu_init(void);
void x86_cpu_set_kernel_stack(uint64_t rsp0);
void x86_cpu_relocate_tables_high(void);
void x86_cpu_enable_interrupts(void);
void x86_cpu_disable_interrupts(void);

#if WASMOS_SMP
struct cpu_local;
/* Initialise the AP cpu_local slot: copy GDT template, zero TSS, set stack
 * tops, encode TSS into GDT.  Called from the BSP before sending SIPI. */
void x86_cpu_prepare_ap(struct cpu_local *cpu, uint64_t ist1_top, uint64_t rsp0_top);
/* Load the per-CPU GDT, IDT, TSS, and GS base on the calling AP.
 * Must be called from the AP itself after it reaches 64-bit long mode. */
void x86_ap_cpu_init(uint32_t cpu_id);
#endif

#endif
