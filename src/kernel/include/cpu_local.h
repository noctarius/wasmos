/* cpu_local.h - per-CPU state and the cpu_local() accessor. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "arch/cpu_local_arch.h"
#include "sync/spinlock.h"
#include "process.h"
#include "thread.h"
#include "sched.h"

/*
 * Per-CPU data structure and accessor.
 *
 * g_cpus[0] is the BSP; g_cpus[1..g_cpu_count-1] are APs (when WASMOS_SMP).
 * cpu_local() returns a pointer to the calling CPU's slot. On a non-SMP build
 * it always returns &g_cpus[0]; on an SMP build it reads the architecture's
 * per-CPU register (arch_cpu_local_self()), which resolves to the self-pointer
 * written during per-CPU init.
 *
 * sched_ctx is stored here; process_preempt_trampoline uses the C helper
 * cpu_local_sched_ctx() to obtain its address without needing an asm offset.
 */

#define WASMOS_MAX_CPUS 16

typedef struct cpu_local {
    /* Self-pointer at offset 0 — what the per-CPU register resolves to. */
    struct cpu_local* self;

    /* Logical CPU index 0..N-1. The hardware identity lives in `arch`. */
    uint32_t cpu_id;

    /* Startup synchronisation: AP sets to 1 after full per-CPU init. */
    volatile uint8_t started;

    /* Set to 1 while context_switch / context_switch_to are executing on this
     * CPU.  Used by process_preempt_from_irq to suppress timer preemption
     * while register state is mid-save/restore.  Its offset is encoded in the
     * architecture's context-switch assembly; see the assert below. */
    volatile uint8_t in_context_switch;

    /* Architecture-private per-CPU state (descriptor tables, hardware CPU ID).
     * Only code under arch/ reads or writes it. */
    arch_cpu_t arch;

    /* Scheduler state, per CPU. */
    process_t* current_process;
    thread_t* current_thread;
    uint32_t preempt_disable_count;
    uint32_t pm_preempt_safe_depth;
    volatile uint8_t in_scheduler;

    /* Per-CPU IRQ-disable nesting (moved from spinlock.c globals for SMP safety). */
    volatile uint32_t irq_disable_depth;
    uint64_t irq_saved_flags;

    /* Per-CPU ready queues (one FIFO per priority level).
     * Threads are enqueued on the waking CPU's sched, giving IPC callers and
     * their receivers cache-local scheduling without cross-CPU queue traffic. */
    cpu_sched_t sched;

    /* Cumulative count of threads successfully stolen from other CPUs. */
    uint32_t steal_count;

    /* Cumulative count of threads dispatched by this CPU (context switches
     * out of the scheduler).  Monotonically increasing; never reset. */
    uint32_t dispatch_count;

    /* PID of the last process dispatched by this CPU.  Set on dispatch and
     * never cleared, so it remains visible when the CPU is between threads. */
    uint32_t last_dispatched_pid;

    /* Scheduler context — saved here on every context switch so concurrent CPUs
     * cannot clobber each other's return frame; a shared global cannot do this. */
    process_context_t sched_ctx;

    /* Per-CPU reschedule flag and watchdog state. */
    volatile uint8_t need_resched;
    uint32_t current_pid;
    uint64_t resched_pending_since_tick;
    uint64_t resched_stall_reports;

    /* TODO: last_index is written on dispatch and read nowhere; the scheduler
     * picks by priority band, not by a round-robin cursor. Drop it or start
     * using it. last_run_result records how the last dispatch on this CPU
     * ended. */
    uint32_t last_index;
    process_run_result_t last_run_result;

    /* Per-CPU wasm3 heap PID override.  wasm3_heap_bind_pid() sets this so
     * that wasm3 module loading (which calls malloc) targets a specific
     * process's heap regardless of which process is currently scheduled.
     * Making it per-CPU prevents an AP loading a driver from diverting
     * another CPU's malloc to the wrong heap slot. */
    uint32_t wasm3_heap_bound_pid;

    /* Per-CPU idle thread: dispatched only when the ready queue is empty.
     * Set by process_spawn_idle (BSP) and process_spawn_idle_ap (APs).
     * Never placed in the global ready queue; always dispatched via the
     * cpu_sched_pick_next fallback path. */
    thread_t* idle_thread;
} cpu_local_t;

/* arch/x86_64/context_switch.S encodes this offset as
 * CPU_LOCAL_IN_CTX_SW_OFFSET. If the assert fires, update the .set there. */
_Static_assert(offsetof(cpu_local_t, in_context_switch) == 13,
               "cpu_local_t in_context_switch offset changed; update context_switch.S");

extern cpu_local_t g_cpus[WASMOS_MAX_CPUS];
extern uint32_t g_cpu_count; /* CPUs discovered in MADT; always >= 1 */

/*
 * Return a pointer to the calling CPU's cpu_local_t.
 *
 * SMP build: the architecture's per-CPU register (arch_cpu_local_self()).
 * Non-SMP build: always &g_cpus[0], no per-CPU register dependency.
 */
#if defined(WASMOS_HOST_TEST_SMP)
/* Host harness: pthreads stand in for vCPUs, so cpu_local() must be per-thread.
 * The arch-typical per-CPU register is not available to a hosted program --
 * GS/FS base on x86_64 and TPIDR_EL0 on arm64 already hold libc's thread
 * pointer, and macOS uses GS itself -- so reading it raw yields libc's TSD and
 * writing it breaks pthreads.  _Thread_local is the supported spelling of the
 * same thing: the compiler lowers it to an access off exactly that register.
 * Listed first so it wins regardless of how WASMOS_SMP is set for the build. */
extern _Thread_local cpu_local_t* g_host_cpu_local;
static inline cpu_local_t* cpu_local(void) {
    return g_host_cpu_local;
}
#elif WASMOS_SMP
static inline cpu_local_t* cpu_local(void) {
    return arch_cpu_local_self();
}
#else
static inline cpu_local_t* cpu_local(void) {
    return &g_cpus[0];
}
#endif

/* Return the calling CPU's scheduler state. */
static inline cpu_sched_t* cpu_sched(void) {
    return &cpu_local()->sched;
}
