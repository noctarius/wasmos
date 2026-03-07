#include "process.h"
#include "memory.h"

static process_t g_processes[PROCESS_MAX_COUNT];
static uint32_t g_next_pid;
static uint32_t g_last_index;

static process_t *process_find_slot(void) {
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_UNUSED ||
            g_processes[i].state == PROCESS_STATE_TERMINATED) {
            return &g_processes[i];
        }
    }
    return 0;
}

void process_init(void) {
    g_next_pid = 1;
    g_last_index = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_processes[i].pid = 0;
        g_processes[i].context_id = 0;
        g_processes[i].state = PROCESS_STATE_UNUSED;
        g_processes[i].entry = 0;
        g_processes[i].arg = 0;
        g_processes[i].name = 0;
    }
}

int process_spawn(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid) {
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
    slot->context_id = ctx->id;
    slot->state = PROCESS_STATE_READY;
    slot->entry = entry;
    slot->arg = arg;
    slot->name = name;
    *out_pid = pid;
    return 0;
}

process_t *process_get(uint32_t pid) {
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
        proc->state = PROCESS_STATE_READY;
        woken++;
    }
    return woken;
}

int process_schedule_once(void) {
    if (PROCESS_MAX_COUNT == 0) {
        return 1;
    }

    for (uint32_t scan = 0; scan < PROCESS_MAX_COUNT; ++scan) {
        uint32_t index = (g_last_index + scan) % PROCESS_MAX_COUNT;
        process_t *proc = &g_processes[index];
        if (proc->state != PROCESS_STATE_READY || !proc->entry) {
            continue;
        }

        proc->state = PROCESS_STATE_RUNNING;
        process_run_result_t result = proc->entry(proc, proc->arg);

        if (result == PROCESS_RUN_EXITED) {
            proc->state = PROCESS_STATE_TERMINATED;
        } else if (result == PROCESS_RUN_BLOCKED) {
            proc->state = PROCESS_STATE_BLOCKED;
        } else {
            proc->state = PROCESS_STATE_READY;
        }

        g_last_index = (index + 1) % PROCESS_MAX_COUNT;
        return (result == PROCESS_RUN_YIELDED) ? 0 : 1;
    }

    return 1;
}

uint32_t process_count_active(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_READY ||
            g_processes[i].state == PROCESS_STATE_RUNNING ||
            g_processes[i].state == PROCESS_STATE_BLOCKED) {
            count++;
        }
    }
    return count;
}
