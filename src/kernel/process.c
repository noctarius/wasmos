#include "process.h"
#include "klog.h"
#include "memory.h"
#include "kpanic.h"
#include "physmem.h"
#include "serial.h"
#include "paging.h"
#include "cpu.h"
#include "wasm3/shim.h"
#include "ipc.h"
#include "irq.h"
#include "msi.h"
#include "timer.h"
#include "thread.h"
#include "process_manager.h"
#include "string.h"
#include "wasm3/shim.h"
#include "native_driver.h"
#include "subsystem_registry.h"

#include "sched.h"
#include "sched_event.h"
#include "futex.h"
#include "arch/x86_64/smp.h"

#ifdef WASMOS_WASM_RUNTIME_WARP
extern int warp_sync_linmem_for_pid(uint32_t pid, uint64_t user_root);
#endif

/*
 * process.c owns the process table, the dispatch loop, and the context-switch
 * glue.  The run queues themselves live in sched_thread.c, one per CPU; this
 * file drives them (process_schedule_once_impl) and holds the process-slot state
 * machine that gates what may be dispatched.
 *
 * Deliberately plain: a fixed-size g_processes[] array and explicit state
 * transitions, all funnelled through process_transit so the lifecycle is
 * auditable in one place.  Processes run in ring 0 or ring 3 depending on the
 * context installed by process_set_user_entry / the user-thread spawn path.
 */

static process_t g_processes[PROCESS_MAX_COUNT];
/* FIXME(process-list): migrate to kernel list storage after providing a
 * boot-safe list allocator path for early scheduler init/spawn. */
static uint32_t g_next_pid;
static ksync_spinlock_t g_process_table_lock;
/* Scheduler state lives in cpu_local_t (per-CPU) — see smp.h. */
static process_t* g_idle_process;
/* in_context_switch is per-CPU (cpu_local_t), not global: a global flag would
 * let one CPU's context switch suppress preemption on every other CPU. */
static uint64_t g_ctx_watch_logged;

static void process_clear_runtime_tag(process_t* proc);
static int process_copy_runtime_tag(process_t* proc, const char* tag);
static uint64_t g_ctx_watch_last_logged_rip;
static uint64_t g_ctx_watch_last_logged_rsp;
static uint64_t g_ctx_watch_last_logged_rflags;
static uint64_t g_ctx_watch_last_logged_reason;
static uint8_t g_preempt_smoke_logged;
static uint8_t g_sched_progress_logged;
static uint64_t g_sched_switch_count;
static uint64_t g_trap_frame_invalid_reports;

static process_t* process_find_by_pid(uint32_t pid);
static void process_trampoline(void);
static int process_spawn_as_internal(uint32_t parent_pid, const char* name, process_entry_t entry,
                                     void* arg, uint32_t* out_pid,
                                     thread_state_t initial_thread_state,
                                     thread_block_reason_t initial_thread_reason,
                                     uint8_t enqueue_initial);

static inline uintptr_t process_kernel_alias_addr(uintptr_t addr) {
#ifdef WASMOS_PROCESS_TEST_SEAMS
    /* A host process has one address space and no higher-half alias, so the
     * alias of an address is the address. Rebasing by KERNEL_HIGHER_HALF_BASE
     * here would turn every host pointer into an unmapped one. */
    return addr;
#else
    uint64_t base = KERNEL_HIGHER_HALF_BASE;
    if ((uint64_t)addr < base) {
        return (uintptr_t)((uint64_t)addr + base);
    }
    return addr;
#endif
}

static inline process_t* process_table(void) {
    return (process_t*)(void*)process_kernel_alias_addr((uintptr_t)&g_processes[0]);
}

volatile uint64_t g_ctxsw_last_out_ctx;
volatile uint64_t g_ctxsw_last_out_rip;
volatile uint64_t g_ctxsw_last_out_rsp;
volatile uint64_t g_ctxsw_last_out_rflags;
volatile uint64_t g_ctxsw_last_in_ctx;
volatile uint64_t g_ctxsw_last_in_rip;
volatile uint64_t g_ctxsw_last_in_rsp;
volatile uint64_t g_ctxsw_last_in_rflags;
volatile uint64_t g_ctx_watch_ctx;
volatile uint64_t g_ctx_watch_last_ctx;
volatile uint64_t g_ctx_watch_last_rip;
volatile uint64_t g_ctx_watch_last_rsp;
volatile uint64_t g_ctx_watch_last_rflags;
volatile uint64_t g_ctx_watch_hits;
volatile uint64_t g_ctx_watch_reason;
volatile uint64_t g_ctx_restore_ctx;
volatile uint64_t g_ctx_restore_rip;
volatile uint64_t g_ctx_restore_rsp;
volatile uint64_t g_ctx_restore_rflags;
volatile uint64_t* g_pm_stack_watch;
extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

#define PAGE_SIZE 0x1000u
#define KERNEL_CS_SELECTOR 0x08u
#define KERNEL_DS_SELECTOR 0x10u
#define USER_CS_SELECTOR 0x1Bu
#define USER_DS_SELECTOR 0x23u
#define STACK_GUARD_PAGES 1u
#define STACK_REDZONE_BYTES 4096u
#define STACK_CANARY_VALUE 0xC0DEC0DEF00DFACEULL
#define SCHED_TRAMPOLINE_STACK_BYTES 8192u
#define SCHED_PROGRESS_MARKER_SWITCHES 256ull
#define SCHED_RESCHED_STALL_TICKS 512ull
/* Kernel stacks must land inside the shared higher-half window so they stay
 * mapped under every process root table.  Must match paging.c's
 * HIGHER_HALF_PDE_COUNT (256 * 2 MiB PDEs = 512 MiB). */
#define KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES (512u * 1024u * 1024u)
static uint8_t g_sched_trampoline_stacks[WASMOS_MAX_CPUS][SCHED_TRAMPOLINE_STACK_BYTES]
    __attribute__((aligned(16)));

/* Owned by no subsystem, so a stale slot read as a code pointer is traceable. */
#define STACK_POISON_VALUE 0xDEAD57ACDEAD57ACULL

static void process_stack_poison(uintptr_t stack_base, uintptr_t stack_top) {
    if (!stack_base || stack_top <= stack_base) {
        return;
    }
    uint64_t* p = ptr_cast(uint64_t, stack_base);
    uint64_t* end = ptr_cast(uint64_t, stack_top);
    while (p < end) {
        *p++ = STACK_POISON_VALUE;
    }
}

static int process_alloc_stack(process_t* slot, uint32_t stack_pages) {
    if (!slot || stack_pages == 0) {
        return -1;
    }
    /*
     * Stacks are allocated as [guard][usable][guard]. The guard pages are
     * unmapped immediately so any overrun turns into a deterministic page fault
     * instead of silent memory corruption.
     */
    uint64_t total_pages = (uint64_t)stack_pages + (STACK_GUARD_PAGES * 2u);
    uint64_t base = pfa_alloc_pages_below(total_pages, KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES);
    uint8_t using_higher_half_stack = 1;
    if (!base) {
        /* TODO(ring3-phase2): If stack pressure exceeds the shared higher-half
         * window, extend the explicit kernel allowlist window instead of
         * falling back to low-mapped stacks under user CR3 roots. */
        klog_write("[sched] higher-half stack alloc failed\n");
        return -1;
    }

    uint64_t guard_low = base;
    uint64_t usable_base = base + ((uint64_t)STACK_GUARD_PAGES * PAGE_SIZE);
    uint64_t guard_high = base + ((total_pages - STACK_GUARD_PAGES) * PAGE_SIZE);
    uint64_t higher_half_base = paging_get_higher_half_base();
    uint64_t guard_low_virt = using_higher_half_stack ? (higher_half_base + guard_low) : guard_low;
    uint64_t usable_base_virt =
        using_higher_half_stack ? (higher_half_base + usable_base) : usable_base;
    uint64_t guard_high_virt =
        using_higher_half_stack ? (higher_half_base + guard_high) : guard_high;

    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_low_virt + ((uint64_t)i * PAGE_SIZE)) != 0) {
            klog_write("[sched] guard unmap failed\n");
            return -1;
        }
    }
    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_high_virt + ((uint64_t)i * PAGE_SIZE)) != 0) {
            klog_write("[sched] guard unmap failed\n");
            return -1;
        }
    }

    slot->stack_base = (uintptr_t)usable_base_virt;
    slot->stack_pages = stack_pages;
    slot->stack_top = (uintptr_t)guard_high_virt;
    slot->stack_alloc_base_phys = (uintptr_t)base;

    process_stack_poison(slot->stack_base, slot->stack_top);

    if (slot->stack_base && slot->stack_top > slot->stack_base + sizeof(uint64_t)) {
        /* Canaries catch in-range stack corruption that does not reach the guard
         * pages, such as smashed frames near the bottom or top of the stack. */
        uint64_t* base_canary = ptr_cast(uint64_t, slot->stack_base);
        uint64_t* top_canary = ptr_cast(uint64_t, (slot->stack_top - sizeof(uint64_t)));
        uintptr_t mid_addr = slot->stack_base + (slot->stack_top - slot->stack_base) / 2u;
        uint64_t* mid_canary = ptr_cast(uint64_t, (mid_addr & ~(uintptr_t)0x7u));
        *base_canary = STACK_CANARY_VALUE;
        *top_canary = STACK_CANARY_VALUE;
        *mid_canary = STACK_CANARY_VALUE;
    }
    return 0;
}

static int process_alloc_thread_stack(thread_t* thread, uint32_t stack_pages) {
    if (!thread || stack_pages == 0) {
        return -1;
    }
    uint64_t total_pages = (uint64_t)stack_pages + (STACK_GUARD_PAGES * 2u);
    uint64_t base = pfa_alloc_pages_below(total_pages, KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES);
    if (!base) {
        klog_write("[sched] worker stack alloc failed\n");
        return -1;
    }

    uint64_t guard_low = base;
    uint64_t usable_base = base + ((uint64_t)STACK_GUARD_PAGES * PAGE_SIZE);
    uint64_t guard_high = base + ((total_pages - STACK_GUARD_PAGES) * PAGE_SIZE);
    uint64_t higher_half_base = paging_get_higher_half_base();
    uint64_t guard_low_virt = higher_half_base + guard_low;
    uint64_t usable_base_virt = higher_half_base + usable_base;
    uint64_t guard_high_virt = higher_half_base + guard_high;

    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_low_virt + ((uint64_t)i * PAGE_SIZE)) != 0) {
            klog_write("[sched] worker guard unmap failed\n");
            return -1;
        }
    }
    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_high_virt + ((uint64_t)i * PAGE_SIZE)) != 0) {
            klog_write("[sched] worker guard unmap failed\n");
            return -1;
        }
    }

    thread->kstack_base = (uintptr_t)usable_base_virt;
    thread->kstack_top = (uintptr_t)guard_high_virt;
    thread->kstack_alloc_base_phys = (uintptr_t)base;
    thread->kstack_pages = stack_pages;

    process_stack_poison(thread->kstack_base, thread->kstack_top);
    return 0;
}

static void process_restore_stack_guard_mappings(uint64_t alloc_base_phys, uint32_t stack_pages) {
    if (!alloc_base_phys || stack_pages == 0) {
        return;
    }
    uint64_t total_pages = (uint64_t)stack_pages + (STACK_GUARD_PAGES * 2u);
    uint64_t guard_low = alloc_base_phys;
    uint64_t guard_high = alloc_base_phys + ((total_pages - STACK_GUARD_PAGES) * PAGE_SIZE);
    uint64_t higher_half_base = paging_get_higher_half_base();

    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        uint64_t phys = guard_low + ((uint64_t)i * PAGE_SIZE);
        uint64_t virt = higher_half_base + phys;
        (void)paging_map_4k(virt, phys, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    }
    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        uint64_t phys = guard_high + ((uint64_t)i * PAGE_SIZE);
        uint64_t virt = higher_half_base + phys;
        (void)paging_map_4k(virt, phys, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    }
}

static process_run_result_t process_run_worker_on_stack(process_t* proc, thread_t* thread) {
    process_thread_worker_entry_t entry =
        fn_cast(process_thread_worker_entry_t, process_kernel_alias_addr(thread->worker_entry));
    uintptr_t stack_top = thread->kstack_top - 16u;
    stack_top &= ~(uintptr_t)0xFULL;
    process_run_result_t rc = PROCESS_RUN_EXITED;
#ifdef WASMOS_PROCESS_TEST_SEAMS
    /* A host test has no per-thread kernel stack to switch to, and the switch is
     * not what any lifecycle test is asserting. Call on the current stack. */
    (void)stack_top;
    rc = entry(proc, thread->tid, thread->worker_arg);
#else
    __asm__ volatile("mov %%rsp, %%r15\n"
                     "mov %[stack_top], %%rsp\n"
                     "call *%[entry]\n"
                     "mov %%r15, %%rsp\n"
                     : "=a"(rc)
                     : [stack_top] "r"(stack_top),
                       [entry] "r"(entry),
                       "D"(proc),
                       "S"(thread->tid),
                       "d"(thread->worker_arg)
                     : "r15", "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
#endif
    return rc;
}

extern void context_switch(process_context_t* out, process_context_t* in);
extern void context_switch_to(process_context_t* in);
static void context_switch_high(process_context_t* out, process_context_t* in);
static int process_schedule_once_impl(void);
static thread_t* process_main_thread(process_t* proc);
static process_t* process_owner_for_thread(thread_t* thread);
static thread_t* process_thread_for_transition(process_t* proc);
static thread_t* process_first_owner_ready_thread(process_t* proc);
static process_context_t* process_sched_ctx_for_thread(process_t* proc, thread_t* thread);
static void process_wake_thread_joiner(process_t* owner, thread_t* exited);
static void process_reap(process_t* proc);
static void process_sched_invariant_fail(const char* msg, uint64_t a, uint64_t b);
static void process_set_blocked(process_t* proc, thread_t* thread, process_block_reason_t reason,
                                thread_block_reason_t thread_reason);
static int process_set_ready(process_t* proc, thread_t* thread);
static int process_set_running(process_t* proc, thread_t* thread);
static uint8_t process_has_waiters(uint32_t target_pid);
static void process_try_auto_reap(process_t* proc);
static process_run_result_t process_thread_spawn_default_worker(process_t* process, uint32_t tid,
                                                                void* arg);

static process_run_result_t process_thread_spawn_default_worker(process_t* process, uint32_t tid,
                                                                void* arg) {
    (void)process;
    (void)tid;
    (void)arg;
    return PROCESS_RUN_THREAD_EXITED;
}

static void context_switch_high(process_context_t* out, process_context_t* in) {
    uint64_t higher_half_base = paging_get_higher_half_base();
    uintptr_t fn = (uintptr_t)&context_switch;
    if ((uint64_t)fn < higher_half_base) {
        fn += (uintptr_t)higher_half_base;
    }
    ((void (*)(process_context_t*, process_context_t*))fn)(out, in);
}

static int process_run_on_sched_stack(int (*fn)(void)) {
    if (!fn) {
        return -1;
    }
    uint32_t cpu_id = cpu_local()->cpu_id;
    if (cpu_id >= WASMOS_MAX_CPUS) {
        cpu_id = 0;
    }
    uintptr_t stack_top = process_kernel_alias_addr(
        (uintptr_t)&g_sched_trampoline_stacks[cpu_id][SCHED_TRAMPOLINE_STACK_BYTES]);
    stack_top &= ~(uintptr_t)0xFULL;
    int rc = -1;
#ifdef WASMOS_PROCESS_TEST_SEAMS
    (void)stack_top;
    rc = fn();
#else
    __asm__ volatile("mov %%rsp, %%r15\n"
                     "mov %[stack_top], %%rsp\n"
                     "call *%[fn]\n"
                     "mov %%r15, %%rsp\n"
                     : "=a"(rc)
                     : [stack_top] "r"(stack_top), [fn] "r"(fn)
                     : "r15", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "memory", "cc");
#endif
    return rc;
}

static void process_log_ctx_watch(const char* where) {
    trace_write("[sched] ctxwatch ");
    if (where) {
        trace_write(where);
    }
    trace_write(" ctx=");
    trace_do(serial_write_hex64(g_ctx_watch_last_ctx));
    trace_write("[sched] ctxwatch hits=");
    trace_do(serial_write_hex64(g_ctx_watch_hits));
    trace_write("[sched] ctxwatch reason=");
    trace_do(serial_write_hex64(g_ctx_watch_reason));
    trace_write("[sched] ctxwatch rip=");
    trace_do(serial_write_hex64(g_ctx_watch_last_rip));
    trace_write("[sched] ctxwatch rsp=");
    trace_do(serial_write_hex64(g_ctx_watch_last_rsp));
    trace_write("[sched] ctxwatch rflags=");
    trace_do(serial_write_hex64(g_ctx_watch_last_rflags));
}

static void process_log_ctxsw_state(void) {
    trace_write("[sched] ctxsw out ctx=");
    trace_do(serial_write_hex64(g_ctxsw_last_out_ctx));
    trace_write("[sched] ctxsw out rip=");
    trace_do(serial_write_hex64(g_ctxsw_last_out_rip));
    trace_write("[sched] ctxsw out rsp=");
    trace_do(serial_write_hex64(g_ctxsw_last_out_rsp));
    trace_write("[sched] ctxsw out rflags=");
    trace_do(serial_write_hex64(g_ctxsw_last_out_rflags));
    trace_write("[sched] ctxsw in ctx=");
    trace_do(serial_write_hex64(g_ctxsw_last_in_ctx));
    trace_write("[sched] ctxsw in rip=");
    trace_do(serial_write_hex64(g_ctxsw_last_in_rip));
    trace_write("[sched] ctxsw in rsp=");
    trace_do(serial_write_hex64(g_ctxsw_last_in_rsp));
    trace_write("[sched] ctxsw in rflags=");
    trace_do(serial_write_hex64(g_ctxsw_last_in_rflags));
    trace_write("[sched] ctxsw restore ctx=");
    trace_do(serial_write_hex64(g_ctx_restore_ctx));
    trace_write("[sched] ctxsw restore rip=");
    trace_do(serial_write_hex64(g_ctx_restore_rip));
    trace_write("[sched] ctxsw restore rsp=");
    trace_do(serial_write_hex64(g_ctx_restore_rsp));
    trace_write("[sched] ctxsw restore rflags=");
    trace_do(serial_write_hex64(g_ctx_restore_rflags));
}

static void process_log_ctx_watch_if_changed(void) {
    if (g_ctx_watch_last_rip == g_ctx_watch_last_logged_rip &&
        g_ctx_watch_last_rsp == g_ctx_watch_last_logged_rsp &&
        g_ctx_watch_last_rflags == g_ctx_watch_last_logged_rflags &&
        g_ctx_watch_reason == g_ctx_watch_last_logged_reason) {
        return;
    }
    g_ctx_watch_last_logged_rip = g_ctx_watch_last_rip;
    g_ctx_watch_last_logged_rsp = g_ctx_watch_last_rsp;
    g_ctx_watch_last_logged_rflags = g_ctx_watch_last_rflags;
    g_ctx_watch_last_logged_reason = g_ctx_watch_reason;
    process_log_ctx_watch("ctxsw");
}

static void process_validate_thread_context(process_t* proc, thread_t* thread,
                                            process_context_t* ctx, const char* where) {
    if (!proc || !thread || !ctx) {
        return;
    }
    if (thread->ctx_canary_pre != PROCESS_CTX_CANARY_VALUE ||
        thread->ctx_canary_post != PROCESS_CTX_CANARY_VALUE) {
        klog_printf("[sched] thread ctx canary corrupt pid=%016llx tid=%016llx\n"
                    "[sched] name=%s\n"
                    "[sched] ctx canary pre=%016llx\n"
                    "[sched] ctx canary post=%016llx\n",
                    (unsigned long long)proc->pid,
                    (unsigned long long)thread->tid,
                    proc->name ? proc->name : "(null)",
                    (unsigned long long)thread->ctx_canary_pre,
                    (unsigned long long)thread->ctx_canary_post);
        process_log_ctxsw_state();
        process_log_ctx_watch("canary");
        kpanic("ctx_canary_tripped",
               (uintptr_t)thread->ctx_canary_pre,
               (uintptr_t)thread->ctx_canary_post);
    }
#ifdef WASMOS_PROCESS_TEST_SEAMS
    /* The canary check above still applies -- it is plain memory. The rip/rsp
     * range checks below do not: they bound a saved context against the kernel
     * image and the higher-half alias, and on the host there is no image range
     * to bound against and no saved context to bound. The seam in
     * process_run_worker_on_stack calls a worker entry directly rather than
     * switching to a context, so nothing a test drives ever populates one. */
    (void)where;
    return;
#else
    uint64_t rip = ctx->rip;
    uint8_t is_user_ctx = (uint8_t)((ctx->cs & 0x3u) == 0x3u);
    uint64_t start = addr_cast(uint64_t, &__kernel_start);
    uint64_t end = addr_cast(uint64_t, &__kernel_end);
    uint64_t low_start = start;
    uint64_t low_end = end;
    uint64_t higher_half = paging_get_higher_half_base();
    if (start < higher_half) {
        start += higher_half;
    }
    if (end < higher_half) {
        end += higher_half;
    }
    if (is_user_ctx || (rip >= start && rip < end) || (rip >= low_start && rip < low_end)) {
        if (!is_user_ctx) {
            uint64_t rsp = ctx->rsp;
            if (rsp < higher_half) {
                klog_printf("[sched] invalid thread rsp in %s pid=%016llx tid=%016llx\n"
                            "[sched] name=%s\n"
                            "[sched] rip=%016llx\n"
                            "[sched] rsp=%016llx\n",
                            where ? where : "?",
                            (unsigned long long)proc->pid,
                            (unsigned long long)thread->tid,
                            proc->name ? proc->name : "(null)",
                            (unsigned long long)rip,
                            (unsigned long long)rsp);
                process_log_ctxsw_state();
                process_log_ctx_watch("invalid-rsp");
                kpanic("invalid_rsp", (uintptr_t)rip, (uintptr_t)rsp);
            }
        }
        return;
    }
    klog_printf("[sched] invalid thread rip in %s pid=%016llx tid=%016llx\n"
                "[sched] name=%s\n"
                "[sched] rip=%016llx\n"
                "[sched] rsp=%016llx\n",
                where ? where : "?",
                (unsigned long long)proc->pid,
                (unsigned long long)thread->tid,
                proc->name ? proc->name : "(null)",
                (unsigned long long)rip,
                (unsigned long long)ctx->rsp);
    process_log_ctxsw_state();
    process_log_ctx_watch("invalid-rip");
    kpanic("invalid_rip", (uintptr_t)rip, (uintptr_t)ctx->rsp);
#endif
}

static thread_t* process_main_thread(process_t* proc) {
    if (!proc || proc->main_tid == 0) {
        return 0;
    }
    return thread_get(proc->main_tid);
}

static process_t* process_owner_for_thread(thread_t* thread) {
    if (!thread || thread->owner_pid == 0) {
        return 0;
    }
    return process_find_by_pid(thread->owner_pid);
}

static thread_t* process_thread_for_transition(process_t* proc) {
    if (!proc) {
        return 0;
    }
    if (cpu_local()->current_process == proc && cpu_local()->current_thread) {
        return cpu_local()->current_thread;
    }
    return process_main_thread(proc);
}

static thread_t* process_first_owner_ready_thread(process_t* proc) {
    if (!proc) {
        return 0;
    }
    for (uint32_t i = 0;; ++i) {
        uint32_t tid = 0;
        thread_t* thread = 0;
        if (thread_owner_tid_at(proc->pid, i, &tid) != 0) {
            break;
        }
        thread = thread_get(tid);
        if (!thread || thread->state != THREAD_STATE_READY) {
            continue;
        }
        return thread;
    }
    return 0;
}

static process_context_t* process_sched_ctx_for_thread(process_t* proc, thread_t* thread) {
    if (!proc || !thread) {
        return 0;
    }
    /* Every thread (including kernel workers) saves to its own ctx. */
    return &thread->ctx;
}

static void process_trampoline(void) {
    for (;;) {
        cpu_local()->in_scheduler = 0;
        if (cpu_local()->current_process) {
            uint64_t* base = ptr_cast(uint64_t, cpu_local()->current_process->stack_base);
            uint64_t* top =
                ptr_cast(uint64_t, (cpu_local()->current_process->stack_top - sizeof(uint64_t)));
            uintptr_t mid_addr = cpu_local()->current_process->stack_base +
                                 (cpu_local()->current_process->stack_top -
                                  cpu_local()->current_process->stack_base) /
                                     2u;
            uint64_t* mid = ptr_cast(uint64_t, (mid_addr & ~(uintptr_t)0x7u));
            if (base && top && mid) {
                const uint64_t canary = STACK_CANARY_VALUE;
                if (*base != canary || *top != canary || *mid != canary) {
                    serial_printf_unlocked("[sched] stack canary tripped for %s\n"
                                           "[sched] base=%016llx\n"
                                           "[sched] mid=%016llx\n"
                                           "[sched] top=%016llx\n"
                                           "[sched] base val=%016llx\n"
                                           "[sched] mid val=%016llx\n"
                                           "[sched] top val=%016llx\n",
                                           cpu_local()->current_process->name
                                               ? cpu_local()->current_process->name
                                               : "(unknown)",
                                           addr_cast(unsigned long long, base),
                                           addr_cast(unsigned long long, mid),
                                           addr_cast(unsigned long long, top),
                                           (unsigned long long)*base,
                                           (unsigned long long)*mid,
                                           (unsigned long long)*top);
                    kpanic("stack_canary_tripped", (uintptr_t)base, (uintptr_t)top);
                }
            }
        }
        while (preempt_disable_depth() > 0) {
            preempt_enable();
        }
        /* The top-level scheduler loop enters with IF clear. Kernel processes
         * run through this trampoline and may voluntarily yield/re-enter it;
         * restore normal interrupt delivery before calling their entry point so
         * timer ticks keep advancing while ring-0 process code runs. */
#ifndef WASMOS_PROCESS_TEST_SEAMS
        __asm__ volatile("sti" ::: "memory");
#endif
        if (!cpu_local()->current_process || !cpu_local()->current_process->entry) {
            cpu_local()->last_run_result = PROCESS_RUN_IDLE;
        } else {
            uintptr_t entry_ptr =
                process_kernel_alias_addr((uintptr_t)cpu_local()->current_process->entry);
            process_entry_t entry_fn = (process_entry_t)(void*)entry_ptr;
            /* Guard runtime reentrancy for subsystems that require a
             * single-threaded process entry path. Kernel workers
             * (is_kernel_worker) skip this path entirely. */
            if (cpu_local()->current_process->needs_runtime_lock && cpu_local()->current_thread &&
                !cpu_local()->current_thread->is_kernel_worker) {
                /* Use no-IRQ variant: runtime_lock is held for the entire runtime-locked timeslice.
                 * ksync_spinlock_lock would cli for that whole duration, suppressing keyboard
                 * and mouse IRQ delivery.  No interrupt handler acquires runtime_lock, so
                 * the full irq-disable contract is not needed here. */
                ksync_spinlock_lock_noirq(&cpu_local()->current_process->runtime_lock);
                cpu_local()->current_process->runtime_lock_owner = cpu_local()->current_thread->tid;
                cpu_local()->last_run_result =
                    entry_fn(cpu_local()->current_process, cpu_local()->current_process->arg);
                cpu_local()->current_process->runtime_lock_owner = 0;
                ksync_spinlock_unlock_noirq(&cpu_local()->current_process->runtime_lock);
            } else {
                cpu_local()->last_run_result =
                    entry_fn(cpu_local()->current_process, cpu_local()->current_process->arg);
            }
        }
        cpu_local()->in_scheduler = 1;
        critical_section_enter();
        process_context_t* ctx =
            process_sched_ctx_for_thread(cpu_local()->current_process, cpu_local()->current_thread);
        if (!ctx) {
            cpu_local()->last_run_result = PROCESS_RUN_IDLE;
            continue;
        }
        context_switch_high(ctx, &cpu_local()->sched_ctx);
        if (g_ctx_watch_hits != g_ctx_watch_logged) {
            g_ctx_watch_logged = g_ctx_watch_hits;
            process_log_ctx_watch_if_changed();
        }
    }
}

extern void process_preempt_trampoline(void);

/* Process-slot state machine (mirror of thread_transit).  Every ->state write
 * funnels through process_transit so the whole lifecycle lives in one place:
 *
 *   UNUSED ─┐
 *           ├▶ NEW ▶ LIVE(READY⇄RUNNING⇄BLOCKED) ▶ ZOMBIE ▶ REAPING ▶ DEAD ─┐
 *   DEAD  ──┘                                                                 │
 *      ▲──────────────────────── DEAD▶NEW (slot reuse) ───────────────────────┘
 *
 * Invariants enforced (everything else among LIVE states is permitted so a
 * legitimate scheduler move is never rejected):
 *   1. ZOMBIE is monotonic — only ▶ REAPING (the reap claim); nothing
 *      resurrects a zombie, so "process zombie" is a stable predicate.
 *   2. REAPING ▶ DEAD only (reap completion publishes the reusable slot).
 *   3. A free slot (UNUSED/DEAD) may only enter NEW — never jump straight to a
 *      schedulable state (the free-slot-activation hole).
 *   4. NEW publishes to a LIVE state (or straight to ZOMBIE on spawn failure).
 * UNUSED is produced ONLY by pristine table init (direct write), never as a
 * transit target. */
static int process_transition_legal(process_state_t from, process_state_t to) {
    if (from == to) {
        return 1; /* idempotent no-op is always allowed */
    }
    if (from == PROCESS_STATE_ZOMBIE) {
        return to == PROCESS_STATE_REAPING;
    }
    if (from == PROCESS_STATE_REAPING) {
        return to == PROCESS_STATE_DEAD;
    }
    if (from == PROCESS_STATE_UNUSED || from == PROCESS_STATE_DEAD) {
        return to == PROCESS_STATE_NEW;
    }
    if (to == PROCESS_STATE_UNUSED || to == PROCESS_STATE_NEW || to == PROCESS_STATE_REAPING ||
        to == PROCESS_STATE_DEAD) {
        return 0; /* only the guarded edges above reach these */
    }
    if (from == PROCESS_STATE_NEW) {
        /* NEW means "slot claimed, not yet published".  The publish itself is
         * NEW->READY (a normal spawn) or NEW->BLOCKED (a parked one), and a spawn
         * that fails part-way goes NEW->ZOMBIE.  RUNNING is deliberately NOT in
         * that set: it would mean some CPU dispatched a process whose spawner has
         * not finished building it, and it is what turned a stale-thread-pointer
         * dispatch into a corrupted publish -- process_set_running would transit
         * NEW->RUNNING, and the spawner's own NEW->READY CAS then failed with
         * "spawn publish NEW->LIVE failed".  Refusing the edge means the publish
         * cannot lose that race no matter what reaches process_set_running. */
        return to == PROCESS_STATE_READY || to == PROCESS_STATE_BLOCKED ||
               to == PROCESS_STATE_ZOMBIE;
    }
    /* from is READY/RUNNING/BLOCKED; to is READY/RUNNING/BLOCKED/ZOMBIE. */
    return 1;
}

static int process_transit(process_t* proc, process_state_t from, process_state_t to) {
    if (!proc) {
        return 0;
    }
    if (!process_transition_legal(from, to)) {
        return 0;
    }
    uint32_t expected = (uint32_t)from;
    return __atomic_compare_exchange_n((uint32_t*)&proc->state,
                                       &expected,
                                       (uint32_t)to,
                                       0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE)
               ? 1
               : 0;
}

/* Retrying transit for writers that know the target but not a stable `from`.
 * Re-reads the current state and CASes to `to`, retrying if it loses a race to
 * concurrent LIVE churn (e.g. another thread of the same process moving
 * READY⇄RUNNING on another CPU).  Returns 1 if proc is now in `to`; 0 if a
 * monotonic barrier forbids it (current state is a terminal/beyond `to`, most
 * importantly ZOMBIE/REAPING/DEAD when `to` is a LIVE state).  Terminates: each
 * losing retry re-reads a state that only ever advances toward the ZOMBIE sink,
 * so the legality check eventually returns 0 or the CAS wins. */
static int process_force_transit(process_t* proc, process_state_t to) {
    if (!proc) {
        return 0;
    }
    for (;;) {
        process_state_t from =
            (process_state_t)__atomic_load_n((uint32_t*)&proc->state, __ATOMIC_ACQUIRE);
        if (from == to) {
            return 1;
        }
        if (!process_transition_legal(from, to)) {
            return 0;
        }
        if (process_transit(proc, from, to)) {
            return 1;
        }
    }
}

static void process_reset_slot(process_t* proc) {
    if (!proc) {
        return;
    }
    proc->pid = 0;
    proc->parent_pid = 0;
    proc->context_id = 0;
    proc->main_tid = 0;
    __atomic_store_n(&proc->thread_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&proc->live_thread_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&proc->exiting, 0u, __ATOMIC_RELEASE);
    /* ->state is NOT written here: this zeroes the bookkeeping only.  The
     * pristine table init publishes UNUSED directly; the reaper publishes the
     * terminal state via process_transit(REAPING -> DEAD).  Zeroing state here
     * would transiently expose a REAPING slot as free (UNUSED) and let another
     * CPU claim it mid-reap. */
    proc->block_reason = PROCESS_BLOCK_NONE;
    proc->wait_target_pid = 0;
    proc->exit_status = 0;
    proc->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    proc->ticks_remaining = 0;
    proc->ticks_total = 0;
    proc->is_idle = 0;
    proc->in_hostcall = 0;
    proc->auto_reap = 0;
    proc->reap_requested = 0;
    proc->needs_runtime_lock = 0;
    proc->ready = 0;
    process_clear_runtime_tag(proc);
    proc->ctx = (process_context_t){0};
    proc->ctx_canary_pre = 0;
    proc->ctx_canary_post = 0;
    proc->stack_base = 0;
    proc->stack_top = 0;
    proc->stack_alloc_base_phys = 0;
    proc->stack_pages = 0;
    proc->entry = 0;
    proc->arg = 0;
    for (uint32_t i = 0; i < PROCESS_NAME_MAX; ++i) {
        proc->name_storage[i] = '\0';
    }
    proc->name = 0;
}

static int process_copy_name(process_t* proc, const char* name) {
    if (!proc || !name) {
        return -1;
    }
    uint32_t i = 0;
    for (; name[i] && i + 1 < PROCESS_NAME_MAX; ++i) {
        proc->name_storage[i] = name[i];
    }
    proc->name_storage[i] = '\0';
    proc->name = proc->name_storage;
    return name[i] == '\0' ? 0 : -1;
}

static void process_clear_runtime_tag(process_t* proc) {
    if (!proc) {
        return;
    }
    for (uint32_t i = 0; i < sizeof(proc->runtime_tag); ++i) {
        proc->runtime_tag[i] = '\0';
    }
}

static int process_copy_runtime_tag(process_t* proc, const char* tag) {
    if (!proc || !tag) {
        return -1;
    }
    process_clear_runtime_tag(proc);
    for (uint32_t i = 0; i < WASMOS_APP_SUBSYSTEM_TAG_LEN; ++i) {
        if (tag[i] == '\0') {
            return 0;
        }
        proc->runtime_tag[i] = tag[i];
    }
    return tag[WASMOS_APP_SUBSYSTEM_TAG_LEN] == '\0' ? 0 : -1;
}

/* Claiming a slot is a test-then-write race, so this MUST run under
 * g_process_table_lock and the caller must transition the returned slot out of
 * UNUSED/DEAD before releasing it.  (process_spawn_idle is the one exception:
 * it runs single-threaded at boot, before any AP is up.) */
static process_t* process_find_slot(void) {
    process_t* table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        /* Both free states are claimable: UNUSED (pristine) and DEAD (reaped). */
        if (table[i].state == PROCESS_STATE_UNUSED || table[i].state == PROCESS_STATE_DEAD) {
            return &table[i];
        }
    }
    return 0;
}

/* Lock-free by design and called that way by most of its users, including the
 * dispatch hot path (process_get).  A slot is never handed back to the allocator
 * while it is live -- the reaper publishes DEAD only after process_reap has torn
 * everything down -- and g_next_pid only ever counts up, so a match is the
 * process the caller named.  The returned pointer stays valid only as long as
 * the caller has a reason to believe that process is alive: once it is reaped,
 * the slot can be reclaimed by an unrelated spawn. */
static process_t* process_find_by_pid(uint32_t pid) {
    if (pid == 0) {
        return 0;
    }
    process_t* table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (table[i].pid == pid && table[i].state != PROCESS_STATE_UNUSED) {
            return &table[i];
        }
    }
    return 0;
}

/* Lock-free, on the same terms as process_find_by_pid. */
static process_t* process_find_by_context_internal(uint32_t context_id) {
    if (context_id == 0) {
        return 0;
    }
    process_t* table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (table[i].context_id == context_id && table[i].state != PROCESS_STATE_UNUSED) {
            return &table[i];
        }
    }
    return 0;
}

static void process_wake_waiters(uint32_t target_pid) {
    if (target_pid == 0) {
        return;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_t* proc = &g_processes[i];
        uint8_t woke_any = 0;
        if (proc->state == PROCESS_STATE_UNUSED || proc->state == PROCESS_STATE_DEAD ||
            proc->state == PROCESS_STATE_ZOMBIE || proc->state == PROCESS_STATE_REAPING) {
            continue;
        }
        for (uint32_t j = 0;; ++j) {
            uint32_t tid = 0;
            thread_t* waiter = 0;
            if (thread_owner_tid_at(proc->pid, j, &tid) != 0) {
                break;
            }
            waiter = thread_get(tid);
            if (!waiter || waiter->state != THREAD_STATE_BLOCKED) {
                continue;
            }
            if (waiter->block_reason != THREAD_BLOCK_WAIT_PROCESS) {
                continue;
            }
            if (waiter->wait_target_pid != target_pid) {
                continue;
            }
            waiter->wait_target_pid = 0;
            woke_any = 1;
            int runnable = 1;
            /* The fast path marks the waiter READY directly, without a process
             * transition, because the owner is already the RUNNING process on this
             * CPU.  `exiting` still has to be tested: process.h documents it as
             * "1 slightly ahead of ->state", so RUNNING and exiting are
             * simultaneously true for the whole head of a teardown, and taking
             * this branch there would enqueue a waiter under an owner on its way
             * out.  Falling through to process_set_ready is what refuses it, and
             * counts the refusal like every other one. */
            if (proc == cpu_local()->current_process && proc->state == PROCESS_STATE_RUNNING &&
                !__atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE) && cpu_local()->current_thread &&
                cpu_local()->current_thread->tid != waiter->tid) {
                /* The owner was just verified as this CPU's RUNNING, non-exiting
                 * process, so the wake is permitted and `runnable` stays 1. Only
                 * the DEMOTION is avoided: the waiter was BLOCKED when tested a
                 * few lines up and may have been woken since, and writing READY
                 * over RUNNING would lose that dispatch. The handshake below runs
                 * either way -- it is what covers a target that is executing. */
                (void)thread_wake_if_blocked(waiter->tid);
            } else {
                runnable = process_set_ready(proc, waiter);
            }
            /* Enqueue only on winning the wake/block handshake; otherwise the
             * owning CPU's PROCESS_RUN_BLOCKED handler does it.  A waiter whose
             * own owner is going away is not woken at all, so it must not take
             * the claim either -- leaving no token is what keeps the completion
             * path from enqueuing on its behalf. */
            if (runnable && sched_wake_claim_enqueue(waiter)) {
                sched_enqueue_thread(waiter);
            }
        }
        if (woke_any && process_transit(proc, PROCESS_STATE_BLOCKED, PROCESS_STATE_READY)) {
            proc->block_reason = PROCESS_BLOCK_NONE;
        }
    }
}

static void process_mark_exited(process_t* proc, int32_t exit_status) {
    if (!proc || proc->state == PROCESS_STATE_UNUSED || proc->state == PROCESS_STATE_DEAD) {
        return;
    }
    proc->exit_status = exit_status;
    __atomic_store_n(&proc->exiting, 1u, __ATOMIC_RELEASE);
    proc->block_reason = PROCESS_BLOCK_NONE;
    proc->wait_target_pid = 0;
    /* Force LIVE/NEW -> ZOMBIE, retrying against concurrent LIVE churn so a
     * racing set_ready/set_running on another CPU cannot make us drop the
     * transition and leave the process alive.  A 0 return means the slot is
     * ALREADY ZOMBIE/REAPING/DEAD (a concurrent kill won) — all terminal — so
     * the postcondition "proc is dead" holds either way. */
    (void)process_force_transit(proc, PROCESS_STATE_ZOMBIE);
    thread_mark_owner_exited(proc->pid, exit_status);
    __atomic_store_n(&proc->live_thread_count, 0u, __ATOMIC_RELAXED);
    process_wake_waiters(proc->pid);
}

static uint8_t process_has_waiters(uint32_t target_pid) {
    if (target_pid == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_t* proc = &g_processes[i];
        if (proc->state == PROCESS_STATE_UNUSED || proc->state == PROCESS_STATE_DEAD ||
            proc->state == PROCESS_STATE_ZOMBIE || proc->state == PROCESS_STATE_REAPING) {
            continue;
        }
        for (uint32_t j = 0;; ++j) {
            uint32_t tid = 0;
            thread_t* waiter = 0;
            if (thread_owner_tid_at(proc->pid, j, &tid) != 0) {
                break;
            }
            waiter = thread_get(tid);
            if (!waiter || waiter->state != THREAD_STATE_BLOCKED) {
                continue;
            }
            if (waiter->block_reason != THREAD_BLOCK_WAIT_PROCESS) {
                continue;
            }
            if (waiter->wait_target_pid == target_pid) {
                return 1;
            }
        }
    }
    return 0;
}

/* Atomically claim the reap: only the CPU that transitions ZOMBIE → REAPING
 * proceeds and actually reaps.  This prevents two CPUs from simultaneously
 * freeing the same process's stacks/memory when work-stealing causes concurrent
 * exits/waits to arrive on different CPUs.  All reap paths (auto-reap, wait,
 * PM wait-reply) funnel through here so the CAS is the single gate. */
/* Saturating atomic decrement of a per-process thread counter.
 *
 * These counters are mutated from several CPUs at once -- a spawn increments on
 * one while a retirement decrements on another -- and live_thread_count reaching
 * zero is what gates process_mark_exited, so a lost update marks a process exited
 * early or never. The `if (c > 0) c--;` these sites used was both non-atomic and
 * a check-then-act; a CAS loop is what expresses "decrement unless already zero"
 * without either flaw. */
static void process_count_dec(uint32_t* counter) {
    uint32_t cur = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
    while (cur != 0u) {
        if (__atomic_compare_exchange_n(
                counter, &cur, cur - 1u, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

static void process_reap_claim(process_t* proc) {
    if (!proc) {
        return;
    }
    /* Never free a process slot out from under a CPU that is dispatching one of
     * its threads.  process_schedule_once_impl holds `proc` as a raw pointer
     * across the dispatch AND across the result handling that follows, so a slot
     * freed and re-allocated in that window makes the handler act on whatever
     * process now owns the slot: observed as process_mark_exited landing on a
     * freshly spawned, still-NEW process, whose own publish then failed with
     * "spawn publish NEW->LIVE failed (state=ZOMBIE)".
     *
     * Refusing defers the reap rather than dropping it: the dispatch ends in
     * process_schedule_once_impl, which retries via process_try_auto_reap, and
     * the wait/PM paths retry independently. */
    if (thread_owner_has_active_dispatch(proc->pid)) {
        /* Hand the retry to the dispatch that caused this: the requester may be a
         * one-shot (process_reap_zombie_pid from the PM) and never ask again. */
        __atomic_store_n(&proc->reap_requested, 1u, __ATOMIC_RELEASE);
        return;
    }
    /* The single ZOMBIE -> REAPING claim: only the CPU whose CAS wins proceeds
     * to free the slot; losers no-op.  process_transit enforces that ZOMBIE can
     * ONLY advance to REAPING, so a concurrent state change cannot slip a
     * non-zombie into the reaper. */
    if (!process_transit(proc, PROCESS_STATE_ZOMBIE, PROCESS_STATE_REAPING)) {
        return;
    }
    process_reap(proc);
}

static void process_try_auto_reap(process_t* proc) {
    if (!proc || !proc->auto_reap) {
        return;
    }
    if (process_has_waiters(proc->pid)) {
        return;
    }
    process_reap_claim(proc);
}

/* Reap a specific zombie by pid, CAS-guarded.  Used by the process-manager
 * WAIT path to free a child's slot AFTER its exit status has been delivered to
 * the waiter — interactive CLI children are parented to the CLI, not the PM, so
 * the PM cannot use process_wait() (which enforces the parent check).  No-op if
 * the pid is not a (still-unreaped) zombie. */
void process_reap_zombie_pid(uint32_t pid) {
    if (pid == 0) {
        return;
    }
    process_t* proc = process_find_by_pid(pid);
    if (proc) {
        process_reap_claim(proc);
    }
}

static void process_reap(process_t* proc) {
    if (!proc) {
        return;
    }
    for (uint32_t i = 0;; ++i) {
        uint32_t tid = 0;
        if (thread_owner_tid_at(proc->pid, i, &tid) != 0) {
            break;
        }
        thread_t* thread = thread_get(tid);
        if (!thread) {
            continue;
        }
        if (thread->kstack_alloc_base_phys && thread->kstack_pages) {
            uint64_t total_pages = (uint64_t)thread->kstack_pages + (STACK_GUARD_PAGES * 2u);
            process_restore_stack_guard_mappings((uint64_t)thread->kstack_alloc_base_phys,
                                                 thread->kstack_pages);
            pfa_free_pages((uint64_t)thread->kstack_alloc_base_phys, total_pages);
            thread->kstack_alloc_base_phys = 0;
            thread->kstack_pages = 0;
            thread->kstack_base = 0;
            thread->kstack_top = 0;
        }
    }
    if (proc->stack_alloc_base_phys && proc->stack_pages) {
        uint64_t total_pages = (uint64_t)proc->stack_pages + (STACK_GUARD_PAGES * 2u);
        process_restore_stack_guard_mappings((uint64_t)proc->stack_alloc_base_phys,
                                             proc->stack_pages);
        pfa_free_pages((uint64_t)proc->stack_alloc_base_phys, total_pages);
    }
    if (proc->context_id != 0) {
        wasmos_subsystem_registry_drop_owner(proc->context_id);
        irq_release_context(proc->context_id);
        msi_release_context(proc->context_id);
        ipc_endpoints_release_owner(proc->context_id);
        xfer_buffer_drop_context(proc->context_id);
        (void)mm_context_destroy(proc->context_id);
    }
    if (proc->pid != 0) {
        warp_release_pid(proc->pid);
        wasm3_release_pid(proc->pid);
        wasm3_heap_release(proc->pid);
        native_driver_heap_release(proc->pid);
    }
    uint32_t reap_left = thread_reap_owner(proc->pid);
    if (reap_left != 0) {
        /* Every slot should be releasable by now: process_reap_claim refuses while
         * a thread of this process is dispatching, and thread_reap_owner retries
         * the short window where a CPU claims a thread of an already-ZOMBIE owner
         * and then loses at process_set_running. A leftover means a claim outlived
         * that, which is a defect worth naming rather than a slot silently left
         * behind. */
        uint32_t ln = sched_debug_note(SCHED_DEBUG_OWNER_REAP_LEFTOVER);
        if ((ln & (ln - 1u)) == 0u) {
            serial_printf_unlocked("[sched] owner reap left %u slot(s) pid=%u (n=%u)\n",
                                   (unsigned)reap_left,
                                   (unsigned)proc->pid,
                                   (unsigned)(ln + 1u));
        }
    }
    /* All owner threads are now reaped (off every queue, slots freed) and all
     * resources released.  Zero the bookkeeping, then publish the terminal
     * state as the single atomic step that makes the slot reclaimable.  State
     * stays REAPING throughout the free above, so no CPU can claim it early. */
    process_reset_slot(proc);
    /* Must succeed: this CPU owns the slot exclusively (it won the ZOMBIE ->
     * REAPING claim), so nothing else can have moved it.  A failure means the
     * slot would leak in REAPING forever — surface it. */
    if (!process_transit(proc, PROCESS_STATE_REAPING, PROCESS_STATE_DEAD)) {
        process_sched_invariant_fail(
            "reap publish REAPING->DEAD failed", proc->pid, (uint64_t)proc->state);
    }
}

/* The CALLING CPU's scheduler context — the one every dispatched thread switches
 * back into.  The storage lives in that CPU's cpu_local_t and outlives every
 * process, so the pointer never dangles, but it names a different object on each
 * CPU and must not be cached across a migration. */
process_context_t* cpu_local_sched_ctx(void) {
    return &cpu_local()->sched_ctx;
}

/* BSP-only bring-up: resets the pid counter, scrubs the process table to
 * pristine UNUSED, initialises this CPU's scheduler state and diagnostics, and
 * brings the thread table, run queue and futex table up.  Must run before any
 * spawn and before interrupts can preempt, since it publishes the table slots by
 * DIRECT write (the only producer of UNUSED — process_transit has no edge to
 * it).  An AP runs process_ap_init instead, which touches only its own per-CPU
 * state and must not repeat the global work here. */
void process_init(void) {
    g_next_pid = 1;
    ksync_spinlock_init(&g_process_table_lock);
    cpu_local()->last_index = 0;
    cpu_local()->current_pid = 0;
    cpu_local()->need_resched = 0;
    cpu_local()->current_process = 0;
    cpu_local()->current_thread = 0;
    cpu_local()->last_run_result = PROCESS_RUN_IDLE;
    cpu_local()->preempt_disable_count = 0;
    g_idle_process = 0;
    cpu_local()->in_scheduler = 1;
    g_ctx_watch_ctx = 0;
    g_ctx_watch_last_ctx = 0;
    g_ctx_watch_last_rip = 0;
    g_ctx_watch_last_rsp = 0;
    g_ctx_watch_last_rflags = 0;
    g_ctx_watch_hits = 0;
    g_ctx_watch_reason = 0;
    g_ctx_watch_logged = 0;
    g_ctx_watch_last_logged_rip = 0;
    g_ctx_watch_last_logged_rsp = 0;
    g_ctx_watch_last_logged_rflags = 0;
    g_ctx_watch_last_logged_reason = 0;
    g_preempt_smoke_logged = 0;
    __atomic_store_n(&g_sched_progress_logged, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sched_switch_count, 0u, __ATOMIC_RELAXED);
    cpu_local()->resched_pending_since_tick = 0;
    cpu_local()->resched_stall_reports = 0;
    g_trap_frame_invalid_reports = 0;
    g_ctx_restore_ctx = 0;
    g_ctx_restore_rip = 0;
    g_ctx_restore_rsp = 0;
    g_ctx_restore_rflags = 0;
    g_pm_stack_watch = 0;
    cpu_local()->pm_preempt_safe_depth = 0;
    thread_init();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_reset_slot(&g_processes[i]);
        g_processes[i].state = PROCESS_STATE_UNUSED; /* pristine free slot */
    }
    cpu_local()->sched_ctx.root_table = paging_get_root_table();
    cpu_sched_init(cpu_sched());
    futex_init();
}

void process_ap_init(void) {
    /* Initialize per-CPU scheduler state for an AP before its timer fires.
     * Must be called before lapic_ap_enable() so no timer preemption can
     * occur against an uninitialized sched_ctx. */
    cpu_local()->last_index = 0;
    cpu_local()->current_pid = 0;
    cpu_local()->need_resched = 0;
    cpu_local()->current_process = 0;
    cpu_local()->current_thread = 0;
    cpu_local()->last_run_result = PROCESS_RUN_IDLE;
    cpu_local()->preempt_disable_count = 0;
    cpu_local()->pm_preempt_safe_depth = 0;
    cpu_local()->resched_pending_since_tick = 0;
    cpu_local()->resched_stall_reports = 0;
    cpu_local()->in_scheduler = 1; /* block premature preemption */
    cpu_local()->sched_ctx.root_table = paging_get_root_table();
    /* Do NOT clear global ctx watch state here; the BSP may already have armed
     * it for a target thread before AP bring-up completes.
     * Each AP initialises its own cpu_sched_t (cpu_local()->sched); this is
     * independent of the BSP's queue and does not affect already-enqueued
     * threads. */
    cpu_sched_init(cpu_sched());
}

/* Spawns with the CALLING CPU's current process as the parent.  At boot, with
 * nothing dispatched, current_pid is 0, so the child is parented to 0 — which
 * process_kill treats as "no parent check applies" rather than as an error. */
int process_spawn(const char* name, process_entry_t entry, void* arg, uint32_t* out_pid) {
    return process_spawn_as(cpu_local()->current_pid, name, entry, arg, out_pid);
}

/* Shared spawn body.  Claims a free slot as NEW under g_process_table_lock,
 * builds the address space, main thread, kernel stack and initial ring-0
 * context, then publishes NEW -> READY (and enqueues) or NEW -> BLOCKED
 * (parked, awaiting process_unpark_pid), per `enqueue_initial`.  Returns 0 with
 * *out_pid set, or -1.
 *
 * The slot is claimed under the table lock but built without it: NEW is not a
 * schedulable state and no path other than this one moves a NEW slot, so the
 * lock is only needed to make find-then-claim atomic against a concurrent spawn.
 *
 * FIXME(spawn-slot-leak): every failure AFTER the ->NEW claim returns without
 * putting the slot back. process_find_slot only reclaims UNUSED/DEAD and
 * process_transition_legal has no NEW -> DEAD edge, so a failed spawn strands
 * that g_processes[] entry permanently, along with its mm context on the paths
 * that create one first. */
static int process_spawn_as_internal(uint32_t parent_pid, const char* name, process_entry_t entry,
                                     void* arg, uint32_t* out_pid,
                                     thread_state_t initial_thread_state,
                                     thread_block_reason_t initial_thread_reason,
                                     uint8_t enqueue_initial) {
    if (!entry || !out_pid) {
        return -1;
    }

    ksync_spinlock_lock(&g_process_table_lock);
    process_t* slot = process_find_slot();
    if (!slot) {
        ksync_spinlock_unlock(&g_process_table_lock);
        return -1;
    }
    /* Claim the free slot immediately (UNUSED/DEAD -> NEW) so no other CPU
     * grabs it.  Stays NEW — not schedulable — until fully initialised, then
     * publishes to a LIVE state at the end.  Must succeed: find_slot returned a
     * free slot under this same lock, so nothing else can have raced it. */
    if (!process_transit(slot, slot->state, PROCESS_STATE_NEW)) {
        ksync_spinlock_unlock(&g_process_table_lock);
        process_sched_invariant_fail("spawn claim ->NEW failed", (uint64_t)slot->state, 0);
    }

    uint32_t pid = g_next_pid++;
    ksync_spinlock_unlock(&g_process_table_lock);
    mm_context_t* ctx = mm_context_create(pid);
    if (!ctx) {
        return -1;
    }
    if (paging_clone_low_slot_in_root(ctx->root_table) != 0) {
        mm_context_destroy(ctx->id);
        return -1;
    }

    slot->pid = pid;
    slot->parent_pid = parent_pid;
    slot->context_id = ctx->id;
    slot->main_tid = 0;
    __atomic_store_n(&slot->thread_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->live_thread_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->exiting, 0u, __ATOMIC_RELEASE);
    /* state stays NEW here; published to LIVE below once init completes. */
    slot->block_reason = PROCESS_BLOCK_NONE;
    slot->wait_target_pid = 0;
    slot->exit_status = 0;
    slot->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    slot->ticks_remaining = slot->time_slice_ticks;
    slot->ticks_total = 0;
    slot->ctx_canary_pre = PROCESS_CTX_CANARY_VALUE;
    slot->ctx_canary_post = PROCESS_CTX_CANARY_VALUE;
    slot->needs_runtime_lock = 0;
    process_clear_runtime_tag(slot);
    (void)process_copy_runtime_tag(slot, "KERNEL");
    slot->entry = entry;
    slot->arg = arg;
    if (process_copy_name(slot, name ? name : "") != 0) {
        return -1;
    }
    if (thread_spawn_in_owner(
            pid, name ? name : "", initial_thread_state, initial_thread_reason, &slot->main_tid) !=
        0) {
        return -1;
    }

    thread_t* main_thread = process_main_thread(slot);
    if (!main_thread) {
        return -1;
    }
    main_thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    main_thread->ticks_remaining = main_thread->time_slice_ticks;
    main_thread->ticks_total = 0;

    __atomic_store_n(&slot->thread_count, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->live_thread_count, 1u, __ATOMIC_RELAXED);
    uint32_t stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_stack(slot, stack_pages) != 0) {
        return -1;
    }
    slot->ctx.rsp = slot->stack_top - (STACK_REDZONE_BYTES + 8u);
    slot->ctx.user_rsp = slot->ctx.rsp;
    slot->ctx.rip = (uint64_t)process_kernel_alias_addr((uintptr_t)process_trampoline);
    slot->ctx.rflags = 0x200;
    slot->ctx.cs = KERNEL_CS_SELECTOR;
    slot->ctx.ss = KERNEL_DS_SELECTOR;
    slot->ctx.root_table = ctx->root_table;

    main_thread = process_main_thread(slot);
    if (main_thread) {
        main_thread->ctx = slot->ctx;
        sched_thread_init(main_thread, sched_default_prio(slot->is_idle, 0, 0, 0));
    }

    ksync_spinlock_init(&slot->runtime_lock);
    slot->runtime_lock_owner = 0;
    sched_event_init(&slot->wait_event, SCHED_EVENT_TYPE_PROCESS);
    if (name && (strcmp(name, "process-manager") == 0 || strcmp(name, "native-call-min") == 0)) {
        thread_t* watch_thread = process_main_thread(slot);
        if (watch_thread) {
            g_ctx_watch_ctx = addr_cast(uint64_t, &watch_thread->ctx);
            g_ctx_watch_last_ctx = g_ctx_watch_ctx;
            g_ctx_watch_hits = 0;
            g_ctx_watch_reason = 0;
            trace_write("[sched] ctxwatch armed ctx=");
            trace_do(serial_write_hex64(g_ctx_watch_ctx));
        }
        if (slot->stack_top >= sizeof(uint64_t)) {
            g_pm_stack_watch = ptr_cast(uint64_t, (slot->stack_top - sizeof(uint64_t)));
            trace_write("[sched] pm stack watch addr=");
            trace_do(serial_write_hex64(addr_cast(uint64_t, g_pm_stack_watch)));
        }
    }
    if (name && strcmp(name, "preempt-busy") == 0) {
        trace_write("[sched] spawn preempt-busy rip=");
        trace_do(serial_write_hex64(slot->ctx.rip));
        trace_write("[sched] spawn preempt-busy rsp=");
        trace_do(serial_write_hex64(slot->ctx.rsp));
        trace_write("[sched] spawn preempt-busy stack base=");
        trace_do(serial_write_hex64(slot->stack_base));
        trace_write("[sched] spawn preempt-busy stack top=");
        trace_do(serial_write_hex64(slot->stack_top));
    }
    /* Publish: NEW -> LIVE.  Parked spawns land in BLOCKED (unparked later);
     * normal spawns land READY and are enqueued.  Must succeed: the slot is
     * still NEW (never published, so no scheduler/kill path can reach it). */
    process_state_t initial_proc_state =
        enqueue_initial ? PROCESS_STATE_READY : PROCESS_STATE_BLOCKED;
    if (!process_transit(slot, PROCESS_STATE_NEW, initial_proc_state)) {
        process_sched_invariant_fail("spawn publish NEW->LIVE failed", pid, (uint64_t)slot->state);
    }
    if (!enqueue_initial) {
        slot->block_reason = PROCESS_BLOCK_NONE;
    } else {
        sched_spawn_thread(process_main_thread(slot));
    }
    *out_pid = pid;
    return 0;
}

/* Spawn READY and enqueued: the child may be dispatched on any CPU before this
 * call even returns, so nothing may be configured on it afterwards that it needs
 * at startup.  Use the parked form for that. */
int process_spawn_as(uint32_t parent_pid, const char* name, process_entry_t entry, void* arg,
                     uint32_t* out_pid) {
    return process_spawn_as_internal(
        parent_pid, name, entry, arg, out_pid, THREAD_STATE_READY, THREAD_BLOCK_NONE, 1);
}

int process_spawn_as_parked(uint32_t parent_pid, const char* name, process_entry_t entry, void* arg,
                            uint32_t* out_pid) {
    /* Spawn with the main thread blocked from the start so no AP can dispatch
     * it before PM explicitly unparks the child. */
    return process_spawn_as_internal(
        parent_pid, name, entry, arg, out_pid, THREAD_STATE_BLOCKED, THREAD_BLOCK_NONE, 0);
}

/* Releases a process spawned parked, making its main thread runnable on the
 * CALLING CPU.  Returns 0 on success AND on the no-op case where the thread was
 * not blocked (already unparked, or running), -1 only for an unknown pid or a
 * process with no main thread.  Unparking twice is therefore harmless.
 *
 * Everything the child needs — caps, cwd, priority band, ready gating — must be
 * applied BEFORE this call; after it the child can run on any CPU. */
int process_unpark_pid(uint32_t pid) {
    process_t* proc = process_get(pid);
    if (!proc) {
        return -1;
    }
    thread_t* t = process_main_thread(proc);
    if (!t) {
        return -1;
    }
    if (!thread_wake_if_blocked(t->tid)) {
        return 0;
    }
    if (process_transit(proc, PROCESS_STATE_BLOCKED, PROCESS_STATE_READY)) {
        proc->block_reason = PROCESS_BLOCK_NONE;
    }
    /* Enqueue the unparked child on the local (caller) CPU.  Cross-CPU "push"
     * placement at unpark time races with the IPC wake path and destabilises
     * WARP-backed app startup; instead rely on per-CPU work-stealing
     * (cpu_sched_try_steal) to pull this thread onto an idle AP once it is
     * runnable.  Enqueue unconditionally: the on_rq claim inside
     * cpu_sched_enqueue is the only sound "already queued?" test, since this CPU
     * holds no lock over whichever queue would hold the thread. */
    sched_enqueue_thread(t);
    return 0;
}

int process_spawn_as_ready_gated_parked(uint32_t parent_pid, const char* name,
                                        process_entry_t entry, void* arg, uint32_t* out_pid) {
    /* Spawn parked and require explicit notify_ready before PM considers it started. */
    int rc = process_spawn_as_parked(parent_pid, name, entry, arg, out_pid);
    if (rc != 0)
        return rc;
    process_t* proc = process_get(*out_pid);
    if (proc)
        process_set_require_explicit_ready(proc);
    return 0;
}

/* Creates the single system-wide idle process and installs its main thread as
 * the BSP's idle thread.  Returns -1 if one already exists, so it is not
 * idempotent — the second call is a refusal, not a no-op.
 *
 * Deliberately does NOT take g_process_table_lock: it runs once during boot,
 * before any AP is up, and is the documented exception to process_find_slot's
 * locking rule.  The idle thread is pinned by affinity and never enqueued; the
 * per-CPU fallback in cpu_sched_pick_next is what dispatches it.  It runs on the
 * kernel root table, not a per-process one, and is never reaped, so the shared
 * failure paths of the ordinary spawn do not apply.  Additional per-CPU idle
 * threads are added afterwards by process_spawn_idle_ap. */
int process_spawn_idle(const char* name, process_entry_t entry, void* arg, uint32_t* out_pid) {
    if (!entry || !out_pid) {
        return -1;
    }
    if (g_idle_process) {
        return -1;
    }

    process_t* slot = process_find_slot();
    if (!slot) {
        return -1;
    }
    /* Claim the free slot (boot, single-threaded here). Must succeed. */
    if (!process_transit(slot, slot->state, PROCESS_STATE_NEW)) {
        process_sched_invariant_fail("idle spawn claim ->NEW failed", (uint64_t)slot->state, 0);
    }

    uint32_t pid = g_next_pid++;
    mm_context_t* ctx = mm_context_create(pid);
    if (!ctx) {
        return -1;
    }

    slot->pid = pid;
    slot->parent_pid = 0;
    slot->context_id = ctx->id;
    slot->main_tid = 0;
    __atomic_store_n(&slot->thread_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->live_thread_count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->exiting, 0u, __ATOMIC_RELEASE);
    /* Idle is never reaped; publish LIVE now (NEW -> READY). Must succeed. */
    if (!process_transit(slot, PROCESS_STATE_NEW, PROCESS_STATE_READY)) {
        process_sched_invariant_fail(
            "idle spawn publish NEW->READY failed", pid, (uint64_t)slot->state);
    }
    slot->block_reason = PROCESS_BLOCK_NONE;
    slot->wait_target_pid = 0;
    slot->exit_status = 0;
    slot->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    slot->ticks_remaining = slot->time_slice_ticks;
    slot->ticks_total = 0;
    slot->ctx_canary_pre = PROCESS_CTX_CANARY_VALUE;
    slot->ctx_canary_post = PROCESS_CTX_CANARY_VALUE;
    slot->needs_runtime_lock = 0;
    process_clear_runtime_tag(slot);
    (void)process_copy_runtime_tag(slot, "KERNEL");
    slot->entry = entry;
    slot->arg = arg;
    if (process_copy_name(slot, name ? name : "") != 0) {
        return -1;
    }
    if (thread_spawn_main(pid, name ? name : "", &slot->main_tid) != 0) {
        return -1;
    }

    thread_t* main_thread = process_main_thread(slot);
    if (!main_thread) {
        return -1;
    }
    main_thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    main_thread->ticks_remaining = main_thread->time_slice_ticks;
    main_thread->ticks_total = 0;

    __atomic_store_n(&slot->thread_count, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->live_thread_count, 1u, __ATOMIC_RELAXED);
    slot->is_idle = 1;
    uint32_t stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_stack(slot, stack_pages) != 0) {
        return -1;
    }
    slot->ctx.rsp = slot->stack_top - (STACK_REDZONE_BYTES + 8u);
    slot->ctx.user_rsp = slot->ctx.rsp;
    slot->ctx.rip = (uint64_t)process_kernel_alias_addr((uintptr_t)process_trampoline);
    slot->ctx.rflags = 0x200;
    slot->ctx.cs = KERNEL_CS_SELECTOR;
    slot->ctx.ss = KERNEL_DS_SELECTOR;
    slot->ctx.root_table = paging_get_root_table();

    main_thread = process_main_thread(slot);
    if (main_thread) {
        main_thread->ctx = slot->ctx;
        sched_thread_init(main_thread, SCHED_PRIO_IDLE);
        main_thread->cpu_affinity = 1u << cpu_local()->cpu_id;
        cpu_sched()->idle = main_thread;
        cpu_local()->idle_thread = main_thread;
    }

    ksync_spinlock_init(&slot->runtime_lock);
    slot->runtime_lock_owner = 0;
    sched_event_init(&slot->wait_event, SCHED_EVENT_TYPE_PROCESS);
    g_idle_process = slot;
    *out_pid = pid;
    return 0;
}

/* Adds one more idle THREAD, inside the existing idle process, pinned to
 * `cpu_id`, and installs it as that CPU's idle thread.  Returns -1 before
 * process_spawn_idle has run, for cpu_id 0 (the BSP's idle thread comes from
 * process_spawn_idle) and for a cpu_id outside the table.  Called by each AP
 * during its own bring-up, hence the atomic bumps of the shared counters. */
int process_spawn_idle_ap(uint32_t cpu_id) {
    if (!g_idle_process || cpu_id == 0 || cpu_id >= WASMOS_MAX_CPUS) {
        return -1;
    }
    uint32_t tid = 0;
    if (thread_spawn_in_owner(
            g_idle_process->pid, "idle-ap", THREAD_STATE_READY, THREAD_BLOCK_NONE, &tid) != 0) {
        return -1;
    }
    thread_t* thread = thread_get(tid);
    if (!thread) {
        return -1;
    }
    uint32_t stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_thread_stack(thread, stack_pages) != 0) {
        thread_reap(tid);
        return -1;
    }
    thread->ctx.rsp = thread->kstack_top - (STACK_REDZONE_BYTES + 8u);
    thread->ctx.user_rsp = thread->ctx.rsp;
    thread->ctx.rip = (uint64_t)process_kernel_alias_addr((uintptr_t)process_trampoline);
    thread->ctx.rflags = 0x200u;
    thread->ctx.cs = KERNEL_CS_SELECTOR;
    thread->ctx.ss = KERNEL_DS_SELECTOR;
    thread->ctx.root_table = paging_get_root_table();
    thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    thread->ticks_remaining = thread->time_slice_ticks;
    thread->ticks_total = 0;
    sched_thread_init(thread, SCHED_PRIO_IDLE);
    thread->cpu_affinity = 1u << cpu_id;
    /* Atomic: APs self-install their idle threads concurrently during bringup,
     * so these shared idle-process counters can be bumped from several CPUs at
     * once. */
    __atomic_fetch_add(&g_idle_process->thread_count, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_idle_process->live_thread_count, 1u, __ATOMIC_RELAXED);
    /* AP idle threads are never enqueued; dispatched only via the per-CPU
     * fallback path in cpu_sched_pick_next. */
    g_cpus[cpu_id].idle_thread = thread;
    /* Mirror the BSP idle bootstrap: record this thread as the per-CPU
     * scheduler idle thread.  cs->idle is what the two cross-CPU readers in
     * sched_thread.c compare against -- cpu_sched_load() excludes a running
     * idle thread from this CPU's load, and cpu_sched_steal_pick() refuses to
     * steal it.  Leaving it NULL on an AP therefore makes that CPU look busy to
     * the placement path and makes its idle thread a steal candidate.
     * (The work-steal trigger in process_schedule_once_impl is a different
     * field, cpu_local()->idle_thread; see the note there.) */
    g_cpus[cpu_id].sched.idle = thread;
    return 0;
}

/* Adds a kernel worker thread with no work: it is dispatched once and returns
 * PROCESS_RUN_THREAD_EXITED immediately.  Kept for the entry-point-less
 * signature; process_thread_spawn_worker_internal is the useful form.
 *
 * Compatibility shim: preserve legacy signature but create a schedulable
 * worker thread that immediately exits when no explicit entry point exists. */
int process_thread_spawn_internal(uint32_t owner_pid, const char* name, uint32_t* out_tid) {
    return process_thread_spawn_worker_internal(
        owner_pid, name ? name : "thread-worker", process_thread_spawn_default_worker, 0, out_tid);
}

/* Adds a ring-0 worker thread to a live process, running `entry(proc, tid, arg)`
 * on its own kernel stack at SCHED_PRIO_SYSTEM.  Returns 0 with *out_tid set, or
 * -1 for a NULL entry/out_tid, an unknown owner, an owner that is not in a live
 * state or is already exiting, or an exhausted thread/stack/queue resource.
 *
 * `arg` is borrowed and stored verbatim; it must outlive the thread.  The worker
 * shares the owner's address space and context id but not its stack, and its
 * return value is a process_run_result_t interpreted exactly as a process entry
 * point's.  Workers bypass the runtime_lock the trampoline applies to ordinary
 * entry points. */
int process_thread_spawn_worker_internal(uint32_t owner_pid, const char* name,
                                         process_thread_worker_entry_t entry, void* arg,
                                         uint32_t* out_tid) {
    process_t* owner = process_find_by_pid(owner_pid);
    thread_t* thread = 0;
    uint32_t tid = 0;
    uint32_t stack_pages = 0;
    if (!owner || !entry || !out_tid) {
        return -1;
    }
    if (owner->state == PROCESS_STATE_UNUSED || owner->state == PROCESS_STATE_DEAD ||
        owner->state == PROCESS_STATE_NEW || owner->state == PROCESS_STATE_ZOMBIE ||
        owner->state == PROCESS_STATE_REAPING ||
        __atomic_load_n(&owner->exiting, __ATOMIC_ACQUIRE)) {
        return -1;
    }
    /* Spawn PARKED, not READY.  READY publishes the thread to every CPU as a
     * legal wake/enqueue target, and the scheduler fields it needs -- sched_node
     * above all -- are not set up until sched_thread_init() below, with a stack
     * allocation in between.  A wake landing in that window enqueues the thread,
     * and sched_thread_init's list_head_init then self-links the node while the
     * queue's head still points at it: the band is spliced through a node with
     * two owners, its ready bit can never clear, and the picker returns that one
     * node on every dispatch forever.  The user-thread path below already spawns
     * BLOCKED and promotes after init; this is the same contract. */
    if (thread_spawn_in_owner(
            owner_pid, name ? name : "", THREAD_STATE_BLOCKED, THREAD_BLOCK_NONE, &tid) != 0) {
        return -1;
    }
    thread = thread_get(tid);
    if (!thread) {
        return -1;
    }
    stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_thread_stack(thread, stack_pages) != 0) {
        thread_reap(tid);
        return -1;
    }
    thread->is_kernel_worker = 1;
    thread->worker_entry = (uintptr_t)entry;
    thread->worker_arg = arg;
    thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    thread->ticks_remaining = thread->time_slice_ticks;
    thread->ticks_total = 0;
    sched_thread_init(thread, SCHED_PRIO_SYSTEM);
    (void)__atomic_fetch_add(&owner->thread_count, 1u, __ATOMIC_ACQ_REL);
    (void)__atomic_fetch_add(&owner->live_thread_count, 1u, __ATOMIC_ACQ_REL);
    /* Scheduler state is now complete: publish the thread as runnable, then
     * enqueue it.  sched_spawn_thread -> cpu_sched_enqueue only accepts a READY
     * thread, so the promotion must precede it. */
    if (!thread_transit(thread, THREAD_STATE_BLOCKED, THREAD_STATE_READY)) {
        process_count_dec(&owner->thread_count);
        process_count_dec(&owner->live_thread_count);
        thread_reap(tid);
        return -1;
    }
    sched_spawn_thread(thread);
    *out_tid = tid;
    return 0;
}

/* Adds a ring-3 thread to a live process: its own kernel stack for trap entry,
 * plus a user context (USER_CS/SS, entry_rip, user_stack_top) on the owner's
 * root table.  Returns 0 with *out_tid set, or -1.
 *
 * user_stack_top is rounded DOWN to 16 bytes rather than refused, because the
 * SysV entry alignment is the kernel's to establish, not the guest's to get
 * right.  entry_rip and user_stack_top of 0 are refused; neither is otherwise
 * validated against the owner's mappings, so a bad address faults in ring 3.
 *
 * Spawns BLOCKED and promotes via process_wake_thread only after
 * sched_thread_init, for the reason spelled out on the worker path above.  Note
 * the polarity: process_wake_thread returns NON-zero on success, so the 0 case
 * here is the failure that unwinds the counters and reaps the slot. */
int process_thread_spawn_user_internal(uint32_t owner_pid, const char* name, uint64_t entry_rip,
                                       uint64_t user_stack_top, uint32_t* out_tid) {
    process_t* owner = process_find_by_pid(owner_pid);
    thread_t* thread = 0;
    uint32_t tid = 0;
    uint32_t stack_pages = 0;
    uint64_t user_root = 0;
    if (!owner || !out_tid || entry_rip == 0 || user_stack_top == 0) {
        return -1;
    }
    if (owner->state == PROCESS_STATE_UNUSED || owner->state == PROCESS_STATE_DEAD ||
        owner->state == PROCESS_STATE_NEW || owner->state == PROCESS_STATE_ZOMBIE ||
        owner->state == PROCESS_STATE_REAPING ||
        __atomic_load_n(&owner->exiting, __ATOMIC_ACQUIRE)) {
        return -1;
    }
    if ((user_stack_top & 0xFULL) != 0) {
        user_stack_top &= ~0xFULL;
    }
    if (thread_spawn_in_owner(owner_pid,
                              name ? name : "user-thread",
                              THREAD_STATE_BLOCKED,
                              THREAD_BLOCK_NONE,
                              &tid) != 0) {
        return -1;
    }
    thread = thread_get(tid);
    if (!thread) {
        thread_reap(tid);
        return -1;
    }
    stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_thread_stack(thread, stack_pages) != 0) {
        thread_reap(tid);
        return -1;
    }
    user_root = mm_context_root_table(owner->context_id);
    if (user_root == 0) {
        thread_reap(tid);
        return -1;
    }
    thread->ctx.rip = entry_rip;
    thread->ctx.cs = USER_CS_SELECTOR;
    thread->ctx.ss = USER_DS_SELECTOR;
    thread->ctx.user_rsp = user_stack_top;
    thread->ctx.rsp = thread->kstack_top - (STACK_REDZONE_BYTES + 8u);
    thread->ctx.rflags = 0x200;
    thread->ctx.root_table = user_root;
    thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    thread->ticks_remaining = thread->time_slice_ticks;
    thread->ticks_total = 0;
    sched_thread_init(thread, SCHED_PRIO_WASM);
    (void)__atomic_fetch_add(&owner->thread_count, 1u, __ATOMIC_ACQ_REL);
    (void)__atomic_fetch_add(&owner->live_thread_count, 1u, __ATOMIC_ACQ_REL);
    if (process_wake_thread(tid) == 0) {
        process_count_dec(&owner->thread_count);
        process_count_dec(&owner->live_thread_count);
        thread_reap(tid);
        return -1;
    }
    *out_tid = tid;
    return 0;
}

/* Converts an already-spawned process's MAIN thread to ring 3 at (rip,
 * user_rsp).  Returns 0 on success, -1 for an unknown pid, a zero rip/rsp, a
 * kernel stack that is not higher-half, or a root table that still exposes the
 * low slot after stripping.
 *
 * Irreversible for the whole process, not just this thread: it strips the
 * identity-mapped low slot out of the shared root table, so every existing and
 * future thread of that process loses low-half addressability at the same
 * moment.  The higher-half stack check is the precondition that makes that safe
 * — a kernel stack below the higher-half base would become unmapped by the very
 * strip this function performs.
 *
 * TODO: Wire this into process-manager launch policy once the first
 * user-mode service/app path is selected and validated end-to-end. */
int process_set_user_entry(uint32_t pid, uint64_t rip, uint64_t user_rsp) {
    process_t* proc = process_find_by_pid(pid);
    uint64_t higher_half_base = paging_get_higher_half_base();
    if (!proc || rip == 0 || user_rsp == 0) {
        return -1;
    }
    if (proc->stack_base < higher_half_base || proc->stack_top < higher_half_base) {
        return -1;
    }
    uint64_t user_root = mm_context_root_table(proc->context_id);
    if (paging_strip_low_slot_in_root(user_root) != 0) {
        return -1;
    }
    if (paging_verify_user_root_no_low_slot(user_root, 1) != 0) {
        return -1;
    }
    proc->ctx.rip = rip;
    proc->ctx.cs = USER_CS_SELECTOR;
    proc->ctx.ss = USER_DS_SELECTOR;
    proc->ctx.user_rsp = user_rsp;
    proc->ctx.rflags = 0x200;

    thread_t* main_thread = process_main_thread(proc);
    if (main_thread) {
        main_thread->ctx = proc->ctx;
        main_thread->ctx.root_table = user_root;
    }

    return 0;
}

/* Slot for `pid`, or 0 for pid 0 and for a pid with no live slot.  Lock-free on
 * the terms set out on process_find_by_pid: the pointer is valid only while the
 * caller has an independent reason to believe that process is alive, because a
 * reaped slot is recycled by an unrelated spawn.  A ZOMBIE (exited, unreaped)
 * process still resolves — that is how the wait/status paths read its exit
 * status — so a non-NULL result does not mean "running". */
process_t* process_get(uint32_t pid) {
    return process_find_by_pid(pid);
}

/* Slot owning `context_id` (the mm/IPC context id carried on every message), or
 * 0.  This is how a handler turns a message's endpoint owner into a caller
 * identity.  Same lifetime and ZOMBIE caveats as process_get. */
process_t* process_find_by_context(uint32_t context_id) {
    return process_find_by_context_internal(context_id);
}

/* Pid dispatched on the CALLING CPU, or 0 when nothing is (inside the scheduler,
 * or before the first dispatch).  Read through the higher-half alias so the
 * answer is the same whether this code is executing from the kernel's linked
 * addresses or from its low-mapped boot alias. */
uint32_t process_current_pid(void) {
    uint32_t* pid_ptr =
        (uint32_t*)(void*)process_kernel_alias_addr((uintptr_t)&cpu_local()->current_pid);
    return *pid_ptr;
}

/* Stages the status a process will exit with.  Records it only — the process
 * keeps running, and the exit paths (process_mark_exited, the EXITED branch of
 * the dispatch loop) read it back when the timeslice ends.  This is how a
 * ring-3 exit syscall communicates its status before yielding EXITED. */
void process_set_exit_status(process_t* process, int32_t exit_status) {
    if (!process) {
        return;
    }
    process->exit_status = exit_status;
}

/* Switches out of the running thread and back into this CPU's scheduler context,
 * publishing `result` as how the timeslice ended.  Returns only when the
 * scheduler dispatches this thread again — possibly on a different CPU.
 *
 * `result` is what selects the completion path in process_schedule_once_impl:
 * YIELDED re-enqueues and marks the thread sticky, BLOCKED expects it to be
 * parked on an event already (sched_event_wait sets that up before calling
 * here), EXITED/THREAD_EXITED tombstone it.  Passing BLOCKED without having
 * blocked is tolerated — the RUNNING branch there treats it as a yield — but
 * passing YIELDED after blocking would unlink the thread from its wait list.
 *
 * With no current process (the scheduler's own context) it returns immediately,
 * which for a caller expecting to park means it spins instead. */
void process_yield(process_run_result_t result) {
    if (!cpu_local()->current_process) {
        return;
    }
    cpu_local()->last_run_result = result;
    thread_t* thread = cpu_local()->current_thread;
    process_context_t* ctx = process_sched_ctx_for_thread(cpu_local()->current_process, thread);
    if (!ctx) {
        return;
    }
    /* Save the per-CPU wasm3 heap binding so that when this thread resumes
     * (possibly on a different CPU) the correct heap PID is restored.
     * Without this, a CPU that runs a different WASM process after this
     * thread blocks would inherit a stale wasm3_heap_bound_pid. */
    if (thread) {
        thread->wasm3_heap_bound_pid = cpu_local()->wasm3_heap_bound_pid;
    }
    context_switch_high(ctx, &cpu_local()->sched_ctx);
}

/* Marks a process as one that must announce itself (PROC_IPC_NOTIFY_READY /
 * process_notify_ready) before the PM treats it as started.  One-way: there is
 * no clear.  Meaningful only before the child first runs, which is why the PM
 * sets it on a still-parked child. */
void process_set_require_explicit_ready(process_t* process) {
    if (!process) {
        return;
    }
    process->require_explicit_ready = 1;
}

/* process_block_on_ipc: no-op compatibility stub.  Thread blocking state is
 * managed by sched_event_wait, so a caller returning PROCESS_RUN_BLOCKED has no
 * process state to touch here.
 * TODO: remove once all callers are updated. */
void process_block_on_ipc(process_t* process) {
    (void)process;
}

/* Latches the "child has announced itself" flag the sync-spawn poll waits on.
 * A latch, not an event: setting it twice is harmless and it is never cleared
 * for the life of the slot.  Does not wake or unblock anything by itself —
 * pm_poll_sync_spawn observes it on its next PM dispatch. */
void process_notify_ready(process_t* process) {
    if (!process) {
        return;
    }
    process->ready = 1;
}

/* One step of a parent's wait for `target_pid`.  Tri-state, and the caller loops
 * on it: 0 means the child was a zombie, *out_exit_status is filled and its slot
 * has been reaped; 1 means the caller has been parked and must
 * process_yield(PROCESS_RUN_BLOCKED) and call again on resume; -1 means the wait
 * is impossible and never will succeed (target 0, waiting on itself, no such
 * pid, or the target is not this process's child).
 *
 * The parent check is enforced here, which is why the PM — whose interactive
 * children are parented to the CLI, not to it — cannot use this and reaps
 * through process_reap_zombie_pid instead.  The reap is CAS-guarded, so racing
 * an auto-reap on another CPU is safe; only one side frees the slot. */
int process_wait(process_t* process, uint32_t target_pid, int32_t* out_exit_status) {
    if (!process || target_pid == 0 || process->pid == target_pid) {
        return -1;
    }

    process_t* target = process_find_by_pid(target_pid);
    if (!target) {
        return -1;
    }
    if (target->parent_pid != process->pid) {
        return -1;
    }

    if (target->state == PROCESS_STATE_ZOMBIE) {
        if (out_exit_status) {
            *out_exit_status = target->exit_status;
        }
        /* CAS-guarded: safe if another CPU (auto-reap) races the same zombie. */
        process_reap_claim(target);
        process->block_reason = PROCESS_BLOCK_NONE;
        return 0;
    }

    thread_t* thread = process_thread_for_transition(process);
    process_set_blocked(process, thread, PROCESS_BLOCK_WAIT, THREAD_BLOCK_WAIT_PROCESS);
    if (thread) {
        thread->wait_target_pid = target_pid;
    }
    return 1;
}

/* Tri-state return: > 0 means the caller has been parked and should yield and
 * retry, 0 means the target was reaped and *out_exit_status is set, < 0 is a
 * packed WASMOS_ERR_* / WASMOS_INVAL code (packed codes are negative, so the
 * three cases never collide). */
int process_thread_join(process_t* process, uint32_t target_tid, int32_t* out_exit_status) {
    thread_t* target = 0;
    thread_t* caller = 0;
    uint32_t caller_tid = 0;
    if (!process || target_tid == 0) {
        return WASMOS_INVAL;
    }
    caller_tid = thread_current_tid();
    if (caller_tid == 0) {
        return WASMOS_ERR_KERNEL_NO_CALLER;
    }
    if (caller_tid == target_tid) {
        return WASMOS_INVAL; /* a thread cannot join itself */
    }
    target = thread_get(target_tid);
    caller = thread_get(caller_tid);
    if (!target || !caller) {
        return WASMOS_ERR_THREAD_NOT_FOUND;
    }
    if (target->owner_pid != process->pid || caller->owner_pid != process->pid) {
        return WASMOS_ERR_THREAD_NOT_OWNER;
    }
    if (target->detached) {
        return WASMOS_ERR_THREAD_JOIN_FAILED;
    }
    if (target->state == THREAD_STATE_ZOMBIE) {
        if (out_exit_status) {
            *out_exit_status = target->exit_status;
        }
        thread_reap(target->tid);
        process_count_dec(&process->thread_count);
        return 0;
    }
    if (target->join_waiter_tid != 0 && target->join_waiter_tid != caller_tid) {
        return WASMOS_ERR_THREAD_BUSY;
    }
    target->join_waiter_tid = caller_tid;
    process_set_blocked(process, caller, PROCESS_BLOCK_WAIT, THREAD_BLOCK_WAIT_THREAD);
    caller->wait_target_pid = 0;
    return 1;
}

/* Marks a thread as detached: nobody will join it, so it is reaped as soon as it
 * exits rather than left as a joinable zombie.  Returns 0 on success (including
 * detaching an already-detached thread), or a packed negative code — WASMOS_INVAL
 * for target_tid 0, NO_CALLER with no current thread, THREAD_NOT_FOUND,
 * THREAD_NOT_OWNER for a thread of another process, THREAD_BUSY when a DIFFERENT
 * thread is already parked joining it.
 *
 * Detaching a thread that has already exited reaps it immediately here rather
 * than deferring, since the exit path has already run and will not revisit it.
 * One-way: there is no re-attach, and a later join of a detached thread is
 * refused with WASMOS_ERR_THREAD_JOIN_FAILED. */
int process_thread_detach(process_t* process, uint32_t target_tid) {
    thread_t* target = 0;
    uint32_t caller_tid = 0;
    if (!process || target_tid == 0) {
        return WASMOS_INVAL;
    }
    caller_tid = thread_current_tid();
    if (caller_tid == 0) {
        return WASMOS_ERR_KERNEL_NO_CALLER;
    }
    target = thread_get(target_tid);
    if (!target) {
        return WASMOS_ERR_THREAD_NOT_FOUND;
    }
    if (target->owner_pid != process->pid) {
        return WASMOS_ERR_THREAD_NOT_OWNER;
    }
    if (target->join_waiter_tid != 0 && target->join_waiter_tid != caller_tid) {
        return WASMOS_ERR_THREAD_BUSY;
    }
    target->detached = 1;
    if (target->state == THREAD_STATE_ZOMBIE) {
        thread_reap(target->tid);
        process_count_dec(&process->thread_count);
    }
    return 0;
}

/* Terminates `pid` with `exit_status`: tombstones its threads, wakes anyone
 * waiting on it, and auto-reaps if that is enabled and nothing is waiting.
 * Returns 0 on success and 0 again for an already-zombie target (the
 * postcondition holds either way); -1 for an unknown pid, for suicide, and when
 * the caller is a process that is not the target's parent.
 *
 * The parent check is skipped when current_pid is 0 — i.e. when the kernel calls
 * this outside any process — which is what lets in-kernel cleanup paths kill a
 * child they did not spawn.  Asynchronous: the target is not off-CPU on return,
 * a thread of it may still be finishing a timeslice on another CPU. */
int process_kill(uint32_t pid, int32_t exit_status) {
    process_t* target = process_find_by_pid(pid);
    if (!target) {
        return -1;
    }
    if (pid == cpu_local()->current_pid) {
        return -1;
    }
    if (cpu_local()->current_pid != 0 && target->parent_pid != cpu_local()->current_pid) {
        return -1;
    }
    if (target->state == PROCESS_STATE_ZOMBIE) {
        return 0;
    }
    process_mark_exited(target, exit_status);
    process_try_auto_reap(target);
    return 0;
}

/* Enables or disables "free this slot as soon as it exits with nobody waiting".
 * Returns 0, or -1 for an unknown or free slot.  Enabling it re-tests
 * immediately, so calling this on a process that has ALREADY exited reaps it
 * right here — which is why the PM sets it before unparking a one-shot child,
 * not after.
 *
 * Must not be enabled for a process the caller intends to PROC_IPC_WAIT on:
 * process_has_waiters only sees kernel-side waiters (threads blocked in
 * process_wait), not PM's IPC wait list, so auto-reap would free the slot before
 * the status reply is built. */
int process_set_auto_reap(uint32_t pid, uint8_t enabled) {
    process_t* proc = process_find_by_pid(pid);
    if (!proc || proc->state == PROCESS_STATE_UNUSED) {
        return -1;
    }
    proc->auto_reap = enabled ? 1u : 0u;
    process_try_auto_reap(proc);
    return 0;
}

/* Reads an exited process's status.  THREE-valued and NOT 0-on-failure: 0 means
 * the process is a zombie and *out_exit_status is now set, 1 means it exists but
 * has not exited (nothing written), -1 means unknown pid or NULL out pointer.
 * Callers therefore test `!= 0` for "no status yet", which folds the
 * still-running and does-not-exist cases together.
 *
 * Non-destructive: the zombie is left for a wait or a reap to collect, so this
 * can be polled. */
int process_get_exit_status(uint32_t pid, int32_t* out_exit_status) {
    process_t* proc = process_find_by_pid(pid);
    if (!proc || !out_exit_status) {
        return -1;
    }
    if (proc->state != PROCESS_STATE_ZOMBIE) {
        return 1;
    }
    *out_exit_status = proc->exit_status;
    return 0;
}

/* Wakes a blocked thread.  Returns NON-zero on success and 0 on failure — the
 * inverse of the 0-on-success convention most of this file uses, so check the
 * polarity at every call site.  0 covers tid 0, an unknown tid, and a thread
 * that is not BLOCKED (already running or ready), none of which is an error
 * worth distinguishing at a wake site.
 *
 * The actual wake goes through sched_wake_thread, so it participates in the
 * Dekker handshake with the blocking-completion path: the thread may end up
 * enqueued by this call or by the CPU it is yielding off, never by both. */
int process_wake_thread(uint32_t tid) {
    if (tid == 0) {
        return 0;
    }
    thread_t* thread = thread_get(tid);
    if (!thread) {
        return 0;
    }
    if (thread->state != THREAD_STATE_BLOCKED) {
        return 0;
    }
    sched_wake_thread(thread);
    return 1;
}

/* Runs one dispatch round on the calling CPU: pick a thread, switch into it, and
 * handle whatever its timeslice produced.  Returns SCHED_OK or one of the
 * SCHED_R_* codes; only SCHED_R_PICK, SCHED_R_CTX, SCHED_R_ROOT and
 * SCHED_R_MAXCOUNT are fatal to the boot loop, the rest are recoverable races
 * (a thread reaped mid-pick, a process that went zombie) and mean "try again".
 *
 * Blocks for the whole timeslice of whatever it dispatches.
 *
 * This wrapper exists only to normalise where the work runs before entering
 * process_schedule_once_impl.  Two independent corrections: if this code is
 * executing from the kernel's low boot alias, it re-enters itself through the
 * higher-half address; and if it arrived on a low-mapped stack, it runs the impl
 * on this CPU's dedicated scheduler stack.  Both matter because the impl
 * switches CR3 to a user root table, under which low addresses are gone. */
int process_schedule_once(void) {
    uint64_t higher_half_base = paging_get_higher_half_base();
    uintptr_t here = 0;
    uintptr_t rsp_cur = 0;
#ifdef WASMOS_PROCESS_TEST_SEAMS
    /* No higher-half alias on the host: report addresses that make the
     * relocation checks below no-ops rather than fabricating a mapping. */
    here = (uintptr_t)higher_half_base;
    rsp_cur = (uintptr_t)higher_half_base;
#else
    __asm__ volatile("leaq 0f(%%rip), %0\n0:" : "=r"(here));
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_cur));
#endif
    if ((uint64_t)here < higher_half_base) {
        uintptr_t high_fn = (uintptr_t)&process_schedule_once;
        high_fn += (uintptr_t)higher_half_base;
        int (*fn_high)(void) = (int (*)(void))high_fn;
        return fn_high();
    }
    if ((uint64_t)rsp_cur < higher_half_base) {
        /* TODO(ring3-phase2): Delete this trampoline once all scheduler/trap
         * ingress paths are guaranteed to arrive on higher-half stacks. */
        return process_run_on_sched_stack(process_schedule_once_impl);
    }
    return process_schedule_once_impl();
}

static int process_schedule_once_impl(void) {
    /* Single-exit state. Every path after the dispatch reference is taken below
     * leaves through `dispatch_done`, which is the only place that releases it --
     * a leaked reference would make the thread's slot permanently unreapable, so
     * the release must not depend on remembering it at nine separate returns. */
    int sched_rc = SCHED_OK;
    uint32_t reap_pid = 0;
    uint32_t reap_tid = 0;
    if (PROCESS_MAX_COUNT == 0) {
        return SCHED_R_MAXCOUNT;
    }

    /* Fire any timed waits whose deadline has passed before picking the next
     * thread. Cheap when nothing is armed; runs in scheduler context (no
     * run-queue lock held) so it can safely wake/enqueue. */
    sched_timeout_check();

    cpu_sched_t* cs = cpu_sched();
    ksync_spinlock_lock(&cs->lock);
    thread_t* thread = cpu_sched_pick_next(cs);
    ksync_spinlock_unlock(&cs->lock);
    /* No idle thread at all is the one genuinely impossible state.  SCHED_R_PICK
     * is the only code the boot loop (kernel_boot_runtime.c) panics on, alongside
     * the CTX/ROOT/MAXCOUNT failures below; everything else here can legitimately
     * lose its thread to a concurrent reap and must stay recoverable. */
    if (!thread) {
        return SCHED_R_PICK;
    }
    /* Compare against the field cpu_sched_pick_next actually answered from --
     * cpu_local()->idle_thread -- not cs->idle.  They are two fields of the same
     * CPU and are set together at bringup, but only one of them decides what
     * pick_next returns.  Testing the other means a window where they disagree
     * (an AP after process_ap_init, before g_cpus[id].sched.idle is set) leaves
     * this trigger permanently false: the CPU idles while runnable work piles up
     * elsewhere, with nothing to indicate why.  Pinned by the P5 case in
     * tests/unit/test_sched_runqueue.c. */
    uint8_t picked_idle = (thread == cpu_local()->idle_thread) ? 1u : 0u;
    if (picked_idle) {
        thread_t* stolen = cpu_sched_try_steal(cpu_local()->cpu_id);
        if (stolen) {
            /* Do not give up a dispatchable idle for a thread that may already
             * be gone. cpu_sched_steal_pick pulls the thread off the remote
             * queue and drops that queue's lock before this point, while
             * thread_reap_owner walks the global thread table by owner_pid --
             * so a reap running on any CPU can reset a thread that a stealer is
             * already holding. Validating before the swap retains the
             * dispatchable idle; swapping first turns that race into "not even
             * idle was dispatchable", which is a claim about a different bug. */
            process_t* sproc = process_owner_for_thread(stolen);
            if (sproc && sproc->entry) {
                thread = stolen;
                picked_idle = 0u;
            } else {
                /* Reaped mid-steal. It is already off every queue and its slot
                 * is being torn down, so it is dropped rather than re-queued --
                 * the same disposition cpu_sched_pick_next gives stale nodes.
                 *
                 * Counted because "off every queue" is the part that needs
                 * watching: cpu_sched_steal_pick unlinked this thread and
                 * released its on_rq before handing it over, so the drop is
                 * final. Correct while the owner really is gone, and a strand if
                 * it is not -- and the two are indistinguishable after the fact,
                 * which is why the count exists at all. */
                uint32_t dn = sched_debug_note(SCHED_DEBUG_DISPATCH_DROPPED_STEAL_REAPED);
                if ((dn & (dn - 1u)) == 0u) {
                    serial_printf(
                        "[sched] steal dropped tid=%u owner=%u state=%u cpu=%u (n=%u)\n",
                        (unsigned)stolen->tid,
                        (unsigned)stolen->owner_pid,
                        (unsigned)__atomic_load_n((uint32_t*)&stolen->state, __ATOMIC_ACQUIRE),
                        (unsigned)cpu_local()->cpu_id,
                        (unsigned)(dn + 1u));
                }
                return SCHED_R_STALE;
            }
        }
    }
    /* The identity this dispatch is about, captured once `thread` is final -- the
     * steal branch above can replace it, so snapshotting before that compares the
     * idle thread's tid against the stolen thread's and rejects every steal.
     *
     * A thread_t is a SLOT: thread_reset_slot zeroes tid/owner_pid and hands it
     * back to the allocator, and the next spawn re-stamps both.  Every check below
     * reads through the pointer, so without a snapshot to compare against, a slot
     * reaped and re-claimed between here and the dispatch passes each check
     * individually -- as a different thread, of a different process.  See the
     * re-validation after the dispatch claim. */
    uint32_t picked_tid = thread->tid;
    /* Claim the slot for the rest of this dispatch, INCLUDING the result
     * handling. CAS from FREE, not an unconditional increment: the pick above has
     * already dropped the queue lock, so a reaper can be freeing this very slot
     * right now, and an increment would simply land on top of a teardown (or on
     * the next spawn's thread). Losing the CAS means the slot is not ours to run.
     *
     * thread_reset_slot contests the same word, and process_reap_claim refuses
     * while it is held, so neither the thread slot nor its process slot can be
     * recycled underneath the raw pointers this function carries across the
     * switch. */
    uint32_t slot_claim = THREAD_SLOT_FREE;
    if (!__atomic_compare_exchange_n(&thread->dispatch_ref,
                                     &slot_claim,
                                     THREAD_SLOT_DISPATCH,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        /* Being torn down, or already claimed by another CPU that raced us to the
         * same pick. Either way it is the reap race this file already treats as
         * normal.
         *
         * What has ALREADY been touched: cpu_sched_pick_next (or
         * cpu_sched_steal_pick) unlinked this thread under the queue lock and
         * released its on_rq before returning it, so its place in the run queue
         * is gone. Only the SLOT claim is untaken. Returning without accounting
         * for the lost place strands a live thread permanently -- READY, on no
         * queue, owed nothing, so no sweep can find it -- which is why this exit
         * leaves a debt below. Counted as well, so that reading "no strand came
         * from here" means something. */
        uint32_t dn = sched_debug_note(SCHED_DEBUG_DISPATCH_DROPPED_SLOT_LOST);
        if ((dn & (dn - 1u)) == 0u) {
            serial_printf("[sched] slot claim lost tid=%u owner=%u state=%u ref=%u cpu=%u "
                          "(n=%u)\n",
                          (unsigned)picked_tid,
                          (unsigned)thread->owner_pid,
                          (unsigned)__atomic_load_n((uint32_t*)&thread->state, __ATOMIC_ACQUIRE),
                          (unsigned)slot_claim,
                          (unsigned)cpu_local()->cpu_id,
                          (unsigned)(dn + 1u));
        }
        /* Hand the enqueue this pick consumed to whoever can honour it. Only for
         * a claim held by another DISPATCH: a FROZEN slot is thread_reset_slot
         * mid-teardown, whose thread is meant to end unqueued and whose debts it
         * discards anyway. */
        if (slot_claim == THREAD_SLOT_DISPATCH) {
            sched_owe_enqueue_for_dropped_pick(thread);
        }
        return SCHED_R_STALE;
    }
    process_t* proc = process_owner_for_thread(thread);
    if (!proc || !proc->entry) {
        /* Idle losing its owner really is the panic case; any other thread
         * losing one is the reap race above, seen a few instructions later. */
        sched_rc = picked_idle ? SCHED_R_PICK : SCHED_R_STALE;
        goto dispatch_done;
    }
    /* Arm the deferred-reap retry now, not at the tail. A one-shot reaper
     * (process_reap_zombie_pid from the PM) can be refused at any point while
     * this dispatch holds its claim, including during an early exit -- non-READY,
     * claim lost, missing context or root, a failed process_set_running. Setting
     * this only on the paths that run to completion left those exits skipping the
     * retry, which strands the zombie slot permanently. dispatch_done checks
     * reap_requested before acting, so arming it unconditionally costs one
     * process_find_by_pid on paths that had no reap pending. */
    reap_pid = proc->pid;
    /* Thread state alone determines runnability. */
    if (thread->state != THREAD_STATE_READY) {
        /* Rate-limited to powers of two. A non-READY thread parked in a ready
         * queue is re-picked on every scheduling attempt, so logging each one
         * floods the serial line at scheduler speed: it shreds the surrounding
         * output (concurrent unlocked writes interleave mid-line) and slows the
         * loop enough to perturb the race that put it there. Powers of two keep
         * the first report, which is the one that names the original thread,
         * and the running count shows the magnitude.
         *
         * The LOCKED writer, despite running inside the scheduler's own cli
         * window: g_serial_lock is a leaf (nothing reachable from serial_write
         * takes a scheduler lock), spinlock_lock's IRQ-disable and preempt
         * depths are per-CPU counters so nesting it inside that window leaves
         * both unchanged, and preempt_enable only decrements -- it cannot
         * re-enter the scheduler. Writing unlocked here buys nothing (the UART
         * spin is the same either way) and costs coherence: another CPU's line
         * interleaves mid-string, corrupting both. */
        static uint32_t notready_seen;
        uint32_t n = __atomic_fetch_add(&notready_seen, 1u, __ATOMIC_RELAXED);
        if ((n & (n - 1u)) == 0u) {
            serial_printf("[sched] dequeued non-ready tid=%u pid=%u state=%u block=%u (n=%u)\n",
                          (unsigned)thread->tid,
                          (unsigned)(proc ? proc->pid : 0u),
                          (unsigned)thread->state,
                          (unsigned)thread->block_reason,
                          (unsigned)(n + 1u));
        }
        sched_rc = SCHED_R_NOTREADY;
        goto dispatch_done;
    }
    /* The READY test above is the cheap reject; this claim is what makes the
     * dispatch exclusive (see cpu_sched_claim_for_dispatch).  The loser drops the
     * thread -- the winner owns it and re-enqueues it when its dispatch ends. */
    if (!cpu_sched_claim_for_dispatch(thread)) {
        static uint32_t claim_lost_seen;
        uint32_t cn = __atomic_fetch_add(&claim_lost_seen, 1u, __ATOMIC_RELAXED);
        if ((cn & (cn - 1u)) == 0u) {
            serial_printf("[sched] claim lost tid=%u pid=%u cpu=%u state=%u (n=%u)\n",
                          (unsigned)thread->tid,
                          (unsigned)proc->pid,
                          (unsigned)cpu_local()->cpu_id,
                          (unsigned)__atomic_load_n((uint32_t*)&thread->state, __ATOMIC_ACQUIRE),
                          (unsigned)(cn + 1u));
        }
        sched_rc = SCHED_R_CLAIMED;
        goto dispatch_done;
    }

    /* Re-validate the identity now that the claim is held.  The steal path above
     * asks only "does this thread still have an owner", which a slot that has
     * been reaped AND re-claimed by a new spawn answers yes to -- with the new
     * owner.  Comparing against the snapshot is what distinguishes "still the
     * thread we picked" from "same slot, different thread": a mismatch means the
     * slot was recycled mid-flight, which is the reap race the file already
     * treats as normal (SCHED_R_STALE), not a defect.
     *
     * Releasing the claim mirrors the terminal-state path below.  If the slot was
     * reset rather than re-claimed its state is UNUSED, so the transit simply
     * fails and leaves it alone; if it was re-claimed, handing it back to READY
     * is the repair -- this CPU marked a brand-new thread RUNNING that it has no
     * business running. */
    if (thread->tid != picked_tid || thread->owner_pid != proc->pid) {
        (void)thread_transit(thread, THREAD_STATE_RUNNING, THREAD_STATE_READY);
        sched_rc = SCHED_R_STALE;
        goto dispatch_done;
    }

    if (!process_set_running(proc, thread)) {
        /* Process raced to a terminal state between the READY check and now;
         * do not dispatch it.  Its thread will be reaped via the zombie path.
         * Release the claim so the state matches the pre-claim disposition. */
        (void)thread_transit(thread, THREAD_STATE_RUNNING, THREAD_STATE_READY);
        sched_rc = SCHED_R_ZOMBIE;
        goto dispatch_done;
    }
    if (thread->ticks_remaining == 0) {
        thread->ticks_remaining = thread->time_slice_ticks;
    }
    if (thread->time_slice_ticks == 0) {
        process_sched_invariant_fail("zero time slice", thread->tid, 0);
    }
    process_context_t* run_ctx = process_sched_ctx_for_thread(proc, thread);
    critical_section_enter();
    cpu_local()->current_pid = proc->pid;
    cpu_local()->last_dispatched_pid = proc->pid;
    cpu_local()->current_process = proc;
    cpu_local()->current_thread = thread;
    thread->last_cpu = cpu_local()->cpu_id;
    /* Dispatching clears the sticky (just-yielded) hint; it is re-set only if
     * this run ends in another voluntary yield. */
    thread->sched_sticky = 0;
    if (cpu_local()->current_thread->owner_pid != cpu_local()->current_process->pid) {
        process_sched_invariant_fail("current owner mismatch",
                                     cpu_local()->current_thread->owner_pid,
                                     cpu_local()->current_process->pid);
    }
    thread_set_current(thread ? thread->tid : 0);
    critical_section_leave();
    /* Ring3 transitions use TSS.rsp0 as the kernel entry stack. Keep it aligned
     * to the scheduled process stack so user-mode interrupts/syscalls have a
     * deterministic kernel stack landing point. */
    cpu_set_kernel_stack((uint64_t)(proc->stack_top - 16u));
    /* Restore the wasm3 heap binding saved when this thread last yielded. */
    cpu_local()->wasm3_heap_bound_pid = thread->wasm3_heap_bound_pid;
    cpu_local()->sched_ctx.root_table = paging_get_root_table();
    if (!run_ctx) {
        klog_write("[sched] thread ctx missing\n");
        cpu_local()->in_scheduler = 1;
        critical_section_enter();
        cpu_local()->current_process = 0;
        cpu_local()->current_pid = 0;
        cpu_local()->current_thread = 0;
        thread_set_current(0);
        critical_section_leave();
        sched_rc = SCHED_R_CTX;
        goto dispatch_done;
    }
    if (run_ctx->root_table == 0) {
        run_ctx->root_table = mm_context_root_table(proc->context_id);
    }
    if (!thread->is_kernel_worker) {
        process_validate_thread_context(proc, thread, run_ctx, "dispatch");
    }
    /* Every root table this kernel allocates comes from below 4 GiB, so a value
     * at or above that is corruption, not a legitimate high root.  Loading it
     * into CR3 would fault immediately, so fall back to the kernel root and
     * report instead. */
    if (run_ctx->root_table >= 0x100000000ULL) {
        serial_printf_unlocked(
            "[sched] CORRUPT root_table pid=%u name=%s root=%016llx rip=%016llx\n",
            (unsigned)proc->pid,
            proc->name ? proc->name : "?",
            (unsigned long long)run_ctx->root_table,
            (unsigned long long)run_ctx->rip);
        run_ctx->root_table = paging_get_root_table();
    }
    if (run_ctx->root_table == 0) {
        klog_write("[sched] target root missing\n");
        cpu_local()->in_scheduler = 1;
        critical_section_enter();
        cpu_local()->current_process = 0;
        cpu_local()->current_pid = 0;
        cpu_local()->current_thread = 0;
        thread_set_current(0);
        critical_section_leave();
        sched_rc = SCHED_R_ROOT;
        goto dispatch_done;
    }
    if (thread->is_kernel_worker) {
        /* Snapshot all callee-saved registers into g_sched_ctx so that when
         * the worker calls process_yield → context_switch_high(ctx, &cpu_local()->sched_ctx),
         * the scheduler resumes with correct register values and RSP pointing
         * to the return address of the upcoming call to process_run_worker_on_stack.
         * Only callee-saved regs + RSP need updating; RIP is always label-1
         * (set by the last context_switch_high(&cpu_local()->sched_ctx,...) call). */

        process_context_t* _sctx = &cpu_local()->sched_ctx;
        uintptr_t _rsp;
#ifdef WASMOS_PROCESS_TEST_SEAMS
        /* Nothing resumes this scheduler context on the host -- the seam above
         * calls the worker entry directly instead of switching stacks -- so the
         * snapshot has no consumer and the host's own registers must not be
         * written into a kernel context. */
        _rsp = 0;
#else
        __asm__ volatile("mov %%rsp, %[rsp]\n"
                         "mov %%r15, %[r15]\n"
                         "mov %%r14, %[r14]\n"
                         "mov %%r13, %[r13]\n"
                         "mov %%r12, %[r12]\n"
                         "mov %%rbp, %[rbp]\n"
                         "mov %%rbx, %[rbx]\n"
                         "pushfq; pop %[rf]"
                         : [rsp] "=r"(_rsp),
                           [r15] "=m"(_sctx->r15),
                           [r14] "=m"(_sctx->r14),
                           [r13] "=m"(_sctx->r13),
                           [r12] "=m"(_sctx->r12),
                           [rbp] "=m"(_sctx->rbp),
                           [rbx] "=m"(_sctx->rbx),
                           [rf] "=m"(_sctx->rflags)
                         :
                         : "memory");
#endif
        _sctx->rax = (uint64_t)PROCESS_RUN_BLOCKED;
        _sctx->rsp = _rsp - 8u;

        thread->ctx.rsp = 0;
        cpu_local()->last_run_result = process_run_worker_on_stack(proc, thread);
    } else {
#ifdef WASMOS_WASM_RUNTIME_WARP
        if (proc->runtime_tag[0] == 'W' && proc->runtime_tag[1] == 'A' &&
            proc->runtime_tag[2] == 'R' && proc->runtime_tag[3] == 'P' &&
            proc->runtime_tag[4] == '\0' && run_ctx->root_table != 0 &&
            run_ctx->root_table != paging_get_root_table()) {
            (void)warp_sync_linmem_for_pid(proc->pid, run_ctx->root_table);
        }
#endif
        context_switch_high(&cpu_local()->sched_ctx, run_ctx);
    }
    /* Diagnostics, but written from every CPU: plain ++ on a shared counter is a
     * data race whatever the generated code looks like, and it is what a
     * sanitizer arm over this path reports first. Relaxed is right -- nothing
     * orders anything against these, they only have to not tear. */
    (void)__atomic_fetch_add(&g_sched_switch_count, 1u, __ATOMIC_RELAXED);
    cpu_local()->dispatch_count++;
    /* Per-thread counterpart of the CPU's counter: it is what a diagnostic can
     * compare across two snapshots to tell a thread that is executing from one
     * left RUNNING that no longer runs (diag_dump_threads). */
    thread->dispatch_count++;
    if (__atomic_load_n(&g_sched_switch_count, __ATOMIC_RELAXED) >=
            SCHED_PROGRESS_MARKER_SWITCHES &&
        !__atomic_exchange_n(&g_sched_progress_logged, 1u, __ATOMIC_RELAXED)) {
        /* The exchange is what makes the marker print exactly once when several
         * CPUs cross the threshold together. */
        klog_write("[test] sched progress ok\n");
    }
    process_run_result_t result = cpu_local()->last_run_result;
    cpu_local()->in_scheduler = 1;
    critical_section_enter();
    cpu_local()->current_process = 0;
    cpu_local()->current_pid = 0;
    cpu_local()->current_thread = 0;
    thread_set_current(0);
    critical_section_leave();

    /* This CPU has stopped naming the thread, so an enqueue that cpu_sched_enqueue
     * refused while it was running here can now be performed.  Settling it here
     * rather than in the result handling below covers every way a dispatch can
     * end -- a thread that blocked again or exited owes nothing and is skipped
     * inside.  Before this existed the refusal left only a READY mark, which a
     * holder past its own check never acted on, and the thread stayed runnable
     * on no run queue for the rest of the boot. */
    sched_settle_deferred_enqueue(thread);

    if (proc->state == PROCESS_STATE_ZOMBIE || __atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE)) {
        /* A concurrent kill/exit can mark the owner zombie while this thread
         * is still returning from its timeslice. Never requeue it afterwards. */
        cpu_local()->last_index = proc->pid;
        cpu_local()->need_resched = 0;
        sched_rc = SCHED_R_ZOMBIE;
        goto dispatch_done;
    }

    if (result == PROCESS_RUN_EXITED) {
        uint8_t reap_detached = 0;
        uint32_t exited_tid = thread->tid;
        if (thread->is_kernel_worker && thread->tid != proc->main_tid) {
            thread_set_state(thread->tid, THREAD_STATE_ZOMBIE, THREAD_BLOCK_NONE);
            thread_set_exit_status(thread->tid, proc->exit_status);
            process_wake_thread_joiner(proc, thread);
            reap_detached = thread->detached;
            process_count_dec(&proc->live_thread_count);
            if (__atomic_load_n(&proc->live_thread_count, __ATOMIC_ACQUIRE) > 0) {
                thread_t* next = process_first_owner_ready_thread(proc);
                if (next && next->state != THREAD_STATE_ZOMBIE && process_set_ready(proc, next)) {
                    /* Duplicate enqueues are dropped by the on_rq claim. */
                    sched_enqueue_thread(next);
                }
            }
        } else {
            process_mark_exited(proc, proc->exit_status);
        }
        if (reap_detached) {
            /* Deferred to dispatch_done: exited_tid is the thread this dispatch
             * holds the claim on, so reaping it here refuses every time and the
             * slot stays behind while thread_count is decremented. */
            reap_tid = exited_tid;
            process_count_dec(&proc->thread_count);
        }
    } else if (result == PROCESS_RUN_THREAD_EXITED) {
        thread_t* next = 0;
        uint8_t reap_detached = 0;
        uint32_t exited_tid = thread->tid;
        thread_set_state(thread->tid, THREAD_STATE_ZOMBIE, THREAD_BLOCK_NONE);
        thread_set_exit_status(thread->tid, proc->exit_status);
        process_wake_thread_joiner(proc, thread);
        reap_detached = thread->detached;
        process_count_dec(&proc->live_thread_count);
        if (__atomic_load_n(&proc->live_thread_count, __ATOMIC_ACQUIRE) == 0) {
            process_mark_exited(proc, proc->exit_status);
        } else {
            next = process_first_owner_ready_thread(proc);
            if (next) {
                /* A refusal means the owner is already going away; leave the
                 * sibling where it is rather than enqueue it under a dying
                 * process, and let the exit path tear it down. */
                if (process_set_ready(proc, next)) {
                    sched_enqueue_thread(next);
                }
            } else {
                /* No runnable sibling left: park the process.  Best-effort —
                 * a 0 return means it raced to a terminal state, in which case
                 * there is nothing to block. */
                (void)process_force_transit(proc, PROCESS_STATE_BLOCKED);
            }
        }
        if (reap_detached) {
            /* Deferred to dispatch_done: exited_tid is the thread this dispatch
             * holds the claim on, so reaping it here refuses every time and the
             * slot stays behind while thread_count is decremented. */
            reap_tid = exited_tid;
            process_count_dec(&proc->thread_count);
        }
    } else if (result == PROCESS_RUN_BLOCKED) {
        /* The thread is already in BLOCKED state
         * (set by sched_event_wait → thread_set_state before yield).
         * The blocking_transition flag is cleared here once context is saved. */
        if (sched_block_complete_claim(thread) && thread->state == THREAD_STATE_BLOCKED) {
            /* Token claimed here: a waker deferred the enqueue to this path and
             * has not (or not yet) promoted the thread, so do it now. */
            thread_set_state(thread->tid, THREAD_STATE_READY, THREAD_BLOCK_NONE);
        }
        /* A concurrent sched_wake_thread reaching the thread first leaves it
         * already READY — re-enqueue it rather than leaving it stranded. */
        if (thread->state == THREAD_STATE_READY) {
            sched_enqueue_thread(thread);
        } else if (thread->state == THREAD_STATE_RUNNING) {
            /* Legacy callers (native services via the no-op process_block_on_ipc
             * stub) return PROCESS_RUN_BLOCKED without ever blocking through
             * sched_event_wait, so the thread is still RUNNING and on no
             * wait_list. Treat it as a voluntary yield: mark it READY and
             * re-enqueue. Without this it is orphaned — RUNNING, not current on
             * any CPU, not in any ready queue — and never runs again (observed
             * as a boot hang under SMP when such a service is scheduled off
             * CPU 0). Mark it sticky like other pollers so work-stealing leaves
             * it on its home CPU. */
            thread_set_state(thread->tid, THREAD_STATE_READY, THREAD_BLOCK_NONE);
            thread->sched_sticky = 1;
            sched_enqueue_thread(thread);
        }
    } else {
        /* If the thread registered itself in an event wait_list via the
         * non-blocking ipc_recv_for (returned IPC_EMPTY) but then YIELDED
         * rather than blocked, remove it from the wait_list now and clear
         * blocking_transition.  The message stays in the queue and will be
         * dequeued on the next ipc_recv_for call.
         * Guard the enqueue to prevent double-enqueue if the sender already
         * woke the thread via sched_event_wake_one. */
        if (thread->wait_event) {
            sched_event_t* _ev = thread->wait_event;
            ksync_spinlock_lock(&_ev->lock);
            if (!list_head_empty(&thread->event_node)) {
                list_head_del(&thread->event_node);
            }
            thread->wait_event = 0;
            ksync_spinlock_unlock(&_ev->lock);
            __atomic_store_n(&thread->blocking_transition, 0, __ATOMIC_SEQ_CST);
            /* This path promotes to READY and enqueues unconditionally below, so
             * the wake is satisfied here; consume the Dekker token to avoid a
             * stale one forcing a spurious wake on the next real block. */
            __atomic_store_n(&thread->wake_pending, 0, __ATOMIC_SEQ_CST);
        }
        thread_set_state(thread->tid, THREAD_STATE_READY, THREAD_BLOCK_NONE);
        /* This thread voluntarily yielded (PROCESS_RUN_YIELDED) — mark it sticky
         * so work-stealing leaves it on this CPU instead of having idle CPUs
         * thrash re-running a poll/yield loop. */
        if (result == PROCESS_RUN_YIELDED) {
            thread->sched_sticky = 1;
        }
        /* Idle threads live only in the per-CPU fallback path; never enqueue
         * them into the global ready queue so they cannot migrate to a wrong CPU. */
        if (!proc->is_idle) {
            sched_enqueue_thread(thread);
        }
    }

    cpu_local()->last_index = proc->pid;
    cpu_local()->need_resched = 0;
    sched_rc = (result == PROCESS_RUN_YIELDED) ? SCHED_OK : SCHED_R_RANDONE;

dispatch_done:
    __atomic_store_n(&thread->dispatch_ref, THREAD_SLOT_FREE, __ATOMIC_RELEASE);
    /* A runnable thread must leave this function reachable: linked in a ready
     * queue, or owed an enqueue by a CPU that will perform it.  READY with
     * neither is terminal -- nothing enqueues it again, and sched_sweep_owed_
     * enqueues cannot recover it because that sweep is gated on the global debt
     * counter, which a thread carrying no debt never appears in.
     *
     * The aborting exits above are the ones that can produce it: by the time any
     * of them is reachable the thread has already been unlinked (cpu_sched_pick_
     * next unlinks under the queue lock, cpu_sched_try_steal from the remote
     * queue), and two of them hand back only the state via
     * thread_transit(RUNNING, READY).  An exiting or ZOMBIE owner is EXCLUDED
     * rather than reported: leaving that thread unqueued is deliberate, the
     * reaper collects it, and re-enqueueing it would be re-picked and re-refused
     * at process_set_running on every scheduling attempt.
     *
     * EVERY exit is checked, not just the aborts, because the normal exits are not
     * self-evidently safe either: they mark the thread READY and call
     * sched_enqueue_thread, which SKIPS the insert for a bad priority band, for a
     * state that is not READY, or for an idle thread, and defers with a claim when
     * another CPU still names the thread.  A skip leaves exactly this state, and
     * gating the check on the abort codes would have looked past it.
     *
     * Diagnostic, not a repair: which exit strands a LIVE owner's thread is not
     * yet known, and the correct fix differs per exit, so this names the case
     * instead of guessing at it.  `rc` is that name. */
    /* The owner is re-resolved from thread->owner_pid rather than taken from
     * `proc`, and that is not defensive coding -- it is the difference between
     * seeing this case and not.  `proc` is the process the dispatch STARTED with,
     * and the SCHED_R_STALE exit means the slot was recycled mid-flight: `proc` is
     * then the old process, typically already ZOMBIE, while the thread belongs to
     * a live one.  Testing `proc` suppressed the report for exactly the exit that
     * needed it -- a capture carrying a persistent strand showed no report at all. */
    uint32_t stranded_owner_pid = __atomic_load_n(&thread->owner_pid, __ATOMIC_ACQUIRE);
    process_t* stranded_owner = stranded_owner_pid ? process_find_by_pid(stranded_owner_pid) : 0;
    if (stranded_owner && !stranded_owner->is_idle &&
        __atomic_load_n((uint32_t*)&thread->state, __ATOMIC_ACQUIRE) == THREAD_STATE_READY &&
        !__atomic_load_n(&thread->on_rq, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&thread->enqueue_owed, __ATOMIC_ACQUIRE) &&
        stranded_owner->state != PROCESS_STATE_ZOMBIE &&
        !__atomic_load_n(&stranded_owner->exiting, __ATOMIC_ACQUIRE)) {
        uint32_t sn = sched_debug_note(SCHED_DEBUG_DISPATCH_LEFT_STRANDED);
        if ((sn & (sn - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] dispatch left stranded tid=%u pid=%u rc=%d cpu=%u (n=%u)\n",
                (unsigned)thread->tid,
                (unsigned)stranded_owner_pid,
                (int)sched_rc,
                (unsigned)cpu_local()->cpu_id,
                (unsigned)(sn + 1u));
        }
        /* REPORT ONLY, deliberately.  This check once repaired the state here by
         * calling sched_enqueue_thread, and the measurement said not to: it fired
         * 28 times in a single clean boot, because a synchronous test at this point
         * cannot separate "stranded" from "in flight" -- the only difference is
         * elapsed time, and a waker that promotes then enqueues a statement later,
         * or a stealer that has unlinked but not yet claimed, both present exactly
         * this state.  Repairing them enqueued threads that needed nothing and
         * could leave a brief ghost entry for one another CPU was about to
         * dispatch.
         *
         * Recovery belongs where the state has settled, and NOT here.  What
         * remains is the tripwire, whose `rc` names the exit -- but read its
         * output knowing that sched_debug_note rate-limits to powers of two on a
         * GLOBAL per-event counter, so with tens of hits per boot only about six
         * print and the absence of a line for a given thread means nothing. */
    }
    /* Now that the claim is gone, a detached thread this dispatch retired can be
     * released. Its refusal path is not expected to trigger here -- nothing else
     * holds this slot -- so a refusal is reported rather than retried. */
    if (reap_tid != 0 && !thread_reap(reap_tid)) {
        uint32_t rn = sched_debug_note(SCHED_DEBUG_THREAD_REAP_REFUSED);
        if ((rn & (rn - 1u)) == 0u) {
            serial_printf_unlocked("[sched] detached reap refused tid=%u (n=%u)\n",
                                   (unsigned)reap_tid,
                                   (unsigned)(rn + 1u));
        }
    }
    /* Now that the slot is releasable, retry the reap this dispatch deferred.
     * Re-resolved by pid rather than reused as a pointer: the reference is gone,
     * so `proc` is no longer guaranteed to describe that process, and
     * process_find_by_pid answers 0 if it has already been reaped elsewhere. */
    if (reap_pid != 0) {
        process_t* done = process_find_by_pid(reap_pid);
        if (done) {
            /* An explicit reap this dispatch refused is retried unconditionally:
             * its requester already decided, and re-checking waiters here would
             * second-guess a decision made with more context. Otherwise fall back
             * to the ordinary auto-reap policy, which is a no-op unless the
             * process opted in. */
            if (__atomic_exchange_n(&done->reap_requested, 0u, __ATOMIC_ACQ_REL)) {
                process_reap_claim(done);
            } else {
                process_try_auto_reap(done);
            }
        }
    }
    return sched_rc;
}

/* One timer tick's worth of accounting on the CALLING CPU, called from the timer
 * IRQ handler.  Charges the tick to the running thread, decrements its quantum
 * and raises need_resched when it reaches 0; it does NOT switch — the actual
 * preemption happens later at process_preempt_from_irq on the IRQ return path.
 *
 * Does nothing when this CPU is inside the scheduler or has nothing dispatched,
 * and nothing when the running process is no longer RUNNING (a concurrent kill),
 * so a stale current_pid cannot be charged.
 *
 * The rest is the resched-stall watchdog: a resched that stays pending for
 * SCHED_RESCHED_STALL_TICKS while preemption is disabled is reported, and the
 * window restarts on every tick where no preemption-disabling lock is held, so
 * ordinary short spinlock holds inside ring-0 work never accumulate into a
 * report.  Reporting only: it never forces a switch. */
void process_tick(void) {
    uint64_t now = timer_ticks();
    if (cpu_local()->current_pid == 0 || !cpu_local()->current_thread ||
        cpu_local()->in_scheduler) {
        cpu_local()->resched_pending_since_tick = 0;
        return;
    }
    process_t* proc = process_find_by_pid(cpu_local()->current_pid);
    if (!proc || proc->state != PROCESS_STATE_RUNNING) {
        return;
    }
    cpu_local()->current_thread->ticks_total++;
    if (cpu_local()->current_thread->ticks_remaining > 0) {
        cpu_local()->current_thread->ticks_remaining--;
        if (cpu_local()->current_thread->ticks_remaining == 0) {
            cpu_local()->need_resched = 1;
            if (!g_preempt_smoke_logged) {
                g_preempt_smoke_logged = 1;
                klog_write("[test] preempt ok\n");
            }
        }
    }
    if (cpu_local()->need_resched) {
        /* Idle process cannot be preempted (ring-0 hlt loop) — stalls are expected. */
        if (proc && proc->is_idle) {
            cpu_local()->resched_pending_since_tick = 0;
            return;
        }
        /* Only advance the stall counter when a preemption-disabling lock is
         * actually held.  If no lock is held (preempt_is_enabled()), the
         * process is in ring-0 doing legitimate kernel work (e.g. initialising
         * a WARP module) and will return to ring-3 naturally.  Resetting on
         * every such tick ensures the 512-tick window starts fresh from the
         * last spinlock-free tick, so brief spinlock holds inside ring-0 kernel
         * work (IPC endpoint lock, PFA lock) do not accumulate into a spurious
         * watchdog fire. */
        if (preempt_is_enabled()) {
            cpu_local()->resched_pending_since_tick = 0;
        } else if (cpu_local()->resched_pending_since_tick == 0) {
            cpu_local()->resched_pending_since_tick = now;
        } else {
            uint64_t stall_ticks = now - cpu_local()->resched_pending_since_tick;
            if (stall_ticks >= SCHED_RESCHED_STALL_TICKS) {
                cpu_local()->resched_stall_reports++;
                klog_write("[watchdog] resched stall ticks=");
                serial_write_hex64(stall_ticks);
                klog_write("[watchdog] pid=");
                serial_write_hex64(cpu_local()->current_pid);
                klog_write("[watchdog] reports=");
                serial_write_hex64(cpu_local()->resched_stall_reports);
                klog_write("\n");
                cpu_local()->resched_pending_since_tick = now;
            }
        }
    } else {
        cpu_local()->resched_pending_since_tick = 0;
    }
}

/* Whether the CALLING CPU has a reschedule pending.  A hint, not a lock: it can
 * change under the reader, and acting on a stale answer only costs (or defers)
 * one preemption. */
int process_should_resched(void) {
    return cpu_local()->need_resched != 0;
}

/* Raises the reschedule request on the CALLING CPU.  Release-ordered so whatever
 * made a thread runnable is visible before the flag that will act on it; the
 * clearers are plain, since they run on the same CPU that consumes the flag. */
void process_set_need_resched(void) {
    __atomic_store_n(&cpu_local()->need_resched, 1, __ATOMIC_RELEASE);
}

/* Drops the CALLING CPU's pending reschedule and resets the stall watchdog's
 * window with it, so a resched that was declined does not later be reported as a
 * stall. */
void process_clear_resched(void) {
    cpu_local()->need_resched = 0;
    cpu_local()->resched_pending_since_tick = 0;
}

/* Decides whether to preempt the interrupted thread, from inside an IRQ handler.
 *
 * Returns 1 having REWRITTEN `frame` so the handler's iretq lands in
 * process_preempt_trampoline on the kernel stack instead of resuming the
 * interrupted code — the caller must iretq unchanged after that.  Returns 0 to
 * mean "resume normally"; `frame` is then untouched.  `frame` is borrowed and
 * points at the live saved-register frame on the interrupt stack.
 *
 * The gates that decline are, in order: already in the scheduler or a context
 * switch; the process-manager outside a pm_preempt_safe_enter region; no resched
 * pending or preemption disabled; the current process not RUNNING; inside a
 * hostcall; a trap frame that fails validation.
 *
 * Then the decisive one: an interrupt taken FROM RING 0 is never preempted.
 * Only ring-3 threads are preemptible here, which is why a wasm3 guest — which
 * executes inside the ring-0 interpreter — runs its timeslice to completion
 * regardless of need_resched, while a WARP guest in ring 3 is preempted.
 *
 * When it does preempt, the full ring-3 register state is snapshotted into the
 * thread's context first, and the whole privilege-return frame is rewritten
 * (CS/SS/RSP as well as RIP), because a bare RIP/CS rewrite would leave iretq
 * restoring the stale user SS:RSP. */
int process_preempt_from_irq(irq_frame_t* frame) {
    if (!frame) {
        return 0;
    }
    if (cpu_local()->in_scheduler) {
        return 0;
    }
    if (cpu_local()->in_context_switch) {
        return 0;
    }
    if (cpu_local()->current_process &&
        strcmp(cpu_local()->current_process->name, "process-manager") == 0 &&
        cpu_local()->pm_preempt_safe_depth == 0) {
        return 0;
    }
    if (!process_should_resched() || !preempt_is_enabled()) {
        return 0;
    }
    if (!cpu_local()->current_process ||
        cpu_local()->current_process->state != PROCESS_STATE_RUNNING) {
        process_clear_resched();
        return 0;
    }
    if (cpu_local()->current_process->in_hostcall) {
        return 0;
    }

    uint64_t cs = frame->cs;
    uint8_t from_user = (uint8_t)((cs & 0x3u) == 0x3u);
    uint8_t from_kernel = (uint8_t)((cs & 0x3u) == 0x0u);
    uint8_t valid = 1;

    if ((!from_user && !from_kernel) || frame->rip == 0) {
        valid = 0;
    } else if (from_kernel && cs != KERNEL_CS_SELECTOR) {
        valid = 0;
    } else if (from_user) {
        if ((frame->user_ss & 0x3u) != 0x3u || frame->user_rsp == 0) {
            valid = 0;
        }
    }
    if (!valid) {
        g_trap_frame_invalid_reports++;
        klog_write("[watchdog] trap frame invalid cs=");
        serial_write_hex64(frame->cs);
        klog_write("[watchdog] rip=");
        serial_write_hex64(frame->rip);
        klog_write("[watchdog] user_ss=");
        serial_write_hex64(frame->user_ss);
        klog_write("[watchdog] user_rsp=");
        serial_write_hex64(frame->user_rsp);
        klog_write("[watchdog] reports=");
        serial_write_hex64(g_trap_frame_invalid_reports);
        klog_write("\n");
        process_clear_resched();
        return 0;
    }
    if (from_kernel) {
        return 0;
    }

    process_context_t* ctx =
        process_sched_ctx_for_thread(cpu_local()->current_process, cpu_local()->current_thread);
    if (!ctx) {
        process_clear_resched();
        return 0;
    }
    ctx->rax = frame->rax;
    ctx->rbx = frame->rbx;
    ctx->rcx = frame->rcx;
    ctx->rdx = frame->rdx;
    ctx->rbp = frame->rbp;
    ctx->rsi = frame->rsi;
    ctx->rdi = frame->rdi;
    ctx->r8 = frame->r8;
    ctx->r9 = frame->r9;
    ctx->r10 = frame->r10;
    ctx->r11 = frame->r11;
    ctx->r12 = frame->r12;
    ctx->r13 = frame->r13;
    ctx->r14 = frame->r14;
    ctx->r15 = frame->r15;
    ctx->cs = frame->cs;
    ctx->user_rsp = frame->user_rsp;
    ctx->ss = frame->user_ss;
    ctx->rip = frame->rip;
    ctx->rflags = frame->rflags;
    ctx->root_table = paging_get_current_root_table();
    process_validate_thread_context(
        cpu_local()->current_process, cpu_local()->current_thread, ctx, "preempt");
    cpu_local()->current_process->ctx = *ctx;
    if (g_ctx_watch_ctx == addr_cast(uint64_t, ctx)) {
        g_ctx_watch_last_ctx = g_ctx_watch_ctx;
        g_ctx_watch_last_rip = ctx->rip;
        g_ctx_watch_last_rsp = ctx->rsp;
        g_ctx_watch_last_rflags = ctx->rflags;
        g_ctx_watch_reason = 2;
        g_ctx_watch_hits++;
        trace_write("[sched] ctxwatch preempt pid=");
        trace_do(serial_write_hex64(cpu_local()->current_process->pid));
        trace_write("[sched] ctxwatch preempt ctx=");
        trace_do(serial_write_hex64(g_ctx_watch_ctx));
        trace_write("[sched] ctxwatch preempt rip=");
        trace_do(serial_write_hex64(g_ctx_watch_last_rip));
        trace_write("[sched] ctxwatch preempt rsp=");
        trace_do(serial_write_hex64(g_ctx_watch_last_rsp));
        trace_write("[sched] ctxwatch preempt rflags=");
        trace_do(serial_write_hex64(g_ctx_watch_last_rflags));
    }

    thread_t* thread = process_thread_for_transition(cpu_local()->current_process);
    if (!thread) {
        process_clear_resched();
        return 0;
    }
    cpu_local()->last_run_result = PROCESS_RUN_YIELDED;
    process_clear_resched();
    /* Mirror process_yield(PROCESS_RUN_YIELDED): do not mark/enqueue the
     * current thread until control has actually switched back into the
     * scheduler context. Doing it here races against the "current_thread"
     * ownership checks and can strand or duplicate the running thread. */
    /* IRQ0 entered from ring 3 with a 5-slot IRET frame (RIP, CS, RFLAGS,
     * RSP, SS). Redirecting the return into a ring-0 scheduler trampoline must
     * rewrite the full privilege-return frame, not just RIP/CS, otherwise iretq
     * still tries to restore the stale user SS:RSP pair and faults. */
    frame->cs = KERNEL_CS_SELECTOR;
    frame->rip = (uint64_t)process_kernel_alias_addr((uintptr_t)process_preempt_trampoline);
    frame->user_ss = KERNEL_DS_SELECTOR;
    frame->user_rsp = (uint64_t)(cpu_local()->current_process->stack_top - 16u);
    return 1;
}

/* Per-CPU nesting counter for "do not preempt me".  Nestable, so every disable
 * must be matched by exactly one enable.
 *
 * Interrupts are NOT masked: this only suppresses the scheduler's decision to
 * switch away at the IRQ return path, so device IRQs keep being serviced while
 * it is raised.  That separation is what lets wasm_driver.c hold preemption off
 * across a whole WASM execution without blocking interrupt delivery.  Not a
 * lock, and not cross-CPU: another CPU can still run whatever it likes. */
void preempt_disable(void) {
    cpu_local()->preempt_disable_count++;
}

/* Releases one level.  Saturates at 0 instead of underflowing, so an unmatched
 * enable is absorbed silently rather than wrapping the counter to ~0 and pinning
 * preemption off forever.  Does NOT poll for a pending reschedule on reaching 0
 * — the switch waits for the next tick or an explicit preempt_safepoint(). */
void preempt_enable(void) {
    if (cpu_local()->preempt_disable_count > 0) {
        cpu_local()->preempt_disable_count--;
    }
}

/* Non-zero when the CALLING CPU is currently preemptible (depth 0). */
int preempt_is_enabled(void) {
    return cpu_local()->preempt_disable_count == 0;
}

/* Current nesting depth on the CALLING CPU; 0 means preemptible.  Used by the
 * trampoline to unwind a depth a process left raised, and by the tick watchdog
 * to tell a genuine lock hold from ordinary ring-0 work. */
uint32_t preempt_disable_depth(void) {
    return cpu_local()->preempt_disable_count;
}

/* Named aliases of the preempt counter for code that is guarding a short
 * multi-field update rather than a lock hold.  Identical mechanism; the separate
 * spelling is what documents the intent at the call site. */
void critical_section_enter(void) {
    preempt_disable();
}

void critical_section_leave(void) {
    preempt_enable();
}

/* Voluntary preemption point: yields ONLY if a reschedule is already pending,
 * and returns immediately otherwise, so it is cheap to sprinkle through a long
 * ring-0 loop.  Does not consult the preempt depth — a caller inside a
 * preempt-disabled region that calls this yields anyway, which is why it belongs
 * at loop tops rather than inside a critical section. */
void preempt_safepoint(void) {
    if (!cpu_local()->current_process) {
        return;
    }
    if (!process_should_resched()) {
        return;
    }
    process_clear_resched();
    process_yield(PROCESS_RUN_YIELDED);
}

/* Opt IN to preemption, for the process-manager only.  process_preempt_from_irq
 * refuses to preempt a process named "process-manager" unless this per-CPU depth
 * is non-zero, so PM runs non-preemptible by default and marks the regions where
 * being switched out is safe.  Nestable, and the leave saturates at 0 like
 * preempt_enable.  The depth is per-CPU, so it must be raised and dropped on the
 * same CPU — which holds because PM is not preempted while it is raised. */
void pm_preempt_safe_enter(void) {
    cpu_local()->pm_preempt_safe_depth++;
}

void pm_preempt_safe_leave(void) {
    if (cpu_local()->pm_preempt_safe_depth > 0) {
        cpu_local()->pm_preempt_safe_depth--;
    }
}

/* Total scheduler-health complaints, for the boot self-tests to assert on.  A
 * mixed figure by construction: resched stalls are the CALLING CPU's count,
 * invalid trap frames are a system-wide one, so the answer differs per CPU and
 * is only meaningful as "is it still zero?". */
uint64_t process_watchdog_issue_count(void) {
    return cpu_local()->resched_stall_reports + g_trap_frame_invalid_reports;
}

/* Number of enumerable process slots, i.e. the bound for a
 * process_info_at_stats() walk.  Lock-free and immediately stale; treat it as a
 * loop bound whose entries are re-validated, not as a live census. */
uint32_t process_count_active(void) {
    /* Counts everything process_info_at_stats() enumerates, so `ps` iterates a
     * matching range.  Includes ZOMBIE (shown as "zmb") to surface not-yet-
     * reaped children; skips only free and transient in-reap slots. */
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state != PROCESS_STATE_UNUSED &&
            g_processes[i].state != PROCESS_STATE_DEAD &&
            g_processes[i].state != PROCESS_STATE_REAPING) {
            count++;
        }
    }
    return count;
}

/* Threads queued ready on the CALLING CPU, summed across bands under that CPU's
 * queue lock.  Per-CPU, not system-wide: with work stealing the same thread can
 * move between queues, so this is a load reading for this CPU only.  Excludes
 * the running thread and the per-CPU idle thread, neither of which is queued. */
uint32_t process_ready_count(void) {
    /* Sum the per-band counters, which the enqueue/unlink paths actually
     * maintain.  There is no aggregate thread count to read instead: any field
     * the enqueue paths do not increment reports 0 forever, which the
     * sched_ready_count host call would surface as a real answer. */
    cpu_sched_t* cs = cpu_sched();
    uint32_t total = 0;
    ksync_spinlock_lock(&cs->lock);
    for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
        total += cs->thread_count[p];
    }
    ksync_spinlock_unlock(&cs->lock);
    return total;
}

/* Enumerates live processes by dense index: 0 with *out_pid and *out_name set, or -1
 * once `index` is past the end (the loop-termination signal).
 *
 * The index space is NOT the one process_count_active() sizes: this skips
 * ZOMBIE, that counts it.  Iterating to process_count_active() therefore ends in
 * one or more harmless -1s whenever an unreaped child exists; use
 * process_info_at_stats to enumerate the same set the count describes.
 *
 * *out_name borrows the slot's own storage: it stays valid only while that
 * process lives, and is "" (never NULL) for an unnamed process.  Lock-free, so a
 * walk that races a spawn or reap can skip or repeat an entry. */
int process_info_at(uint32_t index, uint32_t* out_pid, const char** out_name) {
    if (!out_pid || !out_name) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED ||
            g_processes[i].state == PROCESS_STATE_DEAD ||
            g_processes[i].state == PROCESS_STATE_ZOMBIE ||
            g_processes[i].state == PROCESS_STATE_REAPING) {
            continue;
        }
        if (current == index) {
            *out_pid = g_processes[i].pid;
            *out_name = g_processes[i].name ? g_processes[i].name : "";
            return 0;
        }
        current++;
    }
    return -1;
}

/* process_info_at plus the parent pid, over the same ZOMBIE-excluding index
 * space and with the same borrowed-name lifetime. */
int process_info_at_ex(uint32_t index, uint32_t* out_pid, uint32_t* out_parent_pid,
                       const char** out_name) {
    if (!out_pid || !out_parent_pid || !out_name) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED ||
            g_processes[i].state == PROCESS_STATE_DEAD ||
            g_processes[i].state == PROCESS_STATE_ZOMBIE ||
            g_processes[i].state == PROCESS_STATE_REAPING) {
            continue;
        }
        if (current == index) {
            *out_pid = g_processes[i].pid;
            *out_parent_pid = g_processes[i].parent_pid;
            *out_name = g_processes[i].name ? g_processes[i].name : "";
            return 0;
        }
        current++;
    }
    return -1;
}

static uint64_t process_sum_thread_ticks(const process_t* proc) {
    uint64_t total = 0;
    if (!proc || proc->pid == 0) {
        return 0;
    }
    for (uint32_t i = 0;; ++i) {
        uint32_t tid = 0;
        if (thread_owner_tid_at(proc->pid, i, &tid) != 0) {
            break;
        }
        thread_t* thread = thread_get(tid);
        if (!thread || thread->owner_pid != proc->pid) {
            continue;
        }
        total += thread->ticks_total;
    }
    return total;
}

static uint64_t process_context_mem_bytes(const process_t* proc) {
    if (!proc || proc->context_id == 0) {
        return 0;
    }
    mm_context_t* ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return 0;
    }
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t region = {0};
        if (mm_context_region_at(ctx, i, &region) != 0) {
            continue;
        }
        bytes += region.size;
    }
    return bytes;
}

static uint64_t process_thread_kstack_total_bytes(const process_t* proc) {
    uint64_t total = 0;
    if (!proc || proc->pid == 0) {
        return 0;
    }
    if (proc->stack_pages != 0) {
        total += (uint64_t)proc->stack_pages * PAGE_SIZE;
    }
    for (uint32_t i = 0;; ++i) {
        uint32_t tid = 0;
        thread_t* thread = 0;
        if (thread_owner_tid_at(proc->pid, i, &tid) != 0) {
            break;
        }
        thread = thread_get(tid);
        if (!thread || thread->owner_pid != proc->pid) {
            continue;
        }
        total += (uint64_t)thread->kstack_pages * PAGE_SIZE;
    }
    return total;
}

/* Full `ps` row for the `index`-th enumerable process: 0 with every out
 * parameter written, -1 past the end or for any NULL out pointer.
 *
 * This is the enumeration process_count_active() sizes — ZOMBIE included, only
 * free and in-reap slots skipped — so the two agree, unlike process_info_at.
 * *out_stats is filled wholesale; current_tid is 0 unless that process happens
 * to be running on the CALLING CPU at this instant, and rss_est_bytes is
 * currently the VM total rather than a resident measurement.  Every figure is a
 * lock-free snapshot assembled field by field, so the row can be internally
 * inconsistent if the process changes mid-read. */
int process_info_at_stats(uint32_t index, uint32_t* out_pid, uint32_t* out_parent_pid,
                          const char** out_name, process_stats_t* out_stats) {
    if (!out_pid || !out_parent_pid || !out_name || !out_stats) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_t* proc = &g_processes[i];
        /* Include ZOMBIE so `ps` shows not-yet-reaped children (state "zmb"),
         * like Linux's Z/defunct — makes leaked/unreaped slots visible.  Skip
         * only truly-free slots and the transient in-reap state. */
        if (proc->state == PROCESS_STATE_UNUSED || proc->state == PROCESS_STATE_DEAD ||
            proc->state == PROCESS_STATE_REAPING) {
            continue;
        }
        if (current == index) {
            *out_pid = proc->pid;
            *out_parent_pid = proc->parent_pid;
            *out_name = proc->name ? proc->name : "";
            out_stats->state = (uint32_t)proc->state;
            out_stats->block_reason = (uint32_t)proc->block_reason;
            for (uint32_t j = 0; j < WASMOS_APP_SUBSYSTEM_TAG_LEN; ++j) {
                out_stats->runtime_tag[j] = proc->runtime_tag[j];
            }
            out_stats->thread_count = __atomic_load_n(&proc->thread_count, __ATOMIC_ACQUIRE);
            out_stats->live_thread_count =
                __atomic_load_n(&proc->live_thread_count, __ATOMIC_ACQUIRE);
            out_stats->current_tid =
                (cpu_local()->current_process && cpu_local()->current_process->pid == proc->pid &&
                 cpu_local()->current_thread)
                    ? cpu_local()->current_thread->tid
                    : 0;
            out_stats->context_id = proc->context_id;
            out_stats->cpu_ticks = process_sum_thread_ticks(proc);
            out_stats->vm_total_bytes = process_context_mem_bytes(proc);
            out_stats->thread_kstack_total_bytes = process_thread_kstack_total_bytes(proc);
            out_stats->heap_committed_bytes = wasm3_heap_committed_bytes(proc->pid) +
                                              native_driver_heap_committed_bytes(proc->pid);
            /* TODO(memory-rss): Replace this estimate with real resident-page
             * accounting once per-context page presence tracking is available.
             */
            out_stats->rss_est_bytes = out_stats->vm_total_bytes;

            thread_t* mt = thread_get(proc->main_tid);
            out_stats->last_cpu = mt ? mt->last_cpu : 0;

            return 0;
        }
        current++;
    }
    return -1;
}

/* Declares whether this process's entry point must be serialised against its own
 * other threads.  Returns 0, or -1 for an unknown pid.  When set, the trampoline
 * takes the process's runtime_lock around each entry-point call — with the
 * no-IRQ spinlock variant, so the lock is held for a whole timeslice without
 * masking device interrupts.  Kernel worker threads bypass it entirely.
 * Any non-zero `required` enables it. */
int process_set_runtime_lock_required(uint32_t pid, uint8_t required) {
    process_t* proc = process_get(pid);
    if (!proc) {
        return -1;
    }
    proc->needs_runtime_lock = required ? 1u : 0u;
    return 0;
}

/* Records the runtime that owns this process ("KERNEL", "WARP", ...).  Returns 0
 * on success, -1 for an unknown pid, a NULL tag, or a tag longer than
 * WASMOS_APP_SUBSYSTEM_TAG_LEN — in which case the prefix has already been
 * stored, so the tag is left TRUNCATED rather than unchanged.  The dispatcher
 * compares this against "WARP" to decide whether a linear-memory resync is
 * needed before switching in, so an inaccurate tag is a correctness matter, not
 * just cosmetic. */
int process_set_runtime_tag(uint32_t pid, const char* tag) {
    process_t* proc = process_get(pid);
    if (!proc) {
        return -1;
    }
    return process_copy_runtime_tag(proc, tag);
}

/* Re-bands a process's MAIN thread before it is first scheduled.  Returns 0, or
 * -1 for a prio outside [0, SCHED_PRIO_MAX), an unknown pid, a process with no
 * main thread, or — see below — a main thread that is already queued.  Lower
 * numbers are higher priority.  Affects only the main thread; other threads of
 * the process keep the band sched_thread_init gave them. */
int process_set_main_prio(uint32_t pid, uint8_t prio) {
    if (prio >= SCHED_PRIO_MAX) {
        return -1;
    }
    process_t* proc = process_get(pid);
    if (!proc) {
        return -1;
    }
    thread_t* t = process_main_thread(proc);
    if (!t) {
        return -1;
    }
    /* Enforced, not merely documented: this is only meaningful before the child
     * is first scheduled.  The PM sets it on a freshly parked (blocked,
     * not-yet-enqueued) process, so the main thread is in no ready_list.
     *
     * Re-banding a QUEUED thread does not corrupt the run queue -- unlink
     * accounts against thread_t::rq_prio, the band the node actually joined, so
     * the old band's counter is drained correctly.  But the thread would keep
     * being DISPATCHED at its old priority until something happened to
     * re-enqueue it, which is a silent policy failure: the caller asked for a
     * priority change and got none, with nothing to say so.  Refuse and count it
     * instead of pretending it worked. */
    if (__atomic_load_n(&t->on_rq, __ATOMIC_ACQUIRE)) {
        uint32_t n = sched_debug_note(SCHED_DEBUG_SET_PRIO_QUEUED);
        if ((n & (n - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] set_main_prio on a queued thread tid=%u pid=%u prio=%u->%u (n=%u,"
                " refused)\n",
                (unsigned)t->tid,
                (unsigned)pid,
                (unsigned)t->sched_prio,
                (unsigned)prio,
                (unsigned)(n + 1u));
        }
        return -1;
    }
    t->sched_prio = prio;
    return 0;
}
static void process_sched_invariant_fail(const char* msg, uint64_t a, uint64_t b) {
    klog_write("[sched] invariant fail: ");
    klog_write(msg ? msg : "(unknown)");
    klog_write("\n[sched] a=");
    serial_write_hex64(a);
    klog_write("[sched] b=");
    serial_write_hex64(b);
    kpanic(msg, a, b);
}

static void process_set_blocked(process_t* proc, thread_t* thread, process_block_reason_t reason,
                                thread_block_reason_t thread_reason) {
    if (!proc || !thread) {
        process_sched_invariant_fail(
            "set_blocked null", addr_cast(uint64_t, proc), addr_cast(uint64_t, thread));
    }
    /* If the process raced to a terminal state, do not block its thread. */
    if (!process_force_transit(proc, PROCESS_STATE_BLOCKED)) {
        return;
    }
    proc->block_reason = reason;
    thread_set_state(thread->tid, THREAD_STATE_BLOCKED, thread_reason);
}

/* Returns 1 if `thread` is now READY and its caller must enqueue it, 0 if the
 * owner raced to a terminal state and it must NOT be enqueued.  Same convention
 * as process_set_running.
 *
 * A NULL argument is a caller bug and still panics.  An owner that is `exiting`
 * or ZOMBIE is not: no caller holds anything that excludes a concurrent
 * kill/exit, so a sibling-requeue racing the owner's teardown is reachable from
 * every call site and is the interleaving this function exists to absorb.  It is
 * counted and rate-limited rather than fatal — as a kpanic it turned a
 * survivable race into a dead machine.
 *
 * ZOMBIE is refused a second time by process_force_transit below, which has no
 * legal ZOMBIE->READY edge.  `exiting` has to be tested here because the state
 * can still be READY/RUNNING/BLOCKED while the exit is in flight. */
static int process_set_ready(process_t* proc, thread_t* thread) {
    if (!proc || !thread) {
        process_sched_invariant_fail(
            "set_ready null", addr_cast(uint64_t, proc), addr_cast(uint64_t, thread));
    }
    if (proc->state == PROCESS_STATE_ZOMBIE || __atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE)) {
        uint32_t n = sched_debug_note(SCHED_DEBUG_SET_READY_EXITING);
        if ((n & (n - 1u)) == 0u) {
            serial_printf_unlocked("[sched] set_ready on exiting owner pid=%u tid=%u state=%u"
                                   " exiting=%u (n=%u, refused)\n",
                                   (unsigned)proc->pid,
                                   (unsigned)thread->tid,
                                   (unsigned)proc->state,
                                   (unsigned)__atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE),
                                   (unsigned)(n + 1u));
        }
        return 0;
    }
    if (!process_force_transit(proc, PROCESS_STATE_READY)) {
        return 0;
    }
    proc->block_reason = PROCESS_BLOCK_NONE;
    /* Promote only a BLOCKED thread, and via thread_wake_if_blocked so that
     * block_reason is cleared with the state -- a READY thread still carrying the
     * reason it blocked for is put straight back to sleep by the wait paths.
     *
     * The result is deliberately IGNORED. This function's return value answers
     * "may this owner's thread be made runnable", NOT "did this call change the
     * thread's state", and conflating them drops wakes: a thread already READY
     * (the requeue case) or RUNNING on another CPU (woken between the caller's
     * test and here) still obliges the caller to complete the wake/block
     * handshake. sched_wake_claim_enqueue is precisely what covers a target that
     * is executing right now -- it hands the enqueue to that thread's completion
     * path -- so reporting 0 for those suppressed the handshake and lost the wake.
     *
     * Restricting the promotion to a BLOCKED thread is what protects a live
     * dispatch. THREAD_STATE_RUNNING *is* the exclusive dispatch claim --
     * cpu_sched_claim_for_dispatch is a READY->RUNNING CAS -- so writing READY
     * over it re-arms that claim, and a second CPU then wins it on a thread that
     * is already executing: two CPUs resuming one context on one kernel stack.
     * Nothing else guards that window. Between the claim and the publication of
     * cpu_local()->current_thread the RUNNING state is the only record anywhere
     * that the thread is spoken for, so sched_enqueue_thread's holder scan cannot
     * see it and only its "state != READY, skip" test keeps an executing thread
     * out of a ready queue. Pinned by "the dispatch claim survived the promotion"
     * in tests/unit/test_process_lifecycle.c. */
    (void)thread_wake_if_blocked(thread->tid);
    return 1;
}

/* Returns 1 if proc is now RUNNING, 0 if it raced to a terminal state and must
 * NOT be dispatched. */
static int process_set_running(process_t* proc, thread_t* thread) {
    if (!proc || !thread) {
        process_sched_invariant_fail(
            "set_running null", addr_cast(uint64_t, proc), addr_cast(uint64_t, thread));
    }
    /* Same race, same treatment as process_set_ready: the 0 return below already
     * means "raced to a terminal state, do not dispatch", and every caller
     * honours it, so this case is counted rather than fatal. */
    if (proc->state == PROCESS_STATE_ZOMBIE || __atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE)) {
        uint32_t n = sched_debug_note(SCHED_DEBUG_SET_RUNNING_EXITING);
        if ((n & (n - 1u)) == 0u) {
            serial_printf_unlocked("[sched] set_running on exiting owner pid=%u tid=%u state=%u"
                                   " exiting=%u (n=%u, refused)\n",
                                   (unsigned)proc->pid,
                                   (unsigned)thread->tid,
                                   (unsigned)proc->state,
                                   (unsigned)__atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE),
                                   (unsigned)(n + 1u));
        }
        return 0;
    }
    if (!process_force_transit(proc, PROCESS_STATE_RUNNING)) {
        return 0;
    }
    thread_set_state(thread->tid, THREAD_STATE_RUNNING, THREAD_BLOCK_NONE);
    return 1;
}

#ifdef WASMOS_PROCESS_TEST_SEAMS
/* Host-test entries; see process.h.  Forwarding only. */
int process_test_set_ready(process_t* proc, thread_t* thread) {
    return process_set_ready(proc, thread);
}

int process_test_set_running(process_t* proc, thread_t* thread) {
    return process_set_running(proc, thread);
}
#endif

static void process_wake_thread_joiner(process_t* owner, thread_t* exited) {
    thread_t* waiter = 0;
    uint32_t waiter_tid = 0;
    if (!owner || !exited) {
        return;
    }
    waiter_tid = exited->join_waiter_tid;
    if (waiter_tid == 0) {
        return;
    }
    exited->join_waiter_tid = 0;
    waiter = thread_get(waiter_tid);
    if (!waiter) {
        return;
    }
    if (waiter->owner_pid != owner->pid) {
        return;
    }
    if (waiter->state != THREAD_STATE_BLOCKED) {
        return;
    }
    if (process_set_ready(owner, waiter) && sched_wake_claim_enqueue(waiter)) {
        sched_enqueue_thread(waiter);
    }
}
