#pragma once

#include <stdint.h>
#include "arch/x86_64/cpu_x86_64.h"
#include "spinlock.h"
#include "process.h"
#include "thread.h"

/*
 * Per-CPU data structure and accessor.
 *
 * g_cpus[0] is the BSP; g_cpus[1..g_cpu_count-1] are APs (when WASMOS_SMP).
 * cpu_local() returns a pointer to the calling CPU's slot. On a non-SMP build
 * it always returns &g_cpus[0]; on an SMP build it reads GS:0 (the self-
 * pointer written during per-CPU init).
 *
 * NOTE: g_in_context_switch remains a standalone global because context_switch.S
 * writes it via RIP-relative addressing. It will migrate to per-CPU when the
 * assembly is updated to use GS-relative addressing.
 * sched_ctx was moved here; process_preempt_trampoline uses the C helper
 * cpu_local_sched_ctx() to obtain its address without needing an asm offset.
 */

#define WASMOS_MAX_CPUS 16

typedef struct cpu_local {
    /* Self-pointer at offset 0 — read via GS:0 for lock-free per-CPU access. */
    struct cpu_local    *self;

    /* CPU identity */
    uint32_t             cpu_id;    /* logical index 0..N-1 */
    uint32_t             apic_id;   /* hardware LAPIC ID from MADT */

    /* Startup synchronisation: AP sets to 1 after full per-CPU init. */
    volatile uint8_t     started;

    /* Per-CPU x86 descriptor tables.
     * Interrupt stacks (IST1 / RSP0) are separate per-CPU allocations whose
     * base addresses are stored in cpu->tss.ist1 / cpu->tss.rsp0.  For the
     * BSP these are static arrays in cpu_x86_64.c; for APs they are allocated
     * at SMP bring-up time. */
    uint64_t    gdt[GDT_ENTRY_COUNT];
    tss_t       tss;

    /* Scheduler state (previously file-static globals in process.c). */
    process_t         *current_process;
    thread_t          *current_thread;
    uint32_t           preempt_disable_count;
    uint32_t           pm_preempt_safe_depth;
    volatile uint8_t   in_scheduler;

    /* Per-CPU IRQ-disable nesting (moved from spinlock.c globals for SMP safety). */
    volatile uint32_t  irq_disable_depth;
    uint64_t           irq_saved_flags;

    /* Protects the ready-queue (needed under SMP for cross-CPU wake-ups). */
    spinlock_t         ready_queue_lock;

    /* Scheduler context — saved here on every context switch so concurrent CPUs
     * cannot clobber each other's return frame (was a shared global g_sched_ctx). */
    process_context_t  sched_ctx;

    /* Per-CPU reschedule flag and watchdog state (formerly file-static globals). */
    volatile uint8_t   need_resched;
    uint32_t           current_pid;
    uint64_t           resched_pending_since_tick;
    uint64_t           resched_stall_reports;

    /* Round-robin scheduling hint and last-run classification (per-CPU). */
    uint32_t           last_index;
    process_run_result_t last_run_result;
} cpu_local_t;

extern cpu_local_t g_cpus[WASMOS_MAX_CPUS];
extern uint32_t    g_cpu_count;   /* CPUs discovered in MADT; always >= 1 */

/*
 * Return a pointer to the calling CPU's cpu_local_t.
 *
 * SMP build: reads GS:0 (the self-pointer written at per-CPU init time).
 * Non-SMP build: always &g_cpus[0], no GS dependency.
 */
#if WASMOS_SMP
static inline cpu_local_t *
cpu_local(void)
{
    cpu_local_t *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}
#else
static inline cpu_local_t *
cpu_local(void)
{
    return &g_cpus[0];
}
#endif

/*
 * SMP init API.  Both functions are no-ops in a non-SMP build.
 *
 * smp_init()     — called early in kmain, after LAPIC + I/O APIC are live.
 *                  Records BSP APIC ID; APs were discovered by ioapic_init().
 * smp_cpus_up()  — called late in kmain, after the scheduler is ready.
 *                  Sends INIT-SIPI-SIPI to each AP and waits for started flag.
 * smp_ap_c_entry — C entry point called by the AP trampoline (WASMOS_SMP only).
 */
#if WASMOS_SMP
void smp_init(void);
void smp_cpus_up(void);
void smp_ap_c_entry(uint32_t cpu_id);
#else
static inline void smp_init(void)    {}
static inline void smp_cpus_up(void) {}
#endif
