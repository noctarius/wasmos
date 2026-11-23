/* cpu_local_arch.h - aarch64 part of the per-CPU block and the TPIDR_EL1
 * self-pointer read behind cpu_local(). Included only through
 * arch/cpu_local_arch.h. */
#pragma once

#include <stdint.h>

struct cpu_local;

/* Embedded in cpu_local_t as `arch`. Only code under arch/aarch64/ touches
 * these fields. */
typedef struct {
    uint64_t mpidr; /* MPIDR_EL1 affinity of this CPU (Aff3..Aff0) */
} arch_cpu_t;

/* The calling CPU's cpu_local_t, read from TPIDR_EL1. Valid once cpu_init()
 * has written TPIDR_EL1; EL0 cannot read or write it. */
static inline struct cpu_local* arch_cpu_local_self(void) {
    struct cpu_local* p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}
