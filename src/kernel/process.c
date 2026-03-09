#include "process.h"
#include "memory.h"

static process_t g_processes[PROCESS_MAX_COUNT];
static uint8_t g_process_stacks[PROCESS_MAX_COUNT][PROCESS_STACK_SIZE] __attribute__((aligned(16)));
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
static process_context_t g_sched_ctx;
static uint32_t g_preempt_disable_count;

extern void context_switch(process_context_t *out, process_context_t *in);

static void ready_queue_reset(void) {
    g_ready_head = 0;
    g_ready_tail = 0;
    g_ready_count = 0;
}

static int ready_queue_enqueue(process_t *proc) {
    if (!proc || proc->in_ready_queue) {
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
        if (!g_current_process || !g_current_process->entry) {
            g_last_run_result = PROCESS_RUN_IDLE;
        } else {
            g_last_run_result = g_current_process->entry(g_current_process, g_current_process->arg);
        }
        context_switch(&g_current_process->ctx, &g_sched_ctx);
    }
}

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
    proc->ctx = (process_context_t){0};
    proc->stack_top = 0;
    proc->entry = 0;
    proc->arg = 0;
    proc->name = 0;
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
    proc->state = PROCESS_STATE_ZOMBIE;
    process_wake_waiters(proc->pid);
}

static void process_reap(process_t *proc) {
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
    slot->entry = entry;
    slot->arg = arg;
    slot->name = name;
    uint32_t index = (uint32_t)(slot - g_processes);
    slot->stack_top = (uintptr_t)g_process_stacks[index] + PROCESS_STACK_SIZE;
    slot->ctx.rsp = slot->stack_top - 8u;
    slot->ctx.rip = (uint64_t)(uintptr_t)process_trampoline;
    slot->ctx.rflags = 0x200;
    ready_queue_enqueue(slot);
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
    process->block_reason = PROCESS_BLOCK_IPC;
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
        if (proc->state != PROCESS_STATE_BLOCKED) {
            continue;
        }
        if (proc->block_reason != PROCESS_BLOCK_IPC) {
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
        return 1;
    }

    proc->state = PROCESS_STATE_RUNNING;
    if (proc->ticks_remaining == 0) {
        proc->ticks_remaining = proc->time_slice_ticks;
    }
    g_current_pid = proc->pid;
    g_current_process = proc;
    context_switch(&g_sched_ctx, &proc->ctx);
    process_run_result_t result = g_last_run_result;
    g_current_process = 0;
    g_current_pid = 0;

    if (result == PROCESS_RUN_EXITED) {
        process_mark_exited(proc, proc->exit_status);
    } else if (result == PROCESS_RUN_BLOCKED) {
        proc->state = PROCESS_STATE_BLOCKED;
        if (proc->block_reason == PROCESS_BLOCK_NONE) {
            proc->block_reason = PROCESS_BLOCK_IPC;
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
        }
    }
}

int process_should_resched(void) {
    return g_need_resched != 0;
}

void process_clear_resched(void) {
    g_need_resched = 0;
}

process_context_t *process_preempt_from_irq(const irq_frame_t *frame) {
    if (!frame) {
        return 0;
    }
    if (!process_should_resched() || !preempt_is_enabled()) {
        return 0;
    }
    if (!g_current_process || g_current_process->state != PROCESS_STATE_RUNNING) {
        process_clear_resched();
        return 0;
    }

    g_current_process->ctx.r15 = frame->r15;
    g_current_process->ctx.r14 = frame->r14;
    g_current_process->ctx.r13 = frame->r13;
    g_current_process->ctx.r12 = frame->r12;
    g_current_process->ctx.r11 = frame->r11;
    g_current_process->ctx.r10 = frame->r10;
    g_current_process->ctx.r9 = frame->r9;
    g_current_process->ctx.r8 = frame->r8;
    g_current_process->ctx.rdi = frame->rdi;
    g_current_process->ctx.rsi = frame->rsi;
    g_current_process->ctx.rbp = frame->rbp;
    g_current_process->ctx.rdx = frame->rdx;
    g_current_process->ctx.rcx = frame->rcx;
    g_current_process->ctx.rbx = frame->rbx;
    g_current_process->ctx.rax = frame->rax;
    g_current_process->ctx.rsp = (uint64_t)((uintptr_t)frame + sizeof(irq_frame_t));
    g_current_process->ctx.rip = frame->rip;
    g_current_process->ctx.rflags = frame->rflags;

    g_current_process->state = PROCESS_STATE_READY;
    g_current_process->block_reason = PROCESS_BLOCK_NONE;
    ready_queue_enqueue(g_current_process);
    g_last_run_result = PROCESS_RUN_YIELDED;
    process_clear_resched();
    return &g_sched_ctx;
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

uint32_t process_count_active(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state != PROCESS_STATE_UNUSED) {
            count++;
        }
    }
    return count;
}

int process_info_at(uint32_t index, uint32_t *out_pid, const char **out_name) {
    if (!out_pid || !out_name) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED) {
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
        if (g_processes[i].state == PROCESS_STATE_UNUSED) {
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
