#include "process.h"
#include "memory.h"
#include "physmem.h"
#include "serial.h"
#include "paging.h"
#include "wasm3_shim.h"
#include "ipc.h"

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
static uint32_t g_ready_queue[PROCESS_MAX_COUNT];
static uint32_t g_ready_head;
static uint32_t g_ready_tail;
static uint32_t g_ready_count;
static process_t *g_current_process;

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
#define STACK_GUARD_PAGES 1u
#define STACK_REDZONE_BYTES 4096u
#define STACK_CANARY_VALUE 0xC0DEC0DEF00DFACEULL

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
    uint64_t base = pfa_alloc_pages(total_pages);
    if (!base) {
        return -1;
    }

    uint64_t guard_low = base;
    uint64_t usable_base = base + ((uint64_t)STACK_GUARD_PAGES * PAGE_SIZE);
    uint64_t guard_high = base + ((total_pages - STACK_GUARD_PAGES) * PAGE_SIZE);

    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_low + ((uint64_t)i * PAGE_SIZE)) != 0) {
            serial_write("[sched] guard unmap failed\n");
            return -1;
        }
    }
    for (uint32_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        if (paging_unmap_4k(guard_high + ((uint64_t)i * PAGE_SIZE)) != 0) {
            serial_write("[sched] guard unmap failed\n");
            return -1;
        }
    }

    slot->stack_base = (uintptr_t)usable_base;
    slot->stack_pages = stack_pages;
    slot->stack_top = (uintptr_t)guard_high;

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

static void process_log_hex64(uint64_t value) {
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static int process_str_eq(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
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

static void process_log_ctx_watch(const char *where) {
    trace_write("[sched] ctxwatch ");
    if (where) {
        trace_write(where);
    }
    trace_write(" ctx=");
    trace_do(process_log_hex64(g_ctx_watch_last_ctx));
    trace_write("[sched] ctxwatch hits=");
    trace_do(process_log_hex64(g_ctx_watch_hits));
    trace_write("[sched] ctxwatch reason=");
    trace_do(process_log_hex64(g_ctx_watch_reason));
    trace_write("[sched] ctxwatch rip=");
    trace_do(process_log_hex64(g_ctx_watch_last_rip));
    trace_write("[sched] ctxwatch rsp=");
    trace_do(process_log_hex64(g_ctx_watch_last_rsp));
    trace_write("[sched] ctxwatch rflags=");
    trace_do(process_log_hex64(g_ctx_watch_last_rflags));
}

static void process_log_ctxsw_state(void) {
    trace_write("[sched] ctxsw out ctx=");
    trace_do(process_log_hex64(g_ctxsw_last_out_ctx));
    trace_write("[sched] ctxsw out rip=");
    trace_do(process_log_hex64(g_ctxsw_last_out_rip));
    trace_write("[sched] ctxsw out rsp=");
    trace_do(process_log_hex64(g_ctxsw_last_out_rsp));
    trace_write("[sched] ctxsw out rflags=");
    trace_do(process_log_hex64(g_ctxsw_last_out_rflags));
    trace_write("[sched] ctxsw in ctx=");
    trace_do(process_log_hex64(g_ctxsw_last_in_ctx));
    trace_write("[sched] ctxsw in rip=");
    trace_do(process_log_hex64(g_ctxsw_last_in_rip));
    trace_write("[sched] ctxsw in rsp=");
    trace_do(process_log_hex64(g_ctxsw_last_in_rsp));
    trace_write("[sched] ctxsw in rflags=");
    trace_do(process_log_hex64(g_ctxsw_last_in_rflags));
    trace_write("[sched] ctxsw restore ctx=");
    trace_do(process_log_hex64(g_ctx_restore_ctx));
    trace_write("[sched] ctxsw restore rip=");
    trace_do(process_log_hex64(g_ctx_restore_rip));
    trace_write("[sched] ctxsw restore rsp=");
    trace_do(process_log_hex64(g_ctx_restore_rsp));
    trace_write("[sched] ctxsw restore rflags=");
    trace_do(process_log_hex64(g_ctx_restore_rflags));
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
        serial_write("[sched] ctx canary corrupt pid=");
        process_log_hex64(proc->pid);
        serial_write("[sched] name=");
        serial_write(proc->name ? proc->name : "(null)");
        serial_write("\n[sched] ctx canary pre=");
        process_log_hex64(proc->ctx_canary_pre);
        serial_write("[sched] ctx canary post=");
        process_log_hex64(proc->ctx_canary_post);
        process_log_ctxsw_state();
        process_log_ctx_watch("canary");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    uint64_t rip = proc->ctx.rip;
    uint64_t start = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t end = (uint64_t)(uintptr_t)&__kernel_end;
    if (rip >= start && rip < end) {
        return;
    }
    serial_write("[sched] invalid rip in ");
    if (where) {
        serial_write(where);
    }
    serial_write(" pid=");
    process_log_hex64(proc->pid);
    serial_write("[sched] name=");
    serial_write(proc->name ? proc->name : "(null)");
    serial_write("\n[sched] rip=");
    process_log_hex64(rip);
    serial_write("[sched] rsp=");
    process_log_hex64(proc->ctx.rsp);
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

static int ready_queue_enqueue(process_t *proc) {
    if (!proc || proc->in_ready_queue) {
        return 0;
    }
    /* The idle task is scheduled as a fallback only and never participates in
     * the normal ready queue rotation. */
    if (proc->is_idle) {
        return 0;
    }
    if (g_ready_count >= PROCESS_MAX_COUNT) {
        return -1;
    }
    g_ready_queue[g_ready_tail] = proc->pid;
    g_ready_tail = (g_ready_tail + 1u) % PROCESS_MAX_COUNT;
    g_ready_count++;
    proc->in_ready_queue = 1;
    return 0;
}

static process_t *ready_queue_dequeue(void) {
    while (g_ready_count > 0) {
        uint32_t pid = g_ready_queue[g_ready_head];
        g_ready_head = (g_ready_head + 1u) % PROCESS_MAX_COUNT;
        g_ready_count--;
        process_t *proc = process_find_by_pid(pid);
        if (!proc) {
            continue;
        }
        proc->in_ready_queue = 0;
        if (proc->state == PROCESS_STATE_READY) {
            return proc;
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
                    serial_write_unlocked("[sched] stack canary tripped for ");
                    if (g_current_process->name) {
                        serial_write_unlocked(g_current_process->name);
                    } else {
                        serial_write_unlocked("(unknown)");
                    }
                    serial_write_unlocked("\n");
                    serial_write_unlocked("[sched] base=");
                    process_log_hex64((uint64_t)(uintptr_t)base);
                    serial_write_unlocked("[sched] mid=");
                    process_log_hex64((uint64_t)(uintptr_t)mid);
                    serial_write_unlocked("[sched] top=");
                    process_log_hex64((uint64_t)(uintptr_t)top);
                    serial_write_unlocked("[sched] base val=");
                    process_log_hex64(*base);
                    serial_write_unlocked("[sched] mid val=");
                    process_log_hex64(*mid);
                    serial_write_unlocked("[sched] top val=");
                    process_log_hex64(*top);
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
            g_last_run_result = g_current_process->entry(g_current_process, g_current_process->arg);
        }
        critical_section_enter();
        g_in_scheduler = 1;
        context_switch(&g_current_process->ctx, &g_sched_ctx);
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
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED) {
            return &g_processes[i];
        }
    }
    return 0;
}

static process_t *process_find_by_pid(uint32_t pid) {
    if (pid == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].pid == pid && g_processes[i].state != PROCESS_STATE_UNUSED) {
            return &g_processes[i];
        }
    }
    return 0;
}

static process_t *process_find_by_context_internal(uint32_t context_id) {
    if (context_id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].context_id == context_id &&
            g_processes[i].state != PROCESS_STATE_UNUSED) {
            return &g_processes[i];
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
        ready_queue_enqueue(proc);
    }
}

static void process_mark_exited(process_t *proc, int32_t exit_status) {
    if (!proc || proc->state == PROCESS_STATE_UNUSED) {
        return;
    }
    proc->exit_status = exit_status;
    proc->block_reason = PROCESS_BLOCK_NONE;
    proc->wait_target_pid = 0;
    /* TODO: Add safe automatic reaping for exited kernel-owned children that
     * are never waited on. Today they remain zombies until an explicit wait or
     * a subsystem-specific reap path (for example the process manager) handles
     * them. */
    proc->state = PROCESS_STATE_ZOMBIE;
    process_wake_waiters(proc->pid);
}

static void process_reap(process_t *proc) {
    if (!proc) {
        return;
    }
    if (proc->stack_base && proc->stack_pages) {
        uint64_t total_pages = (uint64_t)proc->stack_pages + (STACK_GUARD_PAGES * 2u);
        uint64_t stack_alloc_base = (uint64_t)proc->stack_base - ((uint64_t)STACK_GUARD_PAGES * PAGE_SIZE);
        pfa_free_pages(stack_alloc_base, total_pages);
    }
    if (proc->context_id != 0) {
        ipc_endpoints_release_owner(proc->context_id);
        (void)mm_context_destroy(proc->context_id);
    }
    if (proc->pid != 0) {
        wasm3_heap_release(proc->pid);
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
    g_ctx_restore_ctx = 0;
    g_ctx_restore_rip = 0;
    g_ctx_restore_rsp = 0;
    g_ctx_restore_rflags = 0;
    g_pm_stack_watch = 0;
    g_pm_preempt_safe_depth = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_reset_slot(&g_processes[i]);
    }
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

    slot->pid = pid;
    slot->parent_pid = parent_pid;
    slot->context_id = ctx->id;
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
    uint32_t stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_stack(slot, stack_pages) != 0) {
        return -1;
    }
    slot->ctx.rsp = slot->stack_top - (STACK_REDZONE_BYTES + 8u);
    slot->ctx.rip = (uint64_t)(uintptr_t)process_trampoline;
    slot->ctx.rflags = 0x200;
    if (process_str_eq(name, "process-manager")) {
        g_ctx_watch_ctx = (uint64_t)(uintptr_t)&slot->ctx;
        g_ctx_watch_last_ctx = g_ctx_watch_ctx;
        g_ctx_watch_hits = 0;
        g_ctx_watch_reason = 0;
        trace_write("[sched] ctxwatch armed ctx=");
        trace_do(process_log_hex64(g_ctx_watch_ctx));
        if (slot->stack_top >= sizeof(uint64_t)) {
            g_pm_stack_watch = (uint64_t *)(uintptr_t)(slot->stack_top - sizeof(uint64_t));
            trace_write("[sched] pm stack watch addr=");
            trace_do(process_log_hex64((uint64_t)(uintptr_t)g_pm_stack_watch));
        }
    }
    if (process_str_eq(name, "preempt-busy")) {
        trace_write("[sched] spawn preempt-busy rip=");
        trace_do(process_log_hex64(slot->ctx.rip));
        trace_write("[sched] spawn preempt-busy rsp=");
        trace_do(process_log_hex64(slot->ctx.rsp));
        trace_write("[sched] spawn preempt-busy stack base=");
        trace_do(process_log_hex64(slot->stack_base));
        trace_write("[sched] spawn preempt-busy stack top=");
        trace_do(process_log_hex64(slot->stack_top));
    }
    ready_queue_enqueue(slot);
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
    slot->is_idle = 1;
    uint32_t stack_pages = (PROCESS_STACK_SIZE + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (process_alloc_stack(slot, stack_pages) != 0) {
        return -1;
    }
    slot->ctx.rsp = slot->stack_top - (STACK_REDZONE_BYTES + 8u);
    slot->ctx.rip = (uint64_t)(uintptr_t)process_trampoline;
    slot->ctx.rflags = 0x200;
    g_idle_process = slot;
    *out_pid = pid;
    return 0;
}

process_t *process_get(uint32_t pid) {
    return process_find_by_pid(pid);
}

process_t *process_find_by_context(uint32_t context_id) {
    return process_find_by_context_internal(context_id);
}

uint32_t process_current_pid(void) {
    return g_current_pid;
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
    context_switch(&g_current_process->ctx, &g_sched_ctx);
}

void process_block_on_ipc(process_t *process) {
    if (!process) {
        return;
    }
    process->state = PROCESS_STATE_BLOCKED;
    process->block_reason = PROCESS_BLOCK_IPC;
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

    process->block_reason = PROCESS_BLOCK_WAIT;
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
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        process_t *proc = &g_processes[i];
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
        proc->block_reason = PROCESS_BLOCK_NONE;
        proc->state = PROCESS_STATE_READY;
        ready_queue_enqueue(proc);
        woken++;
    }
    return woken;
}

int process_schedule_once(void) {
    if (PROCESS_MAX_COUNT == 0) {
        return 1;
    }

    process_t *proc = ready_queue_dequeue();
    if (!proc || proc->state != PROCESS_STATE_READY || !proc->entry) {
        if (g_idle_process && g_idle_process->state == PROCESS_STATE_READY) {
            proc = g_idle_process;
        } else {
            return 1;
        }
    }

    if (proc->ctx_canary_pre != PROCESS_CTX_CANARY_VALUE ||
        proc->ctx_canary_post != PROCESS_CTX_CANARY_VALUE) {
        serial_write("[sched] ctx canary corrupt before restore pid=");
        process_log_hex64(proc->pid);
        serial_write("[sched] name=");
        serial_write(proc->name ? proc->name : "(null)");
        serial_write("\n[sched] ctx canary pre=");
        process_log_hex64(proc->ctx_canary_pre);
        serial_write("[sched] ctx canary post=");
        process_log_hex64(proc->ctx_canary_post);
        process_log_ctxsw_state();
        process_log_ctx_watch("pre-restore-canary");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    proc->state = PROCESS_STATE_RUNNING;
    if (proc->ticks_remaining == 0) {
        proc->ticks_remaining = proc->time_slice_ticks;
    }
    process_validate_context(proc, "schedule");
    critical_section_enter();
    g_current_pid = proc->pid;
    g_current_process = proc;
    critical_section_leave();
    if (mm_context_activate(proc->context_id) != 0) {
        serial_write("[sched] cr3 activate failed\n");
        critical_section_enter();
        g_current_process = 0;
        g_current_pid = 0;
        critical_section_leave();
        return 1;
    }
    context_switch(&g_sched_ctx, &proc->ctx);
    if (mm_context_activate(0) != 0) {
        serial_write("[sched] root cr3 restore failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    process_run_result_t result = g_last_run_result;
    critical_section_enter();
    g_current_process = 0;
    g_current_pid = 0;
    critical_section_leave();

    if (result == PROCESS_RUN_EXITED) {
        process_mark_exited(proc, proc->exit_status);
    } else if (result == PROCESS_RUN_BLOCKED) {
        if (proc->state == PROCESS_STATE_READY) {
            proc->block_reason = PROCESS_BLOCK_NONE;
        } else {
            proc->state = PROCESS_STATE_BLOCKED;
            if (proc->block_reason == PROCESS_BLOCK_NONE) {
                proc->block_reason = PROCESS_BLOCK_IPC;
            }
        }
    } else {
        proc->state = PROCESS_STATE_READY;
        proc->block_reason = PROCESS_BLOCK_NONE;
        proc->wait_target_pid = 0;
        ready_queue_enqueue(proc);
    }

    g_last_index = proc->pid;
    g_need_resched = 0;
    return (result == PROCESS_RUN_YIELDED) ? 0 : 1;
}

void process_tick(void) {
    if (g_current_pid == 0) {
        return;
    }
    process_t *proc = process_find_by_pid(g_current_pid);
    if (!proc || proc->state != PROCESS_STATE_RUNNING) {
        return;
    }
    proc->ticks_total++;
    if (proc->ticks_remaining > 0) {
        proc->ticks_remaining--;
        if (proc->ticks_remaining == 0) {
            g_need_resched = 1;
            if (!g_preempt_smoke_logged) {
                g_preempt_smoke_logged = 1;
                serial_write("[test] preempt ok\n");
            }
        }
    }
}

int process_should_resched(void) {
    return g_need_resched != 0;
}

void process_clear_resched(void) {
    g_need_resched = 0;
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

    uint64_t start = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t end = (uint64_t)(uintptr_t)&__kernel_end;
    if (process_str_eq(g_current_process->name, "process-manager")) {
        static uint8_t logged_frame_dump;
        if (!logged_frame_dump) {
            logged_frame_dump = 1;
            trace_write("[irq] pm preempt frame dump\n");
            trace_write("[irq] frame ptr=");
            trace_do(process_log_hex64((uint64_t)(uintptr_t)frame));
            trace_write("[irq] frame rip=");
            trace_do(process_log_hex64(frame->rip));
            trace_write("[irq] frame cs=");
            trace_do(process_log_hex64(frame->cs));
            trace_write("[irq] frame rflags=");
            trace_do(process_log_hex64(frame->rflags));
#if WASMOS_TRACE
            uint64_t *raw = (uint64_t *)(uintptr_t)frame;
            for (uint32_t i = 0; i < 8; ++i) {
                trace_write("[irq] frame qword ");
                trace_do(process_log_hex64(i));
                trace_do(process_log_hex64(raw[i]));
            }
            uint64_t *tail = (uint64_t *)((uintptr_t)frame + 120);
            for (uint32_t i = 0; i < 4; ++i) {
                trace_write("[irq] iret qword ");
                trace_do(process_log_hex64(i));
                trace_do(process_log_hex64(tail[i]));
            }
#endif
        }
    }
    if ((frame->rip < start || frame->rip >= end) && process_str_eq(g_current_process->name, "process-manager")) {
        static uint8_t logged_bad_frame;
        if (!logged_bad_frame) {
            logged_bad_frame = 1;
            trace_write("[irq] bad frame rip for pm\n");
            trace_write("[irq] frame ptr=");
            trace_do(process_log_hex64((uint64_t)(uintptr_t)frame));
            trace_write("[irq] frame rip=");
            trace_do(process_log_hex64(frame->rip));
            trace_write("[irq] frame cs=");
            trace_do(process_log_hex64(frame->cs));
            trace_write("[irq] frame rflags=");
            trace_do(process_log_hex64(frame->rflags));
            trace_write("[irq] frame rax=");
            trace_do(process_log_hex64(frame->rax));
            trace_write("[irq] frame rbx=");
            trace_do(process_log_hex64(frame->rbx));
            trace_write("[irq] frame rcx=");
            trace_do(process_log_hex64(frame->rcx));
            trace_write("[irq] frame rdx=");
            trace_do(process_log_hex64(frame->rdx));
            trace_write("[irq] frame rbp=");
            trace_do(process_log_hex64(frame->rbp));
            trace_write("[irq] frame rsi=");
            trace_do(process_log_hex64(frame->rsi));
            trace_write("[irq] frame rdi=");
            trace_do(process_log_hex64(frame->rdi));
            trace_write("[irq] frame r8=");
            trace_do(process_log_hex64(frame->r8));
            trace_write("[irq] frame r9=");
            trace_do(process_log_hex64(frame->r9));
            trace_write("[irq] frame r10=");
            trace_do(process_log_hex64(frame->r10));
            trace_write("[irq] frame r11=");
            trace_do(process_log_hex64(frame->r11));
            trace_write("[irq] frame r12=");
            trace_do(process_log_hex64(frame->r12));
            trace_write("[irq] frame r13=");
            trace_do(process_log_hex64(frame->r13));
            trace_write("[irq] frame r14=");
            trace_do(process_log_hex64(frame->r14));
            trace_write("[irq] frame r15=");
            trace_do(process_log_hex64(frame->r15));
#if WASMOS_TRACE
            uint64_t *raw = (uint64_t *)(uintptr_t)frame;
            for (uint32_t i = 0; i < 8; ++i) {
                trace_write("[irq] frame qword ");
                trace_do(process_log_hex64(i));
                trace_do(process_log_hex64(raw[i]));
            }
#endif
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
    g_current_process->ctx.rsp = (uint64_t)((uintptr_t)frame + sizeof(irq_frame_t));
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
        trace_do(process_log_hex64(g_current_process->pid));
        trace_write("[sched] ctxwatch preempt ctx=");
        trace_do(process_log_hex64(g_ctx_watch_ctx));
        trace_write("[sched] ctxwatch preempt rip=");
        trace_do(process_log_hex64(g_ctx_watch_last_rip));
        trace_write("[sched] ctxwatch preempt rsp=");
        trace_do(process_log_hex64(g_ctx_watch_last_rsp));
        trace_write("[sched] ctxwatch preempt rflags=");
        trace_do(process_log_hex64(g_ctx_watch_last_rflags));
    }

    g_current_process->state = PROCESS_STATE_READY;
    g_current_process->block_reason = PROCESS_BLOCK_NONE;
    ready_queue_enqueue(g_current_process);
    g_last_run_result = PROCESS_RUN_YIELDED;
    process_clear_resched();
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
