#include "process.h"
#include "memory.h"

static process_t g_processes[PROCESS_MAX_COUNT];
static uint32_t g_next_pid;
static uint32_t g_last_index;
static uint32_t g_current_pid;

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
    slot->entry = entry;
    slot->arg = arg;
    slot->name = name;
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
        g_current_pid = proc->pid;
        process_run_result_t result = proc->entry(proc, proc->arg);
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
