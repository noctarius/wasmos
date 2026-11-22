/* smp.h - SMP bring-up entry points. Implemented per architecture under
 * arch/<arch>/smp.c. Per-CPU state is in cpu_local.h. */
#pragma once

#include <stdint.h>

/*
 * SMP init API.  Both functions are no-ops in a non-SMP build.
 *
 * smp_init()     — called early in kmain, after the interrupt controller is
 *                  live. Records the boot CPU's hardware ID; secondary CPUs
 *                  were discovered from firmware tables by then.
 * smp_cpus_up()  — called late in kmain, after the scheduler is ready.
 *                  Starts each secondary CPU and waits for its started flag.
 * smp_ap_c_entry — C entry point a secondary CPU reaches from its start-up
 *                  path (WASMOS_SMP only).
 */
#if WASMOS_SMP
void smp_init(void);
void smp_cpus_up(void);
void smp_ap_c_entry(uint32_t cpu_id);
#else
static inline void smp_init(void) {}
static inline void smp_cpus_up(void) {}
#endif
