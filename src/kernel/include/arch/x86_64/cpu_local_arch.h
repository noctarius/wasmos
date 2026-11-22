/* cpu_local_arch.h - x86_64 part of the per-CPU block: descriptor tables, the
 * hardware CPU identity, and the GS-relative self-pointer read behind
 * cpu_local(). Included only through arch/cpu_local_arch.h. */
#pragma once

#include <stdint.h>
#include "arch/x86_64/cpu_x86_64.h"

struct cpu_local;

/* Embedded in cpu_local_t as `arch`. Only code under arch/x86_64/ touches these
 * fields.
 * Interrupt stacks (IST1 / RSP0) are separate per-CPU allocations whose top
 * addresses are stored in tss.ist1 / tss.rsp0. For the BSP these are static
 * arrays in cpu_x86_64.c; for APs they are allocated at SMP bring-up time. */
typedef struct {
    uint32_t apic_id; /* hardware LAPIC ID from MADT */
    uint64_t gdt[GDT_ENTRY_COUNT];
    tss_t tss;
} arch_cpu_t;

/* The calling CPU's cpu_local_t, read from GS:0. Valid once cpu_init()
 * (BSP) or x86_ap_cpu_init() (AP) has loaded the GS base; the self-pointer at
 * offset 0 of cpu_local_t is what GS:0 resolves to. */
static inline struct cpu_local* arch_cpu_local_self(void) {
    struct cpu_local* p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}
