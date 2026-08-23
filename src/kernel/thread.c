/* thread.c - Kernel thread table for per-process threading.
 * Each thread_t has its own kernel stack and saved process_context_t but shares
 * its owner process's address space.  THREAD_MAX_COUNT limits total live threads. */
#include "thread.h"
#include "arch/x86_64/smp.h"
#include "sched.h"
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
 * (NEW->UNUSED).  Because it lives inside thread.* (the state owner), it is not
 * an "external" writer: it does not need thread_transit (which gates the live
 * edges + external callers), and cannot go through it anyway — boot-init has no
 * valid `from`, and NEW->UNUSED is not a legal transit edge.
 *
 * Every caller except thread_init() holds g_thread_table_lock; thread_init runs
 * once on the BSP before any other thread exists, so there is nothing to
 * exclude. */
/* Returns 1 when the slot was released to the allocator, 0 when it was left
 * alone because a CPU has it claimed for dispatch.
 *
 * A dispatch works through a raw pointer to a SLOT: process_schedule_once_impl
 * picks a thread, drops the queue lock, validates it, and only then reads
 * kstack_top, time_slice_ticks and worker_entry. Releasing the slot anywhere in
 * that sequence leaves the dispatching CPU reading a zeroed thread -- observed as
 * process_sched_invariant_fail("zero time slice") with tid 0 -- and, once the
 * allocator hands it on, running on a stack that belongs to somebody else.
 * Re-validating in the dispatcher cannot close it: the reset can land in any
 * window between the last check and the use.
 *
 * THREAD_STATE_RUNNING is the marker because cpu_sched_claim_for_dispatch sets it
 * BEFORE the dispatcher touches anything fragile, so it covers the whole
 * sequence. Refusing defers the teardown rather than dropping it: the claiming
 * CPU's dispatch ends in process_schedule_once_impl and the reap is retried from
 * there (process_try_auto_reap) and from the wait/PM paths. */
static int thread_reset_slot(thread_t* thread) {
    if (!thread) {
        return 0;
    }
    /* Claim the free with a CAS on the same word cpu_sched_claim_for_dispatch
     * writes. A plain test-then-teardown is a TOCTOU against it: this function
     * runs under g_thread_table_lock, which serialises free against ALLOCATE, but
     * a dispatch claim takes no table lock, so the two only serialise if both go
     * through the state word. Reading a non-RUNNING state here and tearing down
     * afterwards lets a claim land in between and hand the claiming CPU a zeroed
     * slot.
     *
     * Publishing UNUSED before the unlink below is safe and deliberate: the
     * allocator cannot take the slot (this runs under the table lock, and so does
     * thread_find_slot), and the lock-free readers that walk the table all skip a
     * slot whose state is UNUSED. */
    /* Win the slot claim, or refuse. A load-then-teardown would be a TOCTOU
     * against the dispatcher, which takes its own claim after the queue lock is
     * dropped: the load could read FREE, the dispatcher could then claim and
     * start reading kstack_top/worker_entry, and this teardown would proceed
     * underneath it. Both sides CAS from FREE so exactly one wins. */
    uint32_t claim = THREAD_SLOT_FREE;
    if (!__atomic_compare_exchange_n(&thread->dispatch_ref,
                                     &claim,
                                     THREAD_SLOT_FROZEN,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return 0; /* a dispatch owns it (or another reset is already tearing down) */
    }
    /* Both refusals below hand the claim back before returning, and that is
     * required, not tidiness.  Every contender for this word CASes from FREE, so a
     * slot left FROZEN is unrecoverable in two directions at once: a later reap
     * fails the claim instead of reaching the state check, so the deferred
     * teardown can never be retried and thread_reap_owner burns all its passes
     * before reporting a leftover; and the dispatcher's claim fails too, so the
     * thread this call declined to free can never be dispatched again -- it is
     * picked, unlinked, dropped without a re-enqueue, and stranded.  One refused
     * reset would cost a slot out of a fixed-size table permanently. */
    uint32_t expected = __atomic_load_n((uint32_t*)&thread->state, __ATOMIC_ACQUIRE);
    if (expected == THREAD_STATE_RUNNING) {
        __atomic_store_n(&thread->dispatch_ref, THREAD_SLOT_FREE, __ATOMIC_RELEASE);
        return 0;
    }
    if (!__atomic_compare_exchange_n((uint32_t*)&thread->state,
                                     &expected,
                                     (uint32_t)THREAD_STATE_UNUSED,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&thread->dispatch_ref, THREAD_SLOT_FREE, __ATOMIC_RELEASE);
        return 0; /* a dispatch claim (or another transition) won; retry later */
    }
    /* Unlink from whatever run queue still holds this thread BEFORE the slot is
     * released to the allocator.  A slot freed while its sched_node is linked is
     * handed to the next spawn, whose sched_thread_init re-initialises the node
     * (self-linking it) while the old queue still points at it: the queue is
     * spliced through a node that now has two owners, its band counter
     * underflows, and its ready bit can never clear again -- the picker then
     * returns that one node on every dispatch forever ("[sched] dequeued
     * non-ready" at scheduler speed, livelocking the CPU). */
    cpu_sched_remove_thread(thread);
    /* Drop any owed-enqueue claim, for the same reason the node above is
     * unlinked: it is scheduler state the allocator must not hand to the next
     * spawn.  Two distinct defects, both by inspection -- this field is simply
     * absent from the reset below, which clears every other scheduler field:
     *
     *   - g_enqueue_owed_count is never decremented for the reaped thread, so it
     *     ratchets upward and sched_sweep_owed_enqueues never returns to its
     *     cheap no-debt path again.
     *   - the claim itself outlives the thread, and the sweep's only guard is
     *     `tid == 0` -- which a slot the next spawn has already re-stamped does
     *     not satisfy.  The claim is then honoured against that new thread,
     *     enqueuing it before its spawner has assigned time_slice_ticks or
     *     published its process.
     *
     * The second is a plausible contributor to the "zero time slice" and "spawn
     * publish NEW->LIVE failed" panics tracked in docs/TASKS.md, but clearing it
     * did not measurably reduce either on its own; it is fixed here because it is
     * wrong, not because it closes those. */
    sched_drop_owed_enqueue(thread);
    /* Leave the node in the canonical detached form.  A zero-filled node (BSS at
     * boot, before any sched_thread_init) has next == NULL, which list_head_empty
     * reports as LINKED -- so "is this thread queued?" answers wrongly for every
     * slot's first use unless the detached state is established here. */
    list_head_init(&thread->sched_node);
    thread->on_rq = 0;
    thread->rq = 0;
    thread->tid = 0;
    thread->owner_pid = 0;
    thread->state = THREAD_STATE_UNUSED;
    thread->block_reason = THREAD_BLOCK_NONE;
    thread->is_kernel_worker = 0;
    thread->blocking_transition = 0;
    thread->wake_pending = 0;
    /* Scrubbed with the rest: a breadcrumb that outlived its thread would name a
     * promotion of the PREVIOUS occupant of this slot. */
    thread->ready_by = 0;
    thread->rq_enq_result = SCHED_ENQ_NONE;
    thread->rq_unlink_site = SCHED_UNLINK_NONE;
    thread->rq_link_count = 0;
    thread->rq_enq_by = 0;
    thread->kstack_base = 0;
    thread->kstack_top = 0;
    thread->kstack_alloc_base_phys = 0;
    thread->kstack_pages = 0;
    thread->worker_entry = 0;
    thread->worker_arg = 0;
    thread->time_slice_ticks = 0;
    thread->ticks_remaining = 0;
    thread->ticks_total = 0;
    thread->dispatch_count = 0;
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
    /* Publish the slot as claimable only now that every field is reset. Until
     * this store it reads as FROZEN, so neither a dispatcher nor another reset
     * can take it mid-teardown. */
    __atomic_store_n(&thread->dispatch_ref, THREAD_SLOT_FREE, __ATOMIC_RELEASE);
    return 1;
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

/* Scrubs the whole table to UNUSED and restarts tid allocation at 1 (0 is the
 * reserved "no thread" id).  Runs once on the BSP from process_init(), before
 * any other thread exists — which is why it is the one thread_reset_slot caller
 * that does not hold g_thread_table_lock, and why re-running it against a live
 * system would silently free every thread out from under the scheduler. */
void thread_init(void) {
    ksync_spinlock_init(&g_thread_table_lock);
    g_next_tid = 1;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        (void)thread_reset_slot(&g_threads[i]);
    }
}

/* thread_spawn_in_owner, publishing straight to READY.  Only for a thread whose
 * scheduler state is already usable, or whose caller runs before any other CPU
 * can pick it up: publishing READY makes it a legal wake and enqueue target on
 * every CPU immediately.  The worker and user-thread spawn paths deliberately
 * use the BLOCKED form and promote after sched_thread_init instead. */
int thread_spawn_main(uint32_t owner_pid, const char* name, uint32_t* out_tid) {
    return thread_spawn_in_owner(owner_pid, name, THREAD_STATE_READY, THREAD_BLOCK_NONE, out_tid);
}

/* Claims a free slot for `owner_pid`, initialises the bookkeeping, and publishes
 * it in `initial_state` (READY or BLOCKED).  Returns 0 with *out_tid set, or -1
 * for owner_pid 0, a NULL out_tid, an exhausted table, a name longer than
 * THREAD_NAME_MAX-1, or a rejected publish; the slot is scrubbed back to UNUSED
 * on every failure after the claim, so nothing leaks.  A NULL name is accepted
 * and becomes "".
 *
 * The thread is NOT scheduler-ready on return: no kernel stack, no context, no
 * sched_node — sched_thread_init and a stack allocation are the caller's job.
 * That is why the workers spawn BLOCKED and are promoted afterwards.  The whole
 * function runs under g_thread_table_lock, so the claim and the publish cannot
 * be split by another CPU's spawn. */
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
    /* Claim the free (UNUSED/DEAD) slot into NEW first: NEW is never
     * schedulable, and thread_find_slot only ever hands back an UNUSED slot, so
     * nothing outside this table lock can see or move it.  A plain store
     * suffices here for that reason; the publish at the end of this function
     * goes through thread_transit's CAS, which is the edge other CPUs race. */
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
        (void)thread_reset_slot(slot);
        ksync_spinlock_unlock(&g_thread_table_lock);
        return -1;
    }
    /* Publish: NEW -> READY|BLOCKED once the slot is fully built. */
    if (!thread_transit(slot, THREAD_STATE_NEW, initial_state)) {
        (void)thread_reset_slot(slot);
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

/* Resolves a tid to its slot, or 0 for tid 0 (the "no thread" sentinel) and for
 * a tid with no live slot.
 *
 * The table lock is dropped before returning, so the pointer is only as valid as
 * the caller's independent reason to believe that thread is alive: once
 * thread_reap/thread_reap_owner releases the slot, the same storage is handed to
 * an unrelated spawn.  Every caller here holds that guarantee structurally (it
 * is the current thread, or a thread of a process being held from reaping).
 * Takes g_thread_table_lock, so it must not be called with that lock held —
 * thread_get_nolock is the in-file variant for that. */
thread_t* thread_get(uint32_t tid) {
    if (tid == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    ksync_spinlock_unlock(&g_thread_table_lock);
    return thread;
}

/* Raw slot access by table index, for whole-table sweeps (sched_timeout_check).
 * Returns 0 only for an out-of-range index — an in-range slot is returned
 * whatever its state, INCLUDING UNUSED, so a caller must filter on the fields it
 * cares about rather than treat a non-NULL result as a live thread.  Takes no
 * lock at all: the slot address is fixed for the life of the system, so the
 * pointer never dangles, but every field read through it races the table lock's
 * writers and must be an atomic load if the answer matters. */
thread_t* thread_table_at(uint32_t index) {
    if (index >= THREAD_MAX_COUNT) {
        return 0;
    }
    return &g_threads[index];
}

/* First live slot in table order whose owner_pid matches.  It does NOT consult
 * the owning process's main_tid, so with several threads in one process the
 * answer is whichever slot the allocator handed out first, which need not be the
 * main thread.  Returns 0 for owner_pid 0 or no match.  Same pointer-lifetime
 * caveat as thread_get: the lock is dropped before returning. */
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

/* Enumerates a process's threads: writes the `index`-th live thread of
 * `owner_pid` into *out_tid and returns 0, or returns -1 once `index` is past
 * the end (which is the loop-termination signal every caller uses).
 *
 * The enumeration is by table position and is only stable while nothing spawns
 * or reaps a thread of that process, and the lock is released between calls — so
 * a walk that races a reap can skip or repeat a thread.  Callers tolerate this
 * by re-validating each tid with thread_get before use. */
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

/* Tombstones every live thread of `owner_pid` as ZOMBIE with the given status.
 * Threads are NOT unlinked from their run queues here and their stacks are not
 * freed — thread_reap_owner does that later, and until then the lazy sweeps in
 * cpu_sched_pick_next / cpu_sched_steal_pick drop the tombstoned nodes as they
 * meet them.  Because ZOMBIE is monotonic, this is what makes "the process is
 * dead" a stable observation for the reap gate.  A thread currently RUNNING on
 * another CPU is marked too; it finishes its timeslice and is then handled by
 * the zombie branch of process_schedule_once_impl. */
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

/* Non-zero if any slot owned by `owner_pid` is claimed for dispatch or is
 * currently RUNNING.
 *
 * One pass under g_thread_table_lock, on purpose. The ordinal accessors
 * (thread_owner_tid_at) drop the lock between calls, so a caller walking by index
 * can have entries shift under it as slots are reaped and skip the one thread it
 * was looking for -- which for a lifetime check means answering "nobody is
 * dispatching" while somebody is. The answer is only ever a snapshot, but it has
 * to be a snapshot of one consistent table.
 *
 * Both conditions matter: the claim covers a dispatch whose thread state has
 * already moved on (a thread that exits is ZOMBIE for the tail of its own result
 * handling), and RUNNING covers a thread whose claim this caller does not own. */
uint8_t thread_owner_has_active_dispatch(uint32_t owner_pid) {
    uint8_t active = 0;
    if (owner_pid == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_thread_table_lock);
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        if (__atomic_load_n(&thread->dispatch_ref, __ATOMIC_ACQUIRE) != THREAD_SLOT_FREE ||
            __atomic_load_n((uint32_t*)&thread->state, __ATOMIC_ACQUIRE) == THREAD_STATE_RUNNING) {
            active = 1;
            break;
        }
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
    return active;
}

/* Releases every slot belonging to `owner_pid`, whatever its state.  Only safe
 * from the process reap path, which has already won the ZOMBIE -> REAPING claim
 * and therefore owns the process exclusively; called against a live process it
 * would free threads out from under a running CPU.
 *
 * Lock order: g_thread_table_lock is held across thread_reset_slot, which calls
 * cpu_sched_remove_thread and takes a run-queue lock inside it.  Nothing in the
 * scheduler takes the thread table lock while holding a queue lock, so this
 * direction is the only one and it does not invert. */
/* Passes thread_reap_owner will make before giving up and reporting leftovers.
 * A few is plenty: each refusal is a dispatch claim that releases within a
 * handful of instructions. */
#define THREAD_REAP_OWNER_PASSES 64u

static uint32_t thread_reap_owner_pass(uint32_t owner_pid);

uint32_t thread_reap_owner(uint32_t owner_pid) {
    uint32_t refused = 0;
    if (owner_pid == 0) {
        return 0;
    }
    /* Bounded retry, not a lock. A slot is refused only while some CPU holds its
     * dispatch claim, and a CPU that claimed a thread of a process this far into
     * teardown loses at process_set_running (which refuses a ZOMBIE or REAPING
     * owner) and releases within a handful of instructions. The window provably
     * closes, so retrying is finite; the bound is there so a bug elsewhere shows
     * up as a reported leftover rather than a hung reaper. */
    for (uint32_t pass = 0; pass < THREAD_REAP_OWNER_PASSES; ++pass) {
        refused = thread_reap_owner_pass(owner_pid);
        if (refused == 0) {
            break;
        }
    }
    return refused;
}

static uint32_t thread_reap_owner_pass(uint32_t owner_pid) {
    uint32_t refused = 0;
    ksync_spinlock_lock(&g_thread_table_lock);
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* thread = &g_threads[i];
        if (thread->state == THREAD_STATE_UNUSED || thread->owner_pid != owner_pid) {
            continue;
        }
        if (!thread_reset_slot(thread)) {
            refused++;
        }
    }
    ksync_spinlock_unlock(&g_thread_table_lock);
    return refused;
}

/* Legal thread state-machine edges, as enforced below:
 *   UNUSED(DEAD) -> NEW                       (allocator claims a free slot)
 *   NEW          -> READY | BLOCKED           (spawn, after init)
 *   READY        -> RUNNING | BLOCKED | ZOMBIE
 *   RUNNING      -> READY | BLOCKED | ZOMBIE
 *   BLOCKED      -> READY | RUNNING | ZOMBIE
 *   ZOMBIE       -> UNUSED(DEAD)              (the reap path, on whichever CPU
 *                                              won the process reap claim)
 * ZOMBIE is monotonic (only the reap path leaves it), which is what makes an
 * "all threads zombie" observation stable.
 *
 * thread_reset_slot bypasses this table by design and is the one writer that
 * may reach UNUSED from NEW (spawn abort) as well as from ZOMBIE. */
static int thread_transition_legal(thread_state_t from, thread_state_t to) {
    if (from == to) {
        return 1; /* idempotent no-op is always allowed */
    }
    /* The state machine enforces exactly two invariants; everything else among
     * the live states (READY/RUNNING/BLOCKED interconversions) is permitted, so
     * no legitimate scheduler move is ever rejected:
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

/* Atomic conditional state change: succeeds (returns 1) only if the edge is
 * legal per the table above AND the thread was still in `from`.  Returns 0
 * otherwise, without distinguishing "illegal edge" from "lost the race" —
 * callers do not act on the difference, they retry or give up.
 *
 * The CAS is what makes decide-and-write atomic across CPUs, so this is the
 * primitive the lockless wake paths use INSTEAD of the table lock; a caller
 * already holding g_thread_table_lock may use it too, the two do not conflict.
 * from == to passes the legality table, so it reports success exactly when the
 * thread is already in that state and 0 when it has moved on. */
/* Record who promoted `thread` to READY.  Stored raw and screened at PRINT time:
 * the stall dump already resolves an address to a symbol and labels anything
 * below the higher half, so duplicating that screen here would only add a paging
 * dependency to every host suite that links this file.  Relaxed because nothing
 * but the dump reads it.
 *
 * ONE frame, not two: __builtin_return_address(1) is rejected as unsafe
 * (-Wframe-address) and suppressing that would trade a real fault risk for
 * diagnostic convenience.  One frame names the primitive's caller, which is
 * specific enough to narrow the promotion to a handful of sites; if a capture
 * lands on a middleman such as sched_mark_ready_if_live, that middleman can
 * record its own caller then. */
static inline void thread_note_ready_by(thread_t* thread, void* ret0) {
    __atomic_store_n(&thread->ready_by, (uint64_t)(uintptr_t)ret0, __ATOMIC_RELAXED);
}

int thread_transit(thread_t* thread, thread_state_t from, thread_state_t to) {
    if (!thread) {
        return 0;
    }
    if (!thread_transition_legal(from, to)) {
        return 0;
    }
    uint32_t expected = (uint32_t)from;
    if (!__atomic_compare_exchange_n((uint32_t*)&thread->state,
                                     &expected,
                                     (uint32_t)to,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return 0;
    }
    if (to == THREAD_STATE_READY) {
        thread_note_ready_by(thread, __builtin_return_address(0));
    }
    return 1;
}

/* Unconditional-target state write: moves the thread to `state` from WHATEVER it
 * is in now, provided that edge is legal.  Reports nothing — an unknown tid and
 * a rejected edge are both silent, so a caller that needs to know whether the
 * change happened must use thread_transit instead.  `reason` is written only
 * alongside an accepted change, so a rejected call leaves the old reason intact. */
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
        if (state == THREAD_STATE_READY) {
            thread_note_ready_by(thread, __builtin_return_address(0));
        }
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
    thread_note_ready_by(thread, __builtin_return_address(0));
    ksync_spinlock_unlock(&g_thread_table_lock);
    return 1;
}

/* Records a thread's exit status.  Independent of the state machine: it neither
 * requires nor causes a transition, so the caller tombstones the thread
 * separately.  An unknown tid is a silent no-op. */
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

/* Releases a single thread slot: unlinks it from any run queue and scrubs it to
 * UNUSED, making it available to the next spawn.  Any pointer previously
 * obtained from thread_get for this tid is stale afterwards.  Unlike
 * thread_reap_owner this does not check the thread is terminal, so the caller
 * must have established that nothing will dispatch it — the join/detach paths
 * reap only a ZOMBIE, the spawn-abort paths reap a thread never published.
 * An unknown tid is a no-op. */
int thread_reap(uint32_t tid) {
    ksync_spinlock_lock(&g_thread_table_lock);
    thread_t* thread = thread_get_nolock(tid);
    if (!thread) {
        ksync_spinlock_unlock(&g_thread_table_lock);
        return 1; /* already gone: the caller's intent is satisfied */
    }
    int reaped = thread_reset_slot(thread);
    ksync_spinlock_unlock(&g_thread_table_lock);
    return reaped;
}

/* Points the CALLING CPU's current_thread at `tid`, or clears it for tid 0.  An
 * unknown tid also clears it, because thread_get answers 0 — so a stale tid
 * silently deconfigures the CPU rather than being reported.
 *
 * Resolves through thread_get, which takes g_thread_table_lock: this must not be
 * called with that lock held.  The dispatcher calls it inside
 * critical_section_enter/leave so the pid/process/thread trio is published as
 * one unit with respect to preemption. */
void thread_set_current(uint32_t tid) {
    if (tid == 0) {
        cpu_local()->current_thread = 0;
        return;
    }
    cpu_local()->current_thread = thread_get(tid);
}

/* Tid running on the CALLING CPU, or 0 when nothing is dispatched — which is the
 * normal state inside the scheduler itself and during early boot, not an error.
 * Lock-free: reads only this CPU's own cpu_local() slot. */
uint32_t thread_current_tid(void) {
    thread_t* thread = cpu_local()->current_thread;
    return thread ? thread->tid : 0;
}
