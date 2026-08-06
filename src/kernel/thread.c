/* thread.c - Kernel thread table for per-process threading.
 * Each thread_t has its own kernel stack and saved process_context_t but shares
 * its owner process's address space.  THREAD_MAX_COUNT limits total live threads. */
#include "thread.h"
#include "arch/x86_64/smp.h"
#include "sync/spinlock.h"

static thread_t g_threads[THREAD_MAX_COUNT];
static uint32_t g_next_tid;
static ksync_spinlock_t g_thread_table_lock;

static void thread_clear_ctx(process_context_t* ctx) {
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

/* thread.c-internal terminal scrub and the SOLE sanctioned sink to UNUSED(DEAD):
 * the reaper's ZOMBIE->UNUSED, boot-init (garbage->UNUSED), and spawn-abort
 * (NEW->UNUSED).  Because it lives inside thread.* (the state owner) and always
 * runs under g_thread_table_lock, it is not an "external" writer — it does not
 * need thread_transit (which gates the live edges + external callers), and
 * cannot go through it anyway (boot-init has no valid `from`). */
static void thread_reset_slot(thread_t* thread) {
    if (!thread) {
        return;
    }
    thread->tid = 0;
    thread->owner_pid = 0;
    thread->state = THREAD_STATE_UNUSED;
    thread->block_reason = THREAD_BLOCK_NONE;
    thread->is_kernel_worker = 0;
    thread->blocking_transition = 0;
    thread->wake_pending = 0;
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
    thread->wait_target_pid = 0;
    thread->join_waiter_tid = 0;
    thread->detached = 0;
    thread->exit_status = 0;
    thread->wasm3_heap_bound_pid = 0;
    thread->sched_timeout_tick = 0;
    for (uint32_t i = 0; i < THREAD_NAME_MAX; ++i) {
        thread->name_storage[i] = '\0';
    }
    thread->name = 0;
}

static thread_t* thread_find_slot(void) {
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        if (g_threads[i].state == THREAD_STATE_UNUSED) {
            return &g_threads[i];
        }
    }
    return 0;
}

static int thread_copy_name(thread_t* thread, const char* name) {
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

void thread_init(void) {
    ksync_spinlock_init(&g_thread_table_lock);
    g_next_tid = 1;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_reset_slot(&g_threads[i]);
    }
}

int thread_spawn_main(uint32_t owner_pid, const char* name, uint32_t* out_tid) {
    return thread_spawn_in_owner(owner_pid, name, THREAD_STATE_READY, THREAD_BLOCK_NONE, out_tid);
}

int thread_spawn_in_owner(uint32_t owner_pid, const char* name, thread_state_t initial_state,
                          thread_block_reason_t initial_reason, uint32_t* out_tid) {
    if (owner_pid == 0 || !out_tid) {
        return -1;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* slot = thread_find_slot();
    if (!slot) {
        ksync_spinlock_unlock(&g_thread_table_lock);
        return -1;
    }
    slot->tid = g_next_tid++;
    slot->owner_pid = owner_pid;
    /* Claim the free (UNUSED/DEAD) slot into NEW first: it is never schedulable
     * and never a legal source of ->READY until fully initialised below.  All
     * under the table lock, so the claim + publish is atomic w.r.t. other
     * spawns; the CAS form keeps the edge honest per the state machine. */
    slot->state = THREAD_STATE_NEW;
    slot->block_reason = initial_reason;
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
    slot->wait_target_pid = 0;
    slot->join_waiter_tid = 0;
    slot->detached = 0;
    slot->exit_status = 0;
    if (thread_copy_name(slot, name ? name : "") != 0) {
        thread_reset_slot(slot);
        ksync_spinlock_unlock(&g_thread_table_lock);
        return -1;
    }
    /* Publish: NEW -> READY|BLOCKED once the slot is fully built. */
    if (!thread_transit(slot, THREAD_STATE_NEW, initial_state)) {
        thread_reset_slot(slot);
        ksync_spinlock_unlock(&g_thread_table_lock);
        return -1;
    }
    *out_tid = slot->tid;
    ksync_spinlock_unlock(&g_thread_table_lock);
    return 0;
}

static thread_t* thread_get_nolock(uint32_t tid) {
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

thread_t* thread_get(uint32_t tid) {
    if (tid == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    ksync_spinlock_unlock(&g_thread_table_lock);
    return thread;
}

thread_t* thread_table_at(uint32_t index) {
    if (index >= THREAD_MAX_COUNT) {
        return 0;
    }
    return &g_threads[index];
}

thread_t* thread_find_main_for_pid(uint32_t owner_pid) {
    if (owner_pid == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        if (g_threads[i].owner_pid == owner_pid && g_threads[i].state != THREAD_STATE_UNUSED) {
            thread_t* thread = &g_threads[i];
            ksync_spinlock_unlock(&g_thread_table_lock);
            return thread;
        }
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
    return 0;
}

int thread_owner_tid_at(uint32_t owner_pid, uint32_t index, uint32_t* out_tid) {
    if (owner_pid == 0 || !out_tid) {
        return -1;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    uint32_t current = 0;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        if (current == index) {
            *out_tid = thread->tid;
            ksync_spinlock_unlock(&g_thread_table_lock);
            return 0;
        }
        current++;
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
    return -1;
}

void thread_mark_owner_exited(uint32_t owner_pid, int32_t exit_status) {
    if (owner_pid == 0) {
        return;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        thread->exit_status = exit_status;
        /* Tombstone via the state machine (*->ZOMBIE).  Legal from
         * READY/RUNNING/BLOCKED; idempotent if already ZOMBIE. */
        thread_transit(thread, thread->state, THREAD_STATE_ZOMBIE);
        thread->block_reason = THREAD_BLOCK_NONE;
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
}

void thread_reap_owner(uint32_t owner_pid) {
    if (owner_pid == 0) {
        return;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        thread_reset_slot(thread);
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
}

/* Legal thread state-machine edges (see design/smp-reap-fsm-reland):
 *   UNUSED(DEAD) -> NEW                       (allocator claims a free slot)
 *   NEW          -> READY | BLOCKED           (spawn, after init)
 *   READY        -> RUNNING | ZOMBIE
 *   RUNNING      -> READY | BLOCKED | ZOMBIE
 *   BLOCKED      -> READY | ZOMBIE
 *   ZOMBIE       -> UNUSED(DEAD)              (reaper / CPU0 only)
 * ZOMBIE is monotonic (only the reaper leaves it), which is what makes an
 * "all threads zombie" observation stable. */
static int thread_transition_legal(thread_state_t from, thread_state_t to) {
    if (from == to) {
        return 1; /* idempotent no-op is always allowed */
    }
    /* The state machine enforces exactly two invariants; everything else among
     * the live states (READY/RUNNING/BLOCKED interconversions) is permitted so
     * we never reject a legitimate scheduler move:
     *   1. ZOMBIE is MONOTONIC — it may only advance to UNUSED (the reaper).
     *      Nothing may resurrect a zombie; this makes "all threads zombie"
     *      a stable predicate for the reap gate.
     *   2. A free (UNUSED/DEAD) slot may only enter NEW — never jump straight
     *      to a schedulable state (the free-slot-activation hole).  NEW then
     *      publishes to READY/BLOCKED once fully initialised. */
    if (from == THREAD_STATE_ZOMBIE) {
        return to == THREAD_STATE_UNUSED;
    }
    if (from == THREAD_STATE_UNUSED) {
        return to == THREAD_STATE_NEW;
    }
    if (to == THREAD_STATE_UNUSED || to == THREAD_STATE_NEW) {
        return 0; /* only the two edges above reach UNUSED/NEW */
    }
    if (from == THREAD_STATE_NEW) {
        return to == THREAD_STATE_READY || to == THREAD_STATE_BLOCKED;
    }
    /* from is READY/RUNNING/BLOCKED; to is READY/RUNNING/BLOCKED/ZOMBIE — all ok. */
    return 1;
}

int thread_transit(thread_t* thread, thread_state_t from, thread_state_t to) {
    if (!thread) {
        return 0;
    }
    if (!thread_transition_legal(from, to)) {
        return 0;
    }
    uint32_t expected = (uint32_t)from;
    return __atomic_compare_exchange_n((uint32_t*)&thread->state, &expected, (uint32_t)to, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
               ? 1
               : 0;
}

void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    if (!thread) {
        ksync_spinlock_unlock(&g_thread_table_lock);
        return;
    }
    /* Enforce the state machine: reject illegal edges (notably any attempt to
     * leave ZOMBIE, which would resurrect a thread the reaper is tearing down
     * and break the "all threads zombie" gate).  Under the table lock so the
     * read-decide-write is atomic. */
    if (thread_transition_legal(thread->state, state)) {
        thread->state = state;
        thread->block_reason = reason;
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
}

/* Atomically transition a thread from BLOCKED to READY under the table lock.
 * Returns 1 if the state was changed, 0 if the thread was not BLOCKED or not
 * found.  Callers must enqueue the thread separately when this returns 1. */
int thread_wake_if_blocked(uint32_t tid) {
    if (tid == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    if (!thread || thread->state != THREAD_STATE_BLOCKED) {
        ksync_spinlock_unlock(&g_thread_table_lock);
        return 0;
    }
    thread->state = THREAD_STATE_READY;
    thread->block_reason = THREAD_BLOCK_NONE;
    ksync_spinlock_unlock(&g_thread_table_lock);
    return 1;
}

void thread_set_exit_status(uint32_t tid, int32_t exit_status) {
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    if (!thread) {
        ksync_spinlock_unlock(&g_thread_table_lock);
        return;
    }
    thread->exit_status = exit_status;
    ksync_spinlock_unlock(&g_thread_table_lock);
}

void thread_reap(uint32_t tid) {
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    if (!thread) {
        ksync_spinlock_unlock(&g_thread_table_lock);
        return;
    }
    thread_reset_slot(thread);
    ksync_spinlock_unlock(&g_thread_table_lock);
}

void thread_set_current(uint32_t tid) {
    if (tid == 0) {
        cpu_local()->current_thread = 0;
        return;
    }
    cpu_local()->current_thread = thread_get(tid);
}

uint32_t thread_current_tid(void) {
    thread_t* thread = cpu_local()->current_thread;
    return thread ? thread->tid : 0;
}
