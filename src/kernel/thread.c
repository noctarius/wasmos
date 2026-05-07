#include "thread.h"

#include <stddef.h>

static thread_t g_threads[THREAD_MAX_COUNT];
static uint32_t g_next_tid;
static uint32_t g_current_tid;

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
    thread->time_slice_ticks = 0;
    thread->ticks_remaining = 0;
    thread->ticks_total = 0;
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
    if (owner_pid == 0 || !out_tid) {
        return -1;
    }
    thread_t *slot = thread_find_slot();
    if (!slot) {
        return -1;
    }
    slot->tid = g_next_tid++;
    slot->owner_pid = owner_pid;
    slot->state = THREAD_STATE_READY;
    slot->block_reason = THREAD_BLOCK_NONE;
    /* TODO(threading-phase-a): Scheduler still dispatches process_t directly.
     * Keep slice accounting mirrored here until ready-queue ownership moves to
     * thread_t in Phase B. */
    slot->time_slice_ticks = 0;
    slot->ticks_remaining = 0;
    slot->ticks_total = 0;
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
