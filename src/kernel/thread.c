#include "thread.h"

#include <stddef.h>

static thread_t g_threads[THREAD_MAX_COUNT];
static uint32_t g_next_tid;
static uint32_t g_current_tid;

static void
thread_clear_ctx(process_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->r15 = 0;
    ctx->r14 = 0;
    ctx->r13 = 0;
    ctx->r12 = 0;
    ctx->r11 = 0;
    ctx->r10 = 0;
    ctx->r9 = 0;
    ctx->r8 = 0;
    ctx->rdi = 0;
    ctx->rsi = 0;
    ctx->rbp = 0;
    ctx->rdx = 0;
    ctx->rcx = 0;
    ctx->rbx = 0;
    ctx->rax = 0;
    ctx->rsp = 0;
    ctx->rip = 0;
    ctx->rflags = 0;
    ctx->cs = 0;
    ctx->ss = 0;
    ctx->user_rsp = 0;
    ctx->root_table = 0;
}

static void
thread_reset_slot(thread_t *thread)
{
    if (!thread) {
        return;
    }
    thread->tid = 0;
    thread->owner_pid = 0;
    thread->state = THREAD_STATE_UNUSED;
    thread->block_reason = THREAD_BLOCK_NONE;
    thread->in_ready_queue = 0;
    thread->is_kernel_worker = 0;
    thread->kstack_base = 0;
    thread->kstack_top = 0;
    thread->kstack_alloc_base_phys = 0;
    thread->kstack_pages = 0;
    thread->worker_entry = 0;
    thread->worker_arg = 0;
    thread->time_slice_ticks = 0;
    thread->ticks_remaining = 0;
    thread->ticks_total = 0;
    thread_clear_ctx(&thread->ctx);
    thread->join_waiter_tid = 0;
    thread->exit_status = 0;
    for (uint32_t i = 0; i < THREAD_NAME_MAX; ++i) {
        thread->name_storage[i] = '\0';
    }
    thread->name = 0;
}

static thread_t *
thread_find_slot(void)
{
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        if (g_threads[i].state == THREAD_STATE_UNUSED) {
            return &g_threads[i];
        }
    }
    return 0;
}

static int
thread_copy_name(thread_t *thread, const char *name)
{
    if (!thread || !name) {
        return -1;
    }
    uint32_t i = 0;
    for (; name[i] && i + 1 < THREAD_NAME_MAX; ++i) {
        thread->name_storage[i] = name[i];
    }
    thread->name_storage[i] = '\0';
    thread->name = thread->name_storage;
    return name[i] == '\0' ? 0 : -1;
}

void
thread_init(void)
{
    g_next_tid = 1;
    g_current_tid = 0;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_reset_slot(&g_threads[i]);
    }
}

int
thread_spawn_main(uint32_t owner_pid, const char *name, uint32_t *out_tid)
{
    return thread_spawn_in_owner(owner_pid,
                                 name,
                                 THREAD_STATE_READY,
                                 THREAD_BLOCK_NONE,
                                 out_tid);
}

int
thread_spawn_in_owner(uint32_t owner_pid,
                      const char *name,
                      thread_state_t initial_state,
                      thread_block_reason_t initial_reason,
                      uint32_t *out_tid)
{
    if (owner_pid == 0 || !out_tid) {
        return -1;
    }
    thread_t *slot = thread_find_slot();
    if (!slot) {
        return -1;
    }
    slot->tid = g_next_tid++;
    slot->owner_pid = owner_pid;
    slot->state = initial_state;
    slot->block_reason = initial_reason;
    slot->in_ready_queue = 0;
    slot->is_kernel_worker = 0;
    slot->kstack_base = 0;
    slot->kstack_top = 0;
    slot->kstack_alloc_base_phys = 0;
    slot->kstack_pages = 0;
    slot->worker_entry = 0;
    slot->worker_arg = 0;
    slot->time_slice_ticks = 0;
    slot->ticks_remaining = 0;
    slot->ticks_total = 0;
    thread_clear_ctx(&slot->ctx);
    slot->join_waiter_tid = 0;
    slot->exit_status = 0;
    if (thread_copy_name(slot, name ? name : "") != 0) {
        thread_reset_slot(slot);
        return -1;
    }
    *out_tid = slot->tid;
    return 0;
}

thread_t *
thread_get(uint32_t tid)
{
    if (tid == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        if (g_threads[i].tid == tid && g_threads[i].state != THREAD_STATE_UNUSED) {
            return &g_threads[i];
        }
    }
    return 0;
}

thread_t *
thread_find_main_for_pid(uint32_t owner_pid)
{
    if (owner_pid == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        if (g_threads[i].owner_pid == owner_pid &&
            g_threads[i].state != THREAD_STATE_UNUSED) {
            return &g_threads[i];
        }
    }
    return 0;
}

int
thread_owner_tid_at(uint32_t owner_pid, uint32_t index, uint32_t *out_tid)
{
    if (owner_pid == 0 || !out_tid) {
        return -1;
    }
    uint32_t current = 0;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t *thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        if (current == index) {
            *out_tid = thread->tid;
            return 0;
        }
        current++;
    }
    return -1;
}

void
thread_mark_owner_exited(uint32_t owner_pid, int32_t exit_status)
{
    if (owner_pid == 0) {
        return;
    }
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t *thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        thread->exit_status = exit_status;
        thread->state = THREAD_STATE_ZOMBIE;
        thread->block_reason = THREAD_BLOCK_NONE;
        if (g_current_tid == thread->tid) {
            g_current_tid = 0;
        }
    }
}

void
thread_reap_owner(uint32_t owner_pid)
{
    if (owner_pid == 0) {
        return;
    }
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t *thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        if (g_current_tid == thread->tid) {
            g_current_tid = 0;
        }
        thread_reset_slot(thread);
    }
}

void
thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason)
{
    thread_t *thread = thread_get(tid);
    if (!thread) {
        return;
    }
    thread->state = state;
    thread->block_reason = reason;
}

void
thread_set_exit_status(uint32_t tid, int32_t exit_status)
{
    thread_t *thread = thread_get(tid);
    if (!thread) {
        return;
    }
    thread->exit_status = exit_status;
}

void
thread_reap(uint32_t tid)
{
    thread_t *thread = thread_get(tid);
    if (!thread) {
        return;
    }
    if (g_current_tid == tid) {
        g_current_tid = 0;
    }
    thread_reset_slot(thread);
}

void
thread_set_current(uint32_t tid)
{
    g_current_tid = tid;
}

uint32_t
thread_current_tid(void)
{
    return g_current_tid;
}
