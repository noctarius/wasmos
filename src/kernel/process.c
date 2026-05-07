#include "process.h"
#include "memory.h"
#include "physmem.h"
#include "serial.h"
#include "paging.h"
#include "cpu.h"
#include "wasm3_shim.h"
#include "ipc.h"
#include "timer.h"
#include "thread.h"

/*
 * process.c contains the single-core scheduler, process table, run queue, and
 * context-switch glue. The implementation is intentionally simple: fixed-size
 * arrays, a FIFO ready queue, and explicit state transitions that are easy to
 * audit while the kernel is still ring-0-only.
 */

static process_t g_processes[PROCESS_MAX_COUNT];
static uint32_t g_next_pid;
static uint32_t g_last_index;
static uint32_t g_current_pid;
static volatile uint8_t g_need_resched;

static process_t *process_find_by_pid(uint32_t pid);
static void process_trampoline(void);
static uint32_t g_ready_queue[THREAD_MAX_COUNT];
static uint32_t g_ready_head;
static uint32_t g_ready_tail;
static uint32_t g_ready_count;
static process_t *g_current_process;
static thread_t *g_current_thread;

static process_run_result_t g_last_run_result;
process_context_t g_sched_ctx;
static uint32_t g_preempt_disable_count;
static process_t *g_idle_process;
volatile uint8_t g_in_context_switch;
volatile uint8_t g_in_scheduler;
static uint64_t g_ctx_watch_logged;
static uint64_t g_ctx_watch_last_logged_rip;
static uint64_t g_ctx_watch_last_logged_rsp;
static uint64_t g_ctx_watch_last_logged_rflags;
static uint64_t g_ctx_watch_last_logged_reason;
static uint8_t g_preempt_smoke_logged;
static uint8_t g_sched_progress_logged;
static uint64_t g_sched_switch_count;
static uint64_t g_resched_pending_since_tick;
static uint64_t g_resched_stall_reports;
static uint64_t g_trap_frame_invalid_reports;

static inline uintptr_t
process_kernel_alias_addr(uintptr_t addr)
{
    uint64_t base = KERNEL_HIGHER_HALF_BASE;
    if ((uint64_t)addr < base) {
        return (uintptr_t)((uint64_t)addr + base);
    }
    return addr;
}

static inline process_t *
process_table(void)
{
    return (process_t *)(void *)process_kernel_alias_addr((uintptr_t)&g_processes[0]);
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
volatile uint64_t *g_pm_stack_watch;
static uint32_t g_pm_preempt_safe_depth;

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

#define PAGE_SIZE 0x1000u
#define KERNEL_CS_SELECTOR 0x08u
#define KERNEL_DS_SELECTOR 0x10u
#define USER_CS_SELECTOR   0x1Bu
#define USER_DS_SELECTOR   0x23u
#define STACK_GUARD_PAGES 1u
#define STACK_REDZONE_BYTES 4096u
#define STACK_CANARY_VALUE 0xC0DEC0DEF00DFACEULL
#define SCHED_TRAMPOLINE_STACK_BYTES 8192u
#define SCHED_PROGRESS_MARKER_SWITCHES 256ull
#define SCHED_RESCHED_STALL_TICKS 512ull
/* Phase-2 stack hardening currently relies on the shared higher-half kernel
 * window (64 MiB by default: 32 * 2 MiB PDEs). */
#define KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES (64u * 1024u * 1024u)
static uint8_t g_sched_trampoline_stack[SCHED_TRAMPOLINE_STACK_BYTES] __attribute__((aligned(16)));

static int
process_alloc_stack(process_t *slot, uint32_t stack_pages)
{
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
        serial_write("[sched] higher-half stack alloc failed\n");
        return -1;
    }

    uint64_t guard_low = base;
    uint64_t usable_base = base + ((uint64_t)STACK_GUARD_PAGES * PAGE_SIZE);
    uint64_t guard_high = base + ((total_pages - STACK_GUARD_PAGES) * PAGE_SIZE);
    uint64_t higher_half_base = paging_get_higher_half_base();
    uint64_t guard_low_virt = using_higher_half_stack ? (higher_half_base + guard_low) : guard_low;
    uint64_t usable_base_virt = using_higher_half_stack ? (higher_half_base + usable_base) : usable_base;
    uint64_t guard_high_virt = using_higher_half_stack ? (higher_half_base + guard_high) : guard_high;

    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_low_virt + ((uint64_t)i * PAGE_SIZE)) != 0) {
            serial_write("[sched] guard unmap failed\n");
            return -1;
        }
    }
    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_high_virt + ((uint64_t)i * PAGE_SIZE)) != 0) {
            serial_write("[sched] guard unmap failed\n");
            return -1;
        }
    }

    slot->stack_base = (uintptr_t)usable_base_virt;
    slot->stack_pages = stack_pages;
    slot->stack_top = (uintptr_t)guard_high_virt;
    slot->stack_alloc_base_phys = (uintptr_t)base;

    if (slot->stack_base && slot->stack_top > slot->stack_base + sizeof(uint64_t)) {
        /* Canaries catch in-range stack corruption that does not reach the guard
         * pages, such as smashed frames near the bottom or top of the stack. */
        uint64_t *base_canary = (uint64_t *)(uintptr_t)slot->stack_base;
        uint64_t *top_canary = (uint64_t *)(uintptr_t)(slot->stack_top - sizeof(uint64_t));
        uintptr_t mid_addr = slot->stack_base + (slot->stack_top - slot->stack_base) / 2u;
        uint64_t *mid_canary = (uint64_t *)(uintptr_t)(mid_addr & ~(uintptr_t)0x7u);
        *base_canary = STACK_CANARY_VALUE;
        *top_canary = STACK_CANARY_VALUE;
        *mid_canary = STACK_CANARY_VALUE;
    }
    return 0;
}

extern void context_switch(process_context_t *out, process_context_t *in);
extern void context_switch_to(process_context_t *in);
static void context_switch_high(process_context_t *out, process_context_t *in);
static int process_schedule_once_impl(void);
static thread_t *process_main_thread(process_t *proc);
static process_t *process_owner_for_thread(thread_t *thread);
static thread_t *process_thread_for_transition(process_t *proc);
static void process_sched_invariant_fail(const char *msg, uint64_t a, uint64_t b);
static void process_set_blocked(process_t *proc, thread_t *thread, process_block_reason_t reason, thread_block_reason_t thread_reason);
static void process_set_ready(process_t *proc, thread_t *thread);
static void process_set_running(process_t *proc, thread_t *thread);


static int process_str_eq(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    a = (const char *)(uintptr_t)process_kernel_alias_addr((uintptr_t)a);
    b = (const char *)(uintptr_t)process_kernel_alias_addr((uintptr_t)b);
    while (*a || *b) {
        if (*a != *b) {
            return 0;
        }
        if (*a == '\0') {
            break;
        }
        ++a;
        ++b;
    }
    return 1;
}

static void
context_switch_high(process_context_t *out, process_context_t *in)
{
    uint64_t higher_half_base = paging_get_higher_half_base();
    uintptr_t fn = (uintptr_t)&context_switch;
    if ((uint64_t)fn < higher_half_base) {
        fn += (uintptr_t)higher_half_base;
    }
    ((void (*)(process_context_t *, process_context_t *))fn)(out, in);
}

static int
process_run_on_sched_stack(int (*fn)(void))
{
    if (!fn) {
        return -1;
    }
    uintptr_t stack_top = process_kernel_alias_addr(
        (uintptr_t)&g_sched_trampoline_stack[SCHED_TRAMPOLINE_STACK_BYTES]);
    stack_top &= ~(uintptr_t)0xFULL;
    int rc = -1;
    __asm__ volatile(
        "mov %%rsp, %%r15\n"
        "mov %[stack_top], %%rsp\n"
        "call *%[fn]\n"
        "mov %%r15, %%rsp\n"
        : "=a"(rc)
        : [stack_top] "r"(stack_top), [fn] "r"(fn)
        : "r15", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "memory", "cc");
    return rc;
}

static void process_log_ctx_watch(const char *where) {
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

static void process_validate_context(process_t *proc, const char *where) {
    if (!proc) {
        return;
    }
    if (proc->ctx_canary_pre != PROCESS_CTX_CANARY_VALUE ||
        proc->ctx_canary_post != PROCESS_CTX_CANARY_VALUE) {
        serial_printf(
            "[sched] ctx canary corrupt pid=%016llx\n"
            "[sched] name=%s\n"
            "[sched] ctx canary pre=%016llx\n"
            "[sched] ctx canary post=%016llx\n",
            (unsigned long long)proc->pid,
            proc->name ? proc->name : "(null)",
            (unsigned long long)proc->ctx_canary_pre,
            (unsigned long long)proc->ctx_canary_post);
        process_log_ctxsw_state();
        process_log_ctx_watch("canary");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    uint64_t rip = proc->ctx.rip;
    uint8_t is_user_ctx = (uint8_t)((proc->ctx.cs & 0x3u) == 0x3u);
    uint64_t start = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t end = (uint64_t)(uintptr_t)&__kernel_end;
    uint64_t low_start = start;
    uint64_t low_end = end;
    uint64_t higher_half = paging_get_higher_half_base();
    if (start < higher_half) {
        start += higher_half;
    }
    if (end < higher_half) {
        end += higher_half;
    }
    if (is_user_ctx || (rip >= start && rip < end) ||
        (rip >= low_start && rip < low_end)) {
        return;
    }
    serial_printf(
        "[sched] invalid rip in %s pid=%016llx\n"
        "[sched] name=%s\n"
        "[sched] rip=%016llx\n"
        "[sched] rsp=%016llx\n",
        where ? where : "?",
        (unsigned long long)proc->pid,
        proc->name ? proc->name : "(null)",
        (unsigned long long)rip,
        (unsigned long long)proc->ctx.rsp);
    process_log_ctxsw_state();
    process_log_ctx_watch("invalid-rip");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void ready_queue_reset(void) {
    g_ready_head = 0;
    g_ready_tail = 0;
    g_ready_count = 0;
}

static thread_t *
process_main_thread(process_t *proc)
{
    if (!proc || proc->main_tid == 0) {
        return 0;
    }
    return thread_get(proc->main_tid);
}

static process_t *
process_owner_for_thread(thread_t *thread)
{
    if (!thread || thread->owner_pid == 0) {
        return 0;
    }
    return process_find_by_pid(thread->owner_pid);
}

static thread_t *
process_thread_for_transition(process_t *proc)
{
    if (!proc) {
        return 0;
    }
    if (g_current_process == proc && g_current_thread) {
        return g_current_thread;
    }
    return process_main_thread(proc);
}

static int ready_queue_enqueue(thread_t *thread) {
    if (!thread) {
        return 0;
    }
    if (thread->state != THREAD_STATE_READY) {
        process_sched_invariant_fail("enqueue non-ready thread", thread->tid, thread->state);
    }
    process_t *proc = process_owner_for_thread(thread);
    if (!proc || proc->in_ready_queue) {
        return 0;
    }
    /* The idle task is scheduled as a fallback only and never participates in
     * the normal ready queue rotation. */
    if (proc->is_idle) {
        return 0;
    }
    if (g_ready_count >= THREAD_MAX_COUNT) {
        return -1;
    }
    g_ready_queue[g_ready_tail] = thread->tid;
    g_ready_tail = (g_ready_tail + 1u) % THREAD_MAX_COUNT;
    g_ready_count++;
    proc->in_ready_queue = 1;
    return 0;
}

static thread_t *ready_queue_dequeue(void) {
    while (g_ready_count > 0) {
        uint32_t tid = g_ready_queue[g_ready_head];
        g_ready_head = (g_ready_head + 1u) % THREAD_MAX_COUNT;
        g_ready_count--;
        thread_t *thread = thread_get(tid);
        process_t *proc = process_owner_for_thread(thread);
        if (!thread || !proc) {
            process_sched_invariant_fail("dequeue owner missing", tid, (uint64_t)(uintptr_t)proc);
            continue;
        }
        proc->in_ready_queue = 0;
        if (thread->state == THREAD_STATE_READY &&
            proc->state == PROCESS_STATE_READY) {
            return thread;
        }
    }
    return 0;
}

static void process_trampoline(void) {
    for (;;) {
        g_in_scheduler = 0;
        if (g_current_process) {
            uint64_t *base = (uint64_t *)(uintptr_t)g_current_process->stack_base;
            uint64_t *top = (uint64_t *)(uintptr_t)(g_current_process->stack_top - sizeof(uint64_t));
            uintptr_t mid_addr = g_current_process->stack_base
                                 + (g_current_process->stack_top - g_current_process->stack_base) / 2u;
            uint64_t *mid = (uint64_t *)(uintptr_t)(mid_addr & ~(uintptr_t)0x7u);
            if (base && top && mid) {
                const uint64_t canary = STACK_CANARY_VALUE;
                if (*base != canary || *top != canary || *mid != canary) {
                    serial_printf_unlocked(
                        "[sched] stack canary tripped for %s\n"
                        "[sched] base=%016llx\n"
                        "[sched] mid=%016llx\n"
                        "[sched] top=%016llx\n"
                        "[sched] base val=%016llx\n"
                        "[sched] mid val=%016llx\n"
                        "[sched] top val=%016llx\n",
                        g_current_process->name ? g_current_process->name : "(unknown)",
                        (unsigned long long)(uintptr_t)base,
                        (unsigned long long)(uintptr_t)mid,
                        (unsigned long long)(uintptr_t)top,
                        (unsigned long long)*base,
                        (unsigned long long)*mid,
                        (unsigned long long)*top);
                    for (;;) {
                        __asm__ volatile("cli; hlt");
                    }
                }
            }
        }
        while (preempt_disable_depth() > 0) {
            preempt_enable();
        }
        if (!g_current_process || !g_current_process->entry) {
            g_last_run_result = PROCESS_RUN_IDLE;
        } else {
            uintptr_t entry_ptr = process_kernel_alias_addr((uintptr_t)g_current_process->entry);
            process_entry_t entry_fn = (process_entry_t)(void *)entry_ptr;
            g_last_run_result = entry_fn(g_current_process, g_current_process->arg);
        }
        critical_section_enter();
        g_in_scheduler = 1;
        context_switch_high(&g_current_process->ctx, &g_sched_ctx);
        if (g_ctx_watch_hits != g_ctx_watch_logged) {
            g_ctx_watch_logged = g_ctx_watch_hits;
            process_log_ctx_watch_if_changed();
        }
    }
}

extern void process_preempt_trampoline(void);

static void process_reset_slot(process_t *proc) {
    if (!proc) {
        return;
    }
    proc->pid = 0;
    proc->parent_pid = 0;
    proc->context_id = 0;
    proc->main_tid = 0;
    proc->thread_count = 0;
    proc->live_thread_count = 0;
    proc->exiting = 0;
    proc->state = PROCESS_STATE_UNUSED;
    proc->block_reason = PROCESS_BLOCK_NONE;
    proc->wait_target_pid = 0;
    proc->exit_status = 0;
    proc->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    proc->ticks_remaining = 0;
    proc->ticks_total = 0;
    proc->in_ready_queue = 0;
    proc->is_idle = 0;
    proc->in_hostcall = 0;
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

static int
process_copy_name(process_t *proc, const char *name)
{
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

static process_t *process_find_slot(void) {
    process_t *table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (table[i].state == PROCESS_STATE_UNUSED) {
            return &table[i];
        }
    }
    return 0;
}

static process_t *process_find_by_pid(uint32_t pid) {
    if (pid == 0) {
        return 0;
    }
    process_t *table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (table[i].pid == pid && table[i].state != PROCESS_STATE_UNUSED) {
            return &table[i];
        }
    }
    return 0;
}

static process_t *process_find_by_context_internal(uint32_t context_id) {
    if (context_id == 0) {
        return 0;
    }
    process_t *table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (table[i].context_id == context_id &&
            table[i].state != PROCESS_STATE_UNUSED) {
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
        process_t *proc = &g_processes[i];
        if (proc->state != PROCESS_STATE_BLOCKED) {
            continue;
        }
        if (proc->block_reason != PROCESS_BLOCK_WAIT) {
            continue;
        }
        if (proc->wait_target_pid != target_pid) {
            continue;
        }
        proc->block_reason = PROCESS_BLOCK_NONE;
        proc->wait_target_pid = 0;
        proc->state = PROCESS_STATE_READY;
        ready_queue_enqueue(process_main_thread(proc));
    }
}

static void process_mark_exited(process_t *proc, int32_t exit_status) {
    if (!proc || proc->state == PROCESS_STATE_UNUSED) {
        return;
    }
    proc->exit_status = exit_status;
    proc->exiting = 1;
    proc->block_reason = PROCESS_BLOCK_NONE;
    proc->wait_target_pid = 0;
    /* TODO: Add safe automatic reaping for exited kernel-owned children that
     * are never waited on. Today they remain zombies until an explicit wait or
     * a subsystem-specific reap path (for example the process manager) handles
     * them. */
    proc->state = PROCESS_STATE_ZOMBIE;
    if (proc->main_tid != 0) {
        thread_set_exit_status(proc->main_tid, exit_status);
        thread_set_state(proc->main_tid, THREAD_STATE_ZOMBIE, THREAD_BLOCK_NONE);
    }
    proc->live_thread_count = 0;
    process_wake_waiters(proc->pid);
}

static void process_reap(process_t *proc) {
    if (!proc) {
        return;
    }
    if (proc->stack_alloc_base_phys && proc->stack_pages) {
        uint64_t total_pages = (uint64_t)proc->stack_pages + (STACK_GUARD_PAGES * 2u);
        pfa_free_pages((uint64_t)proc->stack_alloc_base_phys, total_pages);
    }
    if (proc->context_id != 0) {
        ipc_endpoints_release_owner(proc->context_id);
        (void)mm_context_destroy(proc->context_id);
    }
    if (proc->pid != 0) {
        wasm3_heap_release(proc->pid);
    }
    if (proc->main_tid != 0) {
        thread_reap(proc->main_tid);
    }
    process_reset_slot(proc);
}

void process_init(void) {
    g_next_pid = 1;
    g_last_index = 0;
    g_current_pid = 0;
    g_need_resched = 0;
    ready_queue_reset();
    g_current_process = 0;
    g_current_thread = 0;
    g_last_run_result = PROCESS_RUN_IDLE;
    g_preempt_disable_count = 0;
    g_idle_process = 0;
    g_in_scheduler = 1;
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
    g_sched_progress_logged = 0;
    g_sched_switch_count = 0;
    g_resched_pending_since_tick = 0;
    g_resched_stall_reports = 0;
    g_trap_frame_invalid_reports = 0;
    g_ctx_restore_ctx = 0;
    g_ctx_restore_rip = 0;
    g_ctx_restore_rsp = 0;
    g_ctx_restore_rflags = 0;
    g_pm_stack_watch = 0;
    g_pm_preempt_safe_depth = 0;
    thread_init();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_reset_slot(&g_processes[i]);
    }
    g_sched_ctx.root_table = paging_get_root_table();
}

int process_spawn(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid) {
    return process_spawn_as(g_current_pid, name, entry, arg, out_pid);
}

int process_spawn_as(uint32_t parent_pid, const char *name, process_entry_t entry, void *arg, uint32_t *out_pid) {
    if (!entry || !out_pid) {
        return -1;
    }

    process_t *slot = process_find_slot();
    if (!slot) {
        return -1;
    }

    uint32_t pid = g_next_pid++;
    mm_context_t *ctx = mm_context_create(pid);
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
    slot->thread_count = 0;
    slot->live_thread_count = 0;
    slot->exiting = 0;
    slot->state = PROCESS_STATE_READY;
    slot->block_reason = PROCESS_BLOCK_NONE;
    slot->wait_target_pid = 0;
    slot->exit_status = 0;
    slot->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    slot->ticks_remaining = slot->time_slice_ticks;
    slot->ticks_total = 0;
    slot->ctx_canary_pre = PROCESS_CTX_CANARY_VALUE;
    slot->ctx_canary_post = PROCESS_CTX_CANARY_VALUE;
    slot->entry = entry;
    slot->arg = arg;
    if (process_copy_name(slot, name ? name : "") != 0) {
        return -1;
    }
    if (thread_spawn_main(pid, name ? name : "", &slot->main_tid) != 0) {
        return -1;
    }
    {
        thread_t *main_thread = process_main_thread(slot);
        if (!main_thread) {
            return -1;
        }
        main_thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
        main_thread->ticks_remaining = main_thread->time_slice_ticks;
        main_thread->ticks_total = 0;
    }
    slot->thread_count = 1;
    slot->live_thread_count = 1;
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
    if (process_str_eq(name, "process-manager")) {
        g_ctx_watch_ctx = (uint64_t)(uintptr_t)&slot->ctx;
        g_ctx_watch_last_ctx = g_ctx_watch_ctx;
        g_ctx_watch_hits = 0;
        g_ctx_watch_reason = 0;
        trace_write("[sched] ctxwatch armed ctx=");
        trace_do(serial_write_hex64(g_ctx_watch_ctx));
        if (slot->stack_top >= sizeof(uint64_t)) {
            g_pm_stack_watch = (uint64_t *)(uintptr_t)(slot->stack_top - sizeof(uint64_t));
            trace_write("[sched] pm stack watch addr=");
            trace_do(serial_write_hex64((uint64_t)(uintptr_t)g_pm_stack_watch));
        }
    }
    if (process_str_eq(name, "preempt-busy")) {
        trace_write("[sched] spawn preempt-busy rip=");
        trace_do(serial_write_hex64(slot->ctx.rip));
        trace_write("[sched] spawn preempt-busy rsp=");
        trace_do(serial_write_hex64(slot->ctx.rsp));
        trace_write("[sched] spawn preempt-busy stack base=");
        trace_do(serial_write_hex64(slot->stack_base));
        trace_write("[sched] spawn preempt-busy stack top=");
        trace_do(serial_write_hex64(slot->stack_top));
    }
    ready_queue_enqueue(process_main_thread(slot));
    *out_pid = pid;
    return 0;
}

int process_spawn_idle(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid) {
    if (!entry || !out_pid) {
        return -1;
    }
    if (g_idle_process) {
        return -1;
    }

    process_t *slot = process_find_slot();
    if (!slot) {
        return -1;
    }

    uint32_t pid = g_next_pid++;
    mm_context_t *ctx = mm_context_create(pid);
    if (!ctx) {
        return -1;
    }

    slot->pid = pid;
    slot->parent_pid = 0;
    slot->context_id = ctx->id;
    slot->main_tid = 0;
    slot->thread_count = 0;
    slot->live_thread_count = 0;
    slot->exiting = 0;
    slot->state = PROCESS_STATE_READY;
    slot->block_reason = PROCESS_BLOCK_NONE;
    slot->wait_target_pid = 0;
    slot->exit_status = 0;
    slot->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
    slot->ticks_remaining = slot->time_slice_ticks;
    slot->ticks_total = 0;
    slot->ctx_canary_pre = PROCESS_CTX_CANARY_VALUE;
    slot->ctx_canary_post = PROCESS_CTX_CANARY_VALUE;
    slot->entry = entry;
    slot->arg = arg;
    if (process_copy_name(slot, name ? name : "") != 0) {
        return -1;
    }
    if (thread_spawn_main(pid, name ? name : "", &slot->main_tid) != 0) {
        return -1;
    }
    {
        thread_t *main_thread = process_main_thread(slot);
        if (!main_thread) {
            return -1;
        }
        main_thread->time_slice_ticks = PROCESS_DEFAULT_SLICE_TICKS;
        main_thread->ticks_remaining = main_thread->time_slice_ticks;
        main_thread->ticks_total = 0;
    }
    slot->thread_count = 1;
    slot->live_thread_count = 1;
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
    g_idle_process = slot;
    *out_pid = pid;
    return 0;
}

int
process_set_user_entry(uint32_t pid, uint64_t rip, uint64_t user_rsp)
{
    /* TODO: Wire this into process-manager launch policy once the first
     * user-mode service/app path is selected and validated end-to-end. */
    process_t *proc = process_find_by_pid(pid);
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
    return 0;
}

process_t *process_get(uint32_t pid) {
    return process_find_by_pid(pid);
}

process_t *process_find_by_context(uint32_t context_id) {
    return process_find_by_context_internal(context_id);
}

uint32_t process_current_pid(void) {
    uint32_t *pid_ptr = (uint32_t *)(void *)process_kernel_alias_addr((uintptr_t)&g_current_pid);
    return *pid_ptr;
}

void process_set_exit_status(process_t *process, int32_t exit_status) {
    if (!process) {
        return;
    }
    process->exit_status = exit_status;
}

void process_yield(process_run_result_t result) {
    if (!g_current_process) {
        return;
    }
    g_last_run_result = result;
    context_switch_high(&g_current_process->ctx, &g_sched_ctx);
}

void process_block_on_ipc(process_t *process) {
    if (!process) {
        return;
    }
    thread_t *thread = process_thread_for_transition(process);
    process_set_blocked(process, thread, PROCESS_BLOCK_IPC, THREAD_BLOCK_IPC);
    process->wait_target_pid = 0;
}

int process_wait(process_t *process, uint32_t target_pid, int32_t *out_exit_status) {
    if (!process || target_pid == 0 || process->pid == target_pid) {
        return -1;
    }

    process_t *target = process_find_by_pid(target_pid);
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
        process_reap(target);
        process->block_reason = PROCESS_BLOCK_NONE;
        process->wait_target_pid = 0;
        return 0;
    }

    thread_t *thread = process_thread_for_transition(process);
    process_set_blocked(process, thread, PROCESS_BLOCK_WAIT, THREAD_BLOCK_WAIT_PROCESS);
    process->wait_target_pid = target_pid;
    return 1;
}

int process_kill(uint32_t pid, int32_t exit_status) {
    process_t *target = process_find_by_pid(pid);
    if (!target) {
        return -1;
    }
    if (pid == g_current_pid) {
        return -1;
    }
    if (g_current_pid != 0 && target->parent_pid != g_current_pid) {
        return -1;
    }
    if (target->state == PROCESS_STATE_ZOMBIE) {
        return 0;
    }
    process_mark_exited(target, exit_status);
    return 0;
}

int process_get_exit_status(uint32_t pid, int32_t *out_exit_status) {
    process_t *proc = process_find_by_pid(pid);
    if (!proc || !out_exit_status) {
        return -1;
    }
    if (proc->state != PROCESS_STATE_ZOMBIE) {
        return 1;
    }
    *out_exit_status = proc->exit_status;
    return 0;
}

uint32_t process_wake_by_context(uint32_t context_id) {
    if (context_id == 0) {
        return 0;
    }

    uint32_t woken = 0;
    process_t *table = process_table();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_t *proc = &table[i];
        if (proc->context_id != context_id) {
            continue;
        }
        if (proc->block_reason != PROCESS_BLOCK_IPC) {
            continue;
        }
        if (proc->state != PROCESS_STATE_BLOCKED &&
            !(proc->state == PROCESS_STATE_RUNNING && proc == g_current_process)) {
            continue;
        }
        thread_t *thread = process_thread_for_transition(proc);
        process_set_ready(proc, thread);
        ready_queue_enqueue(thread);
        woken++;
    }
    return woken;
}

int
process_wake_thread(uint32_t tid)
{
    if (tid == 0) {
        return 0;
    }
    thread_t *thread = thread_get(tid);
    process_t *proc = process_owner_for_thread(thread);
    if (!thread || !proc) {
        return 0;
    }
    if (thread->state != THREAD_STATE_BLOCKED) {
        return 0;
    }
    if (proc->state != PROCESS_STATE_BLOCKED &&
        !(proc->state == PROCESS_STATE_RUNNING && proc == g_current_process)) {
        return 0;
    }
    process_set_ready(proc, thread);
    ready_queue_enqueue(thread);
    return 1;
}

int process_schedule_once(void) {
    uint64_t higher_half_base = paging_get_higher_half_base();
    uintptr_t here = 0;
    uintptr_t rsp_cur = 0;
    __asm__ volatile("leaq 0f(%%rip), %0\n0:" : "=r"(here));
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_cur));
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
    if (PROCESS_MAX_COUNT == 0) {
        return 1;
    }

    thread_t *thread = ready_queue_dequeue();
    process_t *proc = process_owner_for_thread(thread);
    if (!thread || !proc || proc->state != PROCESS_STATE_READY || !proc->entry) {
        if (g_idle_process && g_idle_process->state == PROCESS_STATE_READY) {
            proc = g_idle_process;
            thread = process_main_thread(proc);
        } else {
            return 1;
        }
    }

    if (proc->ctx_canary_pre != PROCESS_CTX_CANARY_VALUE ||
        proc->ctx_canary_post != PROCESS_CTX_CANARY_VALUE) {
        serial_write("[sched] ctx canary corrupt before restore pid=");
        serial_write_hex64(proc->pid);
        serial_write("[sched] name=");
        serial_write(proc->name ? proc->name : "(null)");
        serial_write("\n[sched] ctx canary pre=");
        serial_write_hex64(proc->ctx_canary_pre);
        serial_write("[sched] ctx canary post=");
        serial_write_hex64(proc->ctx_canary_post);
        process_log_ctxsw_state();
        process_log_ctx_watch("pre-restore-canary");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    process_set_running(proc, thread);
    if (thread->ticks_remaining == 0) {
        thread->ticks_remaining = thread->time_slice_ticks;
    }
    if (thread->time_slice_ticks == 0) {
        process_sched_invariant_fail("zero time slice", thread->tid, 0);
    }
    process_validate_context(proc, "schedule");
    critical_section_enter();
    g_current_pid = proc->pid;
    g_current_process = proc;
    g_current_thread = thread;
    if (g_current_thread->owner_pid != g_current_process->pid) {
        process_sched_invariant_fail("current owner mismatch", g_current_thread->owner_pid, g_current_process->pid);
    }
    thread_set_current(thread ? thread->tid : 0);
    critical_section_leave();
    /* Ring3 transitions use TSS.rsp0 as the kernel entry stack. Keep it aligned
     * to the scheduled process stack so user-mode interrupts/syscalls have a
     * deterministic kernel stack landing point. */
    cpu_set_kernel_stack((uint64_t)(proc->stack_top - 16u));
    g_sched_ctx.root_table = paging_get_root_table();
    proc->ctx.root_table = mm_context_root_table(proc->context_id);
    if (proc->ctx.root_table == 0) {
        serial_write("[sched] target root missing\n");
        critical_section_enter();
        g_current_process = 0;
        g_current_pid = 0;
        g_current_thread = 0;
        thread_set_current(0);
        critical_section_leave();
        return 1;
    }
    context_switch_high(&g_sched_ctx, &proc->ctx);
    g_sched_switch_count++;
    if (!g_sched_progress_logged && g_sched_switch_count >= SCHED_PROGRESS_MARKER_SWITCHES) {
        g_sched_progress_logged = 1;
        serial_write("[test] sched progress ok\n");
    }
    process_run_result_t result = g_last_run_result;
    critical_section_enter();
    g_current_process = 0;
    g_current_pid = 0;
    g_current_thread = 0;
    thread_set_current(0);
    critical_section_leave();

    if (result == PROCESS_RUN_EXITED) {
        process_mark_exited(proc, proc->exit_status);
    } else if (result == PROCESS_RUN_BLOCKED) {
        if (proc->state == PROCESS_STATE_READY) {
            proc->block_reason = PROCESS_BLOCK_NONE;
        } else {
            if (proc->block_reason == PROCESS_BLOCK_NONE) {
                proc->block_reason = PROCESS_BLOCK_IPC;
            }
            process_set_blocked(proc, thread, proc->block_reason, THREAD_BLOCK_IPC);
        }
        if (proc->state == PROCESS_STATE_READY) {
            thread_set_state(thread->tid, THREAD_STATE_READY, THREAD_BLOCK_NONE);
        }
    } else {
        process_set_ready(proc, thread);
        proc->wait_target_pid = 0;
        ready_queue_enqueue(process_main_thread(proc));
    }

    g_last_index = proc->pid;
    g_need_resched = 0;
    return (result == PROCESS_RUN_YIELDED) ? 0 : 1;
}

void process_tick(void) {
    uint64_t now = timer_ticks();
    if (g_current_pid == 0 || !g_current_thread) {
        g_resched_pending_since_tick = 0;
        return;
    }
    process_t *proc = process_find_by_pid(g_current_pid);
    if (!proc || proc->state != PROCESS_STATE_RUNNING) {
        return;
    }
    g_current_thread->ticks_total++;
    if (g_current_thread->ticks_remaining > 0) {
        g_current_thread->ticks_remaining--;
        if (g_current_thread->ticks_remaining == 0) {
            g_need_resched = 1;
            if (!g_preempt_smoke_logged) {
                g_preempt_smoke_logged = 1;
                serial_write("[test] preempt ok\n");
            }
        }
    }
    if (g_need_resched) {
        if (g_resched_pending_since_tick == 0) {
            g_resched_pending_since_tick = now;
        } else if ((now - g_resched_pending_since_tick) >= SCHED_RESCHED_STALL_TICKS) {
            g_resched_stall_reports++;
            serial_write("[watchdog] resched stall ticks=");
            serial_write_hex64(now - g_resched_pending_since_tick);
            serial_write("[watchdog] pid=");
            serial_write_hex64(g_current_pid);
            serial_write("[watchdog] reports=");
            serial_write_hex64(g_resched_stall_reports);
            serial_write("\n");
            g_resched_pending_since_tick = now;
        }
    } else {
        g_resched_pending_since_tick = 0;
    }
}

int process_should_resched(void) {
    return g_need_resched != 0;
}

void process_clear_resched(void) {
    g_need_resched = 0;
    g_resched_pending_since_tick = 0;
}

int process_preempt_from_irq(irq_frame_t *frame) {
    if (!frame) {
        return 0;
    }
    if (g_in_scheduler) {
        return 0;
    }
    if (g_in_context_switch) {
        return 0;
    }
    if (g_current_process && process_str_eq(g_current_process->name, "process-manager") &&
        g_pm_preempt_safe_depth == 0) {
        return 0;
    }
    if (!process_should_resched() || !preempt_is_enabled()) {
        return 0;
    }
    if (!g_current_process || g_current_process->state != PROCESS_STATE_RUNNING) {
        process_clear_resched();
        return 0;
    }
    if (g_current_process->in_hostcall) {
        return 0;
    }
    {
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
            serial_write("[watchdog] trap frame invalid cs=");
            serial_write_hex64(frame->cs);
            serial_write("[watchdog] rip=");
            serial_write_hex64(frame->rip);
            serial_write("[watchdog] user_ss=");
            serial_write_hex64(frame->user_ss);
            serial_write("[watchdog] user_rsp=");
            serial_write_hex64(frame->user_rsp);
            serial_write("[watchdog] reports=");
            serial_write_hex64(g_trap_frame_invalid_reports);
            serial_write("\n");
            process_clear_resched();
            return 0;
        }
    }

    process_validate_context(g_current_process, "preempt");
    g_current_process->ctx.rax = frame->rax;
    g_current_process->ctx.rbx = frame->rbx;
    g_current_process->ctx.rcx = frame->rcx;
    g_current_process->ctx.rdx = frame->rdx;
    g_current_process->ctx.rbp = frame->rbp;
    g_current_process->ctx.rsi = frame->rsi;
    g_current_process->ctx.rdi = frame->rdi;
    g_current_process->ctx.r8 = frame->r8;
    g_current_process->ctx.r9 = frame->r9;
    g_current_process->ctx.r10 = frame->r10;
    g_current_process->ctx.r11 = frame->r11;
    g_current_process->ctx.r12 = frame->r12;
    g_current_process->ctx.r13 = frame->r13;
    g_current_process->ctx.r14 = frame->r14;
    g_current_process->ctx.r15 = frame->r15;
    g_current_process->ctx.cs = frame->cs;
    if ((frame->cs & 0x3u) == 0x3u) {
        g_current_process->ctx.user_rsp = frame->user_rsp;
        g_current_process->ctx.ss = frame->user_ss;
    } else {
        g_current_process->ctx.rsp = (uint64_t)((uintptr_t)frame + sizeof(irq_frame_t));
        g_current_process->ctx.user_rsp = g_current_process->ctx.rsp;
        g_current_process->ctx.ss = KERNEL_DS_SELECTOR;
    }
    g_current_process->ctx.rip = frame->rip;
    g_current_process->ctx.rflags = frame->rflags;
    if (g_ctx_watch_ctx == (uint64_t)(uintptr_t)&g_current_process->ctx) {
        g_ctx_watch_last_ctx = g_ctx_watch_ctx;
        g_ctx_watch_last_rip = g_current_process->ctx.rip;
        g_ctx_watch_last_rsp = g_current_process->ctx.rsp;
        g_ctx_watch_last_rflags = g_current_process->ctx.rflags;
        g_ctx_watch_reason = 2;
        g_ctx_watch_hits++;
        trace_write("[sched] ctxwatch preempt pid=");
        trace_do(serial_write_hex64(g_current_process->pid));
        trace_write("[sched] ctxwatch preempt ctx=");
        trace_do(serial_write_hex64(g_ctx_watch_ctx));
        trace_write("[sched] ctxwatch preempt rip=");
        trace_do(serial_write_hex64(g_ctx_watch_last_rip));
        trace_write("[sched] ctxwatch preempt rsp=");
        trace_do(serial_write_hex64(g_ctx_watch_last_rsp));
        trace_write("[sched] ctxwatch preempt rflags=");
        trace_do(serial_write_hex64(g_ctx_watch_last_rflags));
    }

    thread_t *thread = process_thread_for_transition(g_current_process);
    process_set_ready(g_current_process, thread);
    ready_queue_enqueue(thread);
    g_last_run_result = PROCESS_RUN_YIELDED;
    process_clear_resched();
    if ((frame->cs & 0x3u) == 0x3u) {
        frame->cs = KERNEL_CS_SELECTOR;
    }
    frame->rip = (uint64_t)(uintptr_t)process_preempt_trampoline;
    return 1;
}

void preempt_disable(void) {
    g_preempt_disable_count++;
}

void preempt_enable(void) {
    if (g_preempt_disable_count > 0) {
        g_preempt_disable_count--;
    }
}

int preempt_is_enabled(void) {
    return g_preempt_disable_count == 0;
}

uint32_t preempt_disable_depth(void) {
    return g_preempt_disable_count;
}

void critical_section_enter(void) {
    preempt_disable();
}

void critical_section_leave(void) {
    preempt_enable();
}

void preempt_safepoint(void) {
    if (!g_current_process) {
        return;
    }
    if (!process_should_resched()) {
        return;
    }
    process_clear_resched();
    process_yield(PROCESS_RUN_YIELDED);
}

void pm_preempt_safe_enter(void) {
    g_pm_preempt_safe_depth++;
}

void pm_preempt_safe_leave(void) {
    if (g_pm_preempt_safe_depth > 0) {
        g_pm_preempt_safe_depth--;
    }
}

uint64_t process_watchdog_issue_count(void) {
    return g_resched_stall_reports + g_trap_frame_invalid_reports;
}

uint32_t process_count_active(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state != PROCESS_STATE_UNUSED &&
            g_processes[i].state != PROCESS_STATE_ZOMBIE) {
            count++;
        }
    }
    return count;
}

uint32_t process_ready_count(void) {
    return g_ready_count;
}

int process_info_at(uint32_t index, uint32_t *out_pid, const char **out_name) {
    if (!out_pid || !out_name) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED ||
            g_processes[i].state == PROCESS_STATE_ZOMBIE) {
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

int process_info_at_ex(uint32_t index, uint32_t *out_pid, uint32_t *out_parent_pid, const char **out_name) {
    if (!out_pid || !out_parent_pid || !out_name) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED ||
            g_processes[i].state == PROCESS_STATE_ZOMBIE) {
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
static void
process_sched_invariant_fail(const char *msg, uint64_t a, uint64_t b)
{
    serial_write("[sched] invariant fail: ");
    serial_write(msg ? msg : "(unknown)");
    serial_write("\n[sched] a=");
    serial_write_hex64(a);
    serial_write("[sched] b=");
    serial_write_hex64(b);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void
process_set_blocked(process_t *proc,
                    thread_t *thread,
                    process_block_reason_t reason,
                    thread_block_reason_t thread_reason)
{
    if (!proc || !thread) {
        process_sched_invariant_fail("set_blocked null", (uint64_t)(uintptr_t)proc, (uint64_t)(uintptr_t)thread);
    }
    proc->state = PROCESS_STATE_BLOCKED;
    proc->block_reason = reason;
    thread_set_state(thread->tid, THREAD_STATE_BLOCKED, thread_reason);
}

static void
process_set_ready(process_t *proc, thread_t *thread)
{
    if (!proc || !thread) {
        process_sched_invariant_fail("set_ready null", (uint64_t)(uintptr_t)proc, (uint64_t)(uintptr_t)thread);
    }
    proc->state = PROCESS_STATE_READY;
    proc->block_reason = PROCESS_BLOCK_NONE;
    thread_set_state(thread->tid, THREAD_STATE_READY, THREAD_BLOCK_NONE);
}

static void
process_set_running(process_t *proc, thread_t *thread)
{
    if (!proc || !thread) {
        process_sched_invariant_fail("set_running null", (uint64_t)(uintptr_t)proc, (uint64_t)(uintptr_t)thread);
    }
    proc->state = PROCESS_STATE_RUNNING;
    thread_set_state(thread->tid, THREAD_STATE_RUNNING, THREAD_BLOCK_NONE);
}
