#include "sched.h"
#include "thread.h"
#include "process.h"
#include "sync/spinlock.h"
#include "serial.h"
#include "string.h"
#include "arch/x86_64/smp.h"

/*
 * sched_thread.c — per-CPU O(1) priority scheduler.
 *
 * One cpu_sched_t lives here (single-CPU; SMP would use an array).
 * Ready threads are held in SCHED_PRIO_MAX FIFO lists, one per priority.
 * The ready_bitmap has bit i set iff ready_list[i] is non-empty, enabling
 * O(1) highest-ready lookup via a small lookup table (ffs_table).
 */

/*
 * Anti-starvation: after this many consecutive dispatches from a given
 * priority band (or higher), the scheduler yields one slot to the next
 * occupied lower-priority band.  This prevents high-priority workers from
 * completely starving lower-priority WASM services they interact with.
 */

/* ffs_table[bitmap] = index of lowest set bit (highest priority), or 0xFF.
 * Covers all 128 valid 7-bit bitmap values. */
static const uint8_t ffs_table[128] = {
    0xFF, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0,
    1,    0, 2, 0, 1, 0, 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0,
    2,    0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0,
    1,    0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5, 0, 1, 0, 2, 0, 1, 0,
    3,    0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
};

static uint32_t g_sched_debug[SCHED_DEBUG_EVENT_COUNT];

/* Placement round-robin cursors.  File scope rather than function statics so
 * sched_debug_reset can re-seed them: as locals they made every tie-breaking
 * test depend on how many placements every earlier test had performed. */
static uint32_t g_spawn_rr = 0;
static uint32_t g_affine_rr = 0;

/* Returns the count BEFORE this hit, so callers keep the (n & (n-1)) == 0
 * power-of-two rate limit they already used with a local static. */
static uint32_t sched_debug_bump(sched_debug_event_t ev) {
    return __atomic_fetch_add(&g_sched_debug[ev], 1u, __ATOMIC_RELAXED);
}

void sched_debug_reset(void) {
    for (unsigned i = 0; i < SCHED_DEBUG_EVENT_COUNT; ++i) {
        __atomic_store_n(&g_sched_debug[i], 0u, __ATOMIC_RELAXED);
    }
    g_spawn_rr = 0;
    g_affine_rr = 0;
}

uint32_t sched_debug_note(sched_debug_event_t ev) {
    if ((unsigned)ev >= SCHED_DEBUG_EVENT_COUNT) {
        return 0;
    }
    return sched_debug_bump(ev);
}

uint32_t sched_debug_count(sched_debug_event_t ev) {
    if ((unsigned)ev >= SCHED_DEBUG_EVENT_COUNT) {
        return 0;
    }
    return __atomic_load_n(&g_sched_debug[ev], __ATOMIC_RELAXED);
}

static inline int cpu_sched_highest_prio(const cpu_sched_t* cs) {
    uint8_t bm = cs->ready_bitmap & 0x7Fu;
    if (bm == 0) {
        return 0xFF;
    }
    return (int)ffs_table[bm];
}

static inline uint32_t cpu_sched_online_mask(void) {
    uint32_t mask = 1u; /* BSP scheduler is initialized during process_init(). */
    uint32_t limit = g_cpu_count;
    if (limit > WASMOS_MAX_CPUS) {
        limit = WASMOS_MAX_CPUS;
    }
    for (uint32_t i = 1; i < limit && i < 32u; ++i) {
        if (g_cpus[i].started) {
            mask |= (1u << i);
        }
    }
    return mask;
}

/* Index of the CPU owning `cs`, or WASMOS_MAX_CPUS if the queue is not one of
 * g_cpus[].  Needed because enqueue is handed a queue, not a CPU id, while
 * affinity is expressed as a CPU mask.  The "not found" answer must be distinct
 * from CPU 0: a caller-owned queue (the in-kernel scheduler selftests build
 * cpu_sched_t on the stack) has no CPU to reason about, and folding it onto 0
 * would let an affinity mask that excludes CPU 0 redirect the thread away from
 * the queue the caller explicitly named. */
static uint32_t cpu_sched_cpu_index(const cpu_sched_t* cs) {
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        if (&g_cpus[i].sched == cs) {
            return i;
        }
    }
    return WASMOS_MAX_CPUS;
}

/* Is `t` allowed to run on `cpu_id`?  An empty intersection is treated as "no
 * constraint" rather than "never runnable", matching
 * cpu_sched_pick_target_cpu_for_thread: a mask naming only offline CPUs must not
 * strand the thread forever. */
static int cpu_sched_affinity_allows(const thread_t* t, uint32_t cpu_id) {
    if (!t || cpu_id >= WASMOS_MAX_CPUS) {
        return 1;
    }
    if ((t->cpu_affinity & cpu_sched_online_mask()) == 0u) {
        return 1;
    }
    return (t->cpu_affinity & (1u << cpu_id)) != 0u;
}

/* CPUs placement may consider: g_cpu_count clamped to the table it indexes, and
 * never zero.  g_cpu_count comes from the MADT scan and is not validated
 * elsewhere, while both placement entry points derive a loop bound AND a modulus
 * from it -- so a count of 0 divides by zero and a count above WASMOS_MAX_CPUS
 * walks off g_cpus[].  The BSP always exists, so flooring at 1 is safe. */
static uint32_t cpu_sched_usable_cpus(void) {
    uint32_t n = g_cpu_count;
    if (n > WASMOS_MAX_CPUS) {
        n = WASMOS_MAX_CPUS;
    }
    if (n == 0u) {
        n = 1u;
    }
    return n;
}

static uint32_t cpu_sched_load_on(uint32_t cpu_id) {
    if (cpu_id >= WASMOS_MAX_CPUS) {
        return UINT32_MAX; /* out of the table: never the lightest */
    }
    cpu_sched_t* cs = &g_cpus[cpu_id].sched;
    uint32_t load = 0;
    for (int p = 0; p < SCHED_PRIO_MAX; p++) {
        load += cs->thread_count[p];
    }
    /* Count the currently running non-idle thread as part of this CPU's
     * load so placement prefers truly idle CPUs first. */
    if (g_cpus[cpu_id].current_thread && g_cpus[cpu_id].current_thread != cs->idle) {
        load++;
    }
    return load;
}

void cpu_sched_init(cpu_sched_t* cs) {
    ksync_spinlock_init(&cs->lock);
    cs->ready_bitmap = 0;
    for (int i = 0; i < SCHED_PRIO_MAX; i++) {
        list_head_init(&cs->ready_list[i]);
        cs->thread_count[i] = 0;
    }
    cs->running = 0;
    cs->idle = 0;
    cs->last_dispatched_prio = SCHED_PRIO_IDLE;
    cs->high_prio_streak = 0;
}

/* Mark a thread READY from a LIVE state (RUNNING/BLOCKED) via the thread state
 * machine, without ever resurrecting a ZOMBIE/UNUSED/NEW slot.  Used by the
 * lockless wake/enqueue race paths below: the CAS inside thread_transit makes
 * the read-decide-write atomic, so a concurrent reaper's ->ZOMBIE always wins
 * over this ->READY (preserving ZOMBIE monotonicity / the reap gate).  Returns
 * 1 if the thread is now READY. */
static int sched_mark_ready_if_live(thread_t* t) {
    uint32_t cur = __atomic_load_n((uint32_t*)&t->state, __ATOMIC_ACQUIRE);
    if (cur == THREAD_STATE_READY) {
        return 1;
    }
    if (cur != THREAD_STATE_RUNNING && cur != THREAD_STATE_BLOCKED) {
        return 0; /* ZOMBIE / UNUSED / NEW — never resurrect */
    }
    return thread_transit(t, (thread_state_t)cur, THREAD_STATE_READY);
}

/* Unlink a thread from the queue that owns it and release its run-queue claim.
 * Caller holds cs->lock and must have established that t is linked in cs.
 *
 * The band's ready bit is derived from list emptiness rather than from the
 * counter reaching zero.  The counter is a statistic (used for load balancing);
 * the list is the truth.  Deriving the bit means a counter that has drifted --
 * historically by underflowing past zero, which wedged the picker on a band
 * whose bit could never clear again -- cannot stop the band from going idle. */
static void cpu_sched_unlink_locked(cpu_sched_t* cs, thread_t* t) {
    /* The band comes from the thread's recorded linkage, never from its current
     * sched_prio, so mutating the priority while queued cannot misdirect the
     * accounting.  Taking it as a parameter invited exactly that mistake. */
    uint8_t prio = t->rq_prio;
    if (prio >= SCHED_PRIO_MAX) {
        prio = 0;
    }
    list_head_del(&t->sched_node);
    /* DIAGNOSTIC: after list_head_del the band must no longer reach this node.
     * If the head still points at it, this queue's chain was spliced through a
     * node whose neighbours belong to a different list -- the ghost that gets
     * re-picked on every dispatch.  Repair the head so the CPU is not wedged,
     * and report it: this fires at the moment of corruption, not minutes later
     * at the dispatch site. */
    if (cs->ready_list[prio].next == &t->sched_node) {
        uint32_t gn = sched_debug_bump(SCHED_DEBUG_GHOST_HEAD);
        if ((gn & (gn - 1u)) == 0u) {
            serial_printf_unlocked("[sched] ghost head tid=%u owner=%u state=%u prio=%u cs=%p "
                                   "count=%u (n=%u)\n",
                                   (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state,
                                   (unsigned)prio, (void*)cs, (unsigned)cs->thread_count[prio],
                                   (unsigned)(gn + 1u));
        }
        /* Report only.  Re-initialising the head here drops every other thread
         * in the band on the floor, which faults in list_head_add_tail shortly
         * after; the livelock is the lesser evil while diagnosing. */
    }
    if (cs->thread_count[prio] > 0) {
        cs->thread_count[prio]--;
    }
    if (list_head_empty(&cs->ready_list[prio])) {
        __atomic_store_n(&cs->ready_bitmap, (uint8_t)(cs->ready_bitmap & ~(1u << prio)),
                         __ATOMIC_RELAXED);
    }
    /* Released atomically: cpu_sched_remove_thread follows t->rq WITHOUT holding
     * any queue lock (it cannot know which lock to take until it has read it),
     * so a plain store here races that read.  Benign in practice -- the value is
     * re-validated under the lock -- but undefined by the memory model and a
     * genuine ThreadSanitizer report. */
    __atomic_store_n(&t->rq, (struct cpu_sched_s*)0, __ATOMIC_RELEASE);
    /* Release the claim last: an enqueuer spinning on the exchange must not be
     * able to start linking this node until the unlink above has retired. */
    __atomic_store_n(&t->on_rq, 0, __ATOMIC_RELEASE);
}

void cpu_sched_enqueue(cpu_sched_t* cs, thread_t* t) {
    /* A NULL thread must not reach the current_thread scan below: a CPU that
     * has not dispatched yet holds current_thread == NULL, so NULL == NULL
     * matches and the "still running elsewhere" path dereferences it. */
    if (!t) {
        return;
    }
    /* One pass over the per-CPU table answers both questions that disqualify a
     * thread from being linked: it is some CPU's idle thread, or it is still
     * running somewhere.  Kept as a single loop because this is the hot wake
     * path -- a second scan would double its cost for no benefit. */
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        /* Idle threads are dispatched exclusively through the
         * cpu_sched_pick_next fallback and are never queued; linking one would
         * let a CPU dispatch it from the ready list while it is simultaneously
         * that CPU's fallback. */
        if (g_cpus[i].idle_thread == t) {
            uint32_t n = sched_debug_bump(SCHED_DEBUG_ENQUEUE_IDLE);
            if ((n & (n - 1u)) == 0u) {
                serial_printf_unlocked(
                    "[sched] enqueue idle tid=%u caller=%016llx (n=%u, skipped)\n",
                    (unsigned)t->tid, (unsigned long long)(uintptr_t)__builtin_return_address(0),
                    (unsigned)(n + 1u));
            }
            return;
        }
        if (g_cpus[i].current_thread == t) {
            serial_printf_unlocked(
                "[sched] enqueue current tid=%u owner=%u caller_cpu=%u holder_cpu=%u state=%u\n",
                (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)cpu_local()->cpu_id,
                (unsigned)i, (unsigned)t->state);
            /* Thread is still running on another CPU.  Mark it ready so the
             * owning CPU re-enqueues when its timeslice or blocking-yield
             * completes (see PROCESS_RUN_BLOCKED handler).  Never halt here
             * under production SMP IPC load. */
            if (sched_mark_ready_if_live(t)) {
                __atomic_store_n((uint32_t*)&t->block_reason, THREAD_BLOCK_NONE, __ATOMIC_RELAXED);
            }
            return;
        }
    }
    /* Invariant: only a READY thread belongs in a ready queue.  Every caller is
     * required to have promoted it first -- sched_wake_thread via
     * thread_wake_if_blocked, the PROCESS_RUN_BLOCKED completion path via its
     * state re-read.  Checked here as well as at dispatch because the dispatch
     * report says only where the corpse was found; this one names who carried
     * it in.  Log-only on purpose: refusing the enqueue could strand a thread on
     * no run queue, which is worse than the violation being reported.
     * Rate-limited and unlocked for the same reasons as the dispatch-side
     * report.  Direct callers (sched_wake_thread) are identified by return
     * address; anything routed through sched_enqueue_thread_from is reported
     * there instead, with its true call site.
     *
     * The insert is SKIPPED rather than forced through.  Enqueueing on an
     * unsettled state is what produces the pathology this check exists to catch:
     * a non-READY thread parked in a ready queue is re-picked and re-rejected on
     * every scheduling attempt.  Declining leaves the thread where it already
     * is, and the ordinary wake path (thread_wake_if_blocked: BLOCKED -> READY ->
     * enqueue) re-enqueues it once the picture is stable.  The one case that
     * would strand a thread is a caller that has already consumed its wake token
     * and treats this call as its last chance -- the Dekker completion path in
     * process_schedule_once_impl -- but that path enqueues only after reading
     * READY itself, so reaching here means the state moved on and a later wake
     * owns it. */
    /* sched_prio indexes ready_list[] and thread_count[] and shifts into
     * ready_bitmap, so a value past the last band is two out-of-bounds writes
     * plus a bit that cpu_sched_highest_prio masks away and can never clear.
     * Unreachable while every caller goes through the sched_prio_t enum, which
     * is exactly why nothing catches it if one ever does not. */
    if (t->sched_prio >= SCHED_PRIO_MAX) {
        uint32_t n = sched_debug_bump(SCHED_DEBUG_BAD_PRIO);
        if ((n & (n - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] enqueue bad prio tid=%u prio=%u caller=%016llx (n=%u, skipped)\n",
                (unsigned)t->tid, (unsigned)t->sched_prio,
                (unsigned long long)(uintptr_t)__builtin_return_address(0), (unsigned)(n + 1u));
        }
        return;
    }
    /* Atomic load: state is published by thread_transit's CAS from other CPUs,
     * and the pick_next/steal sweeps already read it atomically.  This guard was
     * the one plain reader left. */
    uint32_t state = __atomic_load_n((uint32_t*)&t->state, __ATOMIC_ACQUIRE);
    if (state != THREAD_STATE_READY) {
        uint32_t n = sched_debug_bump(SCHED_DEBUG_ENQUEUE_NON_READY);
        if ((n & (n - 1u)) == 0u) {
            /* Report the values already loaded rather than re-reading the fields:
             * a diagnostic is still a reader, and re-reading state here would
             * both race the writer and be able to print a value that never
             * failed the test above. */
            uint32_t reason = __atomic_load_n((uint32_t*)&t->block_reason, __ATOMIC_RELAXED);
            serial_printf_unlocked(
                "[sched] enqueue non-ready tid=%u owner=%u state=%u block=%u "
                "caller=%016llx (n=%u, skipped)\n",
                (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)state, (unsigned)reason,
                (unsigned long long)(uintptr_t)__builtin_return_address(0), (unsigned)(n + 1u));
        }
        return;
    }
    /* SMP wake/block races can reach enqueue from multiple CPUs for the same
     * READY thread.  The on_rq exchange -- not the node's linkage -- is the
     * serialisation point: each CPU's ready_list is protected by that CPU's own
     * lock, so testing linkage under cs->lock would be reading state owned by a
     * different lock (the queue the thread is actually in, or a remote
     * pick_next/steal unlinking it right now).  The claim only succeeds once the
     * previous owner's unlink has retired and released it, so no two CPUs ever
     * touch this node's pointers at once.
     *
     * The claim is taken INSIDE cs->lock, immediately before t->rq is published.
     * Taking it before the lock leaves a window in which on_rq is 1 but rq is
     * still 0, and cpu_sched_remove_thread -- which follows rq -- reads that as
     * "not queued" and clears the claim out from under this enqueue.  The node
     * then lands in the queue unclaimed, and the next enqueue on any CPU links
     * the same node a second time.  Inside the lock the window is a few
     * instructions with interrupts disabled, so no reap can observe or preempt
     * it on this CPU, and a remote one only ever spins briefly (see
     * cpu_sched_remove_thread). */
    /* Affinity is enforced HERE because this is the single funnel every enqueue
     * passes through.  Both sched_wake_thread and the PROCESS_RUN_BLOCKED
     * completion path call cpu_sched_enqueue(cpu_sched(), t) -- i.e. the CALLING
     * CPU's queue -- which would otherwise park a thread on a CPU its mask
     * forbids, silently overriding the placement that
     * cpu_sched_pick_target_cpu_for_thread computed at spawn.  Redirect rather
     * than refuse: dropping the enqueue would strand a runnable thread. */
    uint32_t target_cpu = cpu_sched_cpu_index(cs);
    if (target_cpu < WASMOS_MAX_CPUS && !cpu_sched_affinity_allows(t, target_cpu)) {
        cs = &g_cpus[cpu_sched_pick_target_cpu_for_thread(t, 1)].sched;
    }
    ksync_spinlock_lock(&cs->lock);
    if (__atomic_exchange_n(&t->on_rq, 1, __ATOMIC_ACQ_REL)) {
        ksync_spinlock_unlock(&cs->lock);
        return; /* already queued somewhere */
    }
    uint8_t prio = t->sched_prio;
    /* DIAGNOSTIC: holding the claim, this node MUST be detached -- every unlink
     * releases the claim only after list_head_del has retired.  A linked node
     * here means someone unlinked without releasing, or the node is still in
     * another queue -- one instruction away from splicing two lists through it.
     * Refuse the link rather than corrupt the queue, and name the caller. */
    if (!list_head_empty(&t->sched_node)) {
        uint32_t dn = sched_debug_bump(SCHED_DEBUG_DOUBLE_LINK);
        if ((dn & (dn - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] claimed node still linked tid=%u owner=%u state=%u prio=%u rq=%p "
                "cs=%p caller=%016llx (n=%u)\n",
                (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state, (unsigned)prio,
                (void*)t->rq, (void*)cs, (unsigned long long)(uintptr_t)__builtin_return_address(0),
                (unsigned)(dn + 1u));
        }
        /* Release the claim taken above before bailing.  Returning while still
         * holding it would strand the thread: no queue holds it, and every later
         * enqueue would lose the exchange and drop the insert forever. */
        __atomic_store_n(&t->on_rq, 0, __ATOMIC_RELEASE);
        ksync_spinlock_unlock(&cs->lock);
        return;
    }
    t->rq_prio = prio;
    __atomic_store_n(&t->rq, cs, __ATOMIC_RELEASE); /* see cpu_sched_unlink_locked */
    list_head_add_tail(&cs->ready_list[prio], &t->sched_node);
    cs->thread_count[prio]++;
    __atomic_store_n(&cs->ready_bitmap, (uint8_t)(cs->ready_bitmap | (1u << prio)),
                     __ATOMIC_RELAXED);
    ksync_spinlock_unlock(&cs->lock);
}

void cpu_sched_remove_thread(thread_t* t) {
    if (!t) {
        return;
    }
    /* The reap path cannot know which CPU last enqueued the thread, so follow
     * t->rq and re-validate under that queue's lock.  A concurrent pick_next or
     * steal may unlink it first; re-read and retry rather than unlinking against
     * a stale queue.  One iteration is the norm: a thread being reaped is
     * already terminal and nothing legitimately re-enqueues it.
     *
     * on_rq is the authority for "queued at all", and it is NEVER written here.
     * Writing it would clobber a claim an enqueue on another CPU is holding
     * across its rq publication, leaving that node linked but unclaimed and
     * therefore linkable a second time.  A (on_rq=1, rq=0) reading is an enqueue
     * in flight, not a leaked claim -- the enqueuer publishes rq a few
     * instructions later under its queue lock -- so spin briefly rather than
     * "correcting" it. */
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!__atomic_load_n(&t->on_rq, __ATOMIC_ACQUIRE)) {
            return; /* genuinely on no queue */
        }
        cpu_sched_t* cs = (cpu_sched_t*)__atomic_load_n(&t->rq, __ATOMIC_ACQUIRE);
        if (!cs) {
            cpu_relax(); /* enqueue in flight; let it publish rq */
            continue;
        }
        ksync_spinlock_lock(&cs->lock);
        if (__atomic_load_n(&t->rq, __ATOMIC_ACQUIRE) == cs) {
            cpu_sched_unlink_locked(cs, t);
            ksync_spinlock_unlock(&cs->lock);
            return;
        }
        ksync_spinlock_unlock(&cs->lock);
    }
    (void)sched_debug_bump(SCHED_DEBUG_REMOVE_GAVE_UP);
    serial_printf_unlocked("[sched] remove_thread gave up tid=%u owner=%u state=%u\n",
                           (unsigned)t->tid, (unsigned)t->owner_pid,
                           (unsigned)__atomic_load_n((uint32_t*)&t->state, __ATOMIC_RELAXED));
}

void sched_enqueue_thread_from(thread_t* t, uintptr_t caller) {
    /* A NULL thread must not reach the current_thread scan below: a CPU that
     * has not dispatched yet holds current_thread == NULL, so NULL == NULL
     * matches and the "still running elsewhere" path dereferences it. */
    if (!t) {
        return;
    }
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        if (g_cpus[i].current_thread == t) {
            serial_printf_unlocked("[sched] enqueue current tid=%u owner=%u caller_cpu=%u "
                                   "holder_cpu=%u state=%u caller=%016llx\n",
                                   (unsigned)t->tid, (unsigned)t->owner_pid,
                                   (unsigned)cpu_local()->cpu_id, (unsigned)i, (unsigned)t->state,
                                   (unsigned long long)caller);
            if (sched_mark_ready_if_live(t)) {
                __atomic_store_n((uint32_t*)&t->block_reason, THREAD_BLOCK_NONE, __ATOMIC_RELAXED);
            }
            return;
        }
    }
    /* DIAGNOSTIC: same check as cpu_sched_enqueue, done here because this path
     * knows the ORIGINAL call site.  cpu_sched_enqueue can only report its
     * immediate caller, which for everything routed through here is just this
     * function -- useless for telling one sched_enqueue_thread() site from
     * another. */
    if (t->state != THREAD_STATE_READY) {
        uint32_t n = sched_debug_bump(SCHED_DEBUG_ENQUEUE_FROM_NON_READY);
        if ((n & (n - 1u)) == 0u) {
            serial_printf_unlocked("[sched] enqueue_from non-ready tid=%u owner=%u state=%u "
                                   "block=%u caller=%016llx (n=%u)\n",
                                   (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state,
                                   (unsigned)t->block_reason, (unsigned long long)caller,
                                   (unsigned)(n + 1u));
        }
    }
    cpu_sched_enqueue(cpu_sched(), t);
}

void cpu_sched_dequeue(cpu_sched_t* cs, thread_t* t) {
    /* Caller holds cs->lock. */
    cpu_sched_unlink_locked(cs, t);
}

thread_t* cpu_sched_pick_next(cpu_sched_t* cs) {
    /* Caller holds cs->lock. */
    int prio = cpu_sched_highest_prio(cs);
    if (prio == 0xFF) {
        cs->high_prio_streak = 0;
        cs->last_dispatched_prio = SCHED_PRIO_IDLE;
        /* Return the per-CPU idle thread.  Each CPU has its own, so no two
         * CPUs ever dispatch the same idle thread simultaneously. */
        return cpu_local()->idle_thread;
    }

    /* Anti-starvation: once SCHED_ANTISTARVATION_STREAK threads at priority
     * <= prio have been dispatched and a lower-priority band also has work,
     * yield one slot to that band.  This keeps higher-priority workers from
     * permanently starving the WASM services they cooperate with. */
    if ((int)cs->last_dispatched_prio <= prio &&
        cs->high_prio_streak >= SCHED_ANTISTARVATION_STREAK) {
        /* Find the next lower occupied priority. */
        int lower_prio = -1;
        for (int p = prio + 1; p < SCHED_PRIO_MAX; p++) {
            if (cs->ready_bitmap & (1u << p)) {
                lower_prio = p;
                break;
            }
        }
        if (lower_prio >= 0) {
            prio = lower_prio;
            cs->high_prio_streak = 0;
        } else {
            cs->high_prio_streak++;
        }
    } else if ((int)cs->last_dispatched_prio <= prio) {
        cs->high_prio_streak++;
    } else {
        cs->high_prio_streak = 0;
    }
    cs->last_dispatched_prio = (uint8_t)prio;

    /* Lazy per-CPU sweep: walk this band and DROP any node whose thread is no
     * longer READY (reaped -> UNUSED, or tombstoned -> ZOMBIE).  A thread is
     * only ever marked non-READY while it is off this queue, but a reap can
     * reset/zombie a still-enqueued sibling; dropping it here (under this CPU's
     * own cs->lock) is the sole mechanism needed to keep such nodes off the
     * dispatcher — no cross-CPU removal, no reaper touching the queue.  Returns
     * the first genuinely-READY thread, or idle if the band held only stale
     * nodes. */
    /* Sweep in priority order from `prio` down.  A band that turns out to hold
     * only stale nodes must NOT send us to idle while a lower band has runnable
     * work: the sweep drops those nodes and clears the band's bit, so recomputing
     * the highest occupied band converges and terminates. */
    for (;;) {
        /* A PHANTOM bit -- band marked occupied over an already-empty list -- has
         * nothing for the sweep to drain, so a picker that only advances by
         * draining would select it again on the next pass and forever after,
         * returning idle while lower bands hold runnable work.  Clear it here so
         * the bit cannot outlive its list however it got out of step. */
        if (list_head_empty(&cs->ready_list[prio])) {
            cs->ready_bitmap &= (uint8_t)(~(1u << prio));
            cs->thread_count[prio] = 0;
        }
        list_head_t *pos, *tmp;
        list_for_each_safe(pos, tmp, &cs->ready_list[prio]) {
            thread_t* t = list_entry(pos, thread_t, sched_node);
            cpu_sched_unlink_locked(cs, t);
            uint32_t st = __atomic_load_n((uint32_t*)&t->state, __ATOMIC_ACQUIRE);
            if (t->tid == 0 || st == THREAD_STATE_UNUSED || st == THREAD_STATE_ZOMBIE) {
                continue;
            }
            return t;
        }
        int next = cpu_sched_highest_prio(cs);
        if (next == 0xFF || next == prio) {
            break; /* nothing left anywhere, or the band failed to drain */
        }
        prio = next;
    }
    return cpu_local()->idle_thread;
}

void sched_set_need_resched(void) {
    /* Delegate to the existing process.c resched flag. */
    extern void process_set_need_resched(void);
    process_set_need_resched();
}

void sched_wake_thread(thread_t* t) {
    if (!t) {
        return;
    }

    /* Promote first, then claim: sched_wake_claim_enqueue (thread.h) publishes
     * the wake half of the handshake before reading the completion path's. */
    if (!sched_wake_claim_enqueue(t)) {
        /* Completion path owns the enqueue; leave it something to enqueue. */
        if (sched_mark_ready_if_live(t)) {
            __atomic_store_n((uint32_t*)&t->block_reason, THREAD_BLOCK_NONE, __ATOMIC_RELAXED);
        }
        return;
    }

    /* After the blocked-yield transition completes, only a true BLOCKED->READY
     * transition should enqueue the thread.  A stale remote wake that arrives
     * after the thread resumed RUNNING must be ignored. */
    if (!thread_wake_if_blocked(t->tid)) {
        return;
    }
#if WASMOS_SCHED_CALLER_CPU_BIAS
    /* Pull the receiver onto the waker's CPU queue. */
    t->last_cpu = cpu_local()->cpu_id;
#endif
    /* Always enqueue locally — no remote spinlock in the hot IPC path.  With
     * bias OFF, last_cpu is left as-is so threads stay on whatever CPU they last
     * ran on; work-stealing handles redistribution.  No "already queued?" test
     * here: cpu_sched_enqueue's on_rq claim is the only safe one, because this
     * CPU does not hold the lock of whichever queue would hold the thread. */
    cpu_sched_enqueue(cpu_sched(), t);

    /* Priority preemption: when the thread just made runnable outranks what this
     * CPU is currently running, request a reschedule so it preempts at the
     * next preemption point (typically IRQ return) instead of waiting for the
     * running thread's time slice to expire.  Lower sched_prio == higher band.
     * This is what lets a driver woken by its device IRQ (e.g. the serial
     * driver on RX) run promptly and drain the hardware before it overruns,
     * rather than sitting ready behind an equal-or-lower-priority app. */
    thread_t* cur = cpu_local()->current_thread;
    if (!cur || (uint8_t)t->sched_prio < cur->sched_prio) {
        sched_set_need_resched();
    }
}

void sched_thread_init(thread_t* t, sched_prio_t prio) {
    t->ctx_canary_pre = PROCESS_CTX_CANARY_VALUE;
    t->ctx_canary_post = PROCESS_CTX_CANARY_VALUE;
    /* The new priority is adopted AFTER the unlink below, not before.
     * cpu_sched_remove_thread accounts the unlink against t->sched_prio, so
     * overwriting it first drains the wrong band: the node leaves the old list
     * (list_head_del works regardless) while the new band's counter and ready
     * bit are adjusted instead.  The old band is then left with its bit set over
     * an empty list, which cpu_sched_highest_prio keeps selecting forever while
     * the sweep finds nothing -- the CPU returns idle on every dispatch with
     * runnable work outstanding in lower bands.  Same shape as the storm,
     * reached through the path that exists to prevent corruption. */
    /* A slot handed back by the allocator must not still be linked into a ready
     * queue: re-initialising the node here would self-link it while that queue
     * still points at it, splicing the list through a node two owners now
     * mutate.  thread_reset_slot -> cpu_sched_remove_thread guarantees the
     * unlink happened; clearing the claim keeps the fresh incarnation
     * enqueueable. */
    /* DIAGNOSTIC: re-initialising sched_node here while the thread is still
     * linked into a ready queue self-links the node under the queue's nose --
     * the head keeps pointing at it, and the band is then spliced through a node
     * with two owners (the "ghost head" report).  Name the call site to identify
     * WHICH spawn path handed back a still-queued thread. */
    if (!list_head_empty(&t->sched_node) || __atomic_load_n(&t->on_rq, __ATOMIC_ACQUIRE)) {
        uint32_t in = sched_debug_bump(SCHED_DEBUG_INIT_ON_QUEUED);
        if ((in & (in - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] init on queued tid=%u owner=%u state=%u on_rq=%u oldprio=%u newprio=%u "
                "rq=%p linked=%u caller=%016llx (n=%u)\n",
                (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state, (unsigned)t->on_rq,
                (unsigned)t->sched_prio, (unsigned)prio, (void*)t->rq,
                (unsigned)(!list_head_empty(&t->sched_node)),
                (unsigned long long)(uintptr_t)__builtin_return_address(0), (unsigned)(in + 1u));
        }
        /* Unlink properly instead of orphaning the node under the queue.  Still
         * carrying the OLD priority, so the correct band is drained. */
        cpu_sched_remove_thread(t);
    }
    t->sched_prio = (uint8_t)prio;
    t->cpu_affinity = ~0u;
    t->last_cpu = 0;
    t->on_rq = 0;
    t->rq = 0;
    t->rq_prio = 0;
    list_head_init(&t->sched_node);
    list_head_init(&t->event_node);
    sched_event_init(&t->join_event, SCHED_EVENT_TYPE_JOIN);
    t->wait_event = 0;
    t->pend_state = SCHED_PEND_NONE;
    t->pend_data = 0;
}

sched_prio_t sched_default_prio(int is_idle, int is_kernel_worker, int is_driver,
                                int is_native_service) {
    if (is_idle) {
        return SCHED_PRIO_IDLE;
    }
    if (is_kernel_worker) {
        return SCHED_PRIO_SYSTEM;
    }
    if (is_driver) {
        return SCHED_PRIO_DRIVER;
    }
    if (is_native_service) {
        return SCHED_PRIO_SERVICE;
    }
    return SCHED_PRIO_WASM;
}

uint32_t cpu_sched_pick_target_cpu(void) {
    /* Round-robin counter: on ties (all CPUs equally loaded) the starting search
     * index rotates so spawns spread evenly instead of always
     * accumulating on CPU 0. */
    uint32_t cpus = cpu_sched_usable_cpus();
    uint32_t start = g_spawn_rr % cpus;
    uint32_t best = start;
    uint32_t best_load = UINT32_MAX;

    for (uint32_t n = 0; n < cpus; n++) {
        uint32_t i = (start + n) % cpus;
        uint32_t load = cpu_sched_load_on(i);
        if (load < best_load) {
            best_load = load;
            best = i;
        }
    }
    g_spawn_rr++;
    return best;
}

uint32_t cpu_sched_pick_target_cpu_for_thread(const thread_t* t, uint8_t prefer_last_cpu) {
    uint32_t online_mask = cpu_sched_online_mask();
    uint32_t allowed_mask = online_mask;

    if (t) {
        allowed_mask &= t->cpu_affinity;
        if (allowed_mask == 0u) {
            allowed_mask = online_mask;
        }
        /* Bounded by the clamped count, not g_cpu_count: the shift below is
         * undefined once last_cpu reaches the width of the mask. */
        if (prefer_last_cpu && t->last_cpu < cpu_sched_usable_cpus() &&
            (allowed_mask & (1u << t->last_cpu)) != 0u) {
            return t->last_cpu;
        }
    }

    uint32_t cpus = cpu_sched_usable_cpus();
    uint32_t start = g_affine_rr % cpus;
    uint32_t best = 0u;
    uint32_t best_load = UINT32_MAX;
    for (uint32_t n = 0; n < cpus; ++n) {
        uint32_t cpu_id = (start + n) % cpus;
        if ((allowed_mask & (1u << cpu_id)) == 0u) {
            continue;
        }
        uint32_t load = cpu_sched_load_on(cpu_id);
        if (load < best_load) {
            best_load = load;
            best = cpu_id;
        }
    }
    g_affine_rr++;
    return best;
}

void sched_spawn_thread(struct thread* t) {
    uint32_t target = cpu_sched_pick_target_cpu_for_thread(t, 0);
    t->last_cpu = target;
    cpu_sched_enqueue(&g_cpus[target].sched, t);
}

/* Steal-specific picker: find the highest-priority thread in this queue that is
 * stealable, i.e. not the idle thread and not sched_sticky (a thread whose last
 * run was a voluntary yield — likely a poll/yield loop that should stay on its
 * home CPU rather than be re-run by every idle CPU).  Caller holds cs->lock.
 * Unlike cpu_sched_pick_next this does not touch the anti-starvation globals. */
static thread_t* cpu_sched_steal_pick(cpu_sched_t* cs, uint32_t to_cpu) {
    for (int prio = 0; prio < SCHED_PRIO_MAX; prio++) {
        if (!(cs->ready_bitmap & (1u << prio))) {
            continue;
        }
        list_head_t *pos, *tmp;
        list_for_each_safe(pos, tmp, &cs->ready_list[prio]) {
            thread_t* t = list_entry(pos, thread_t, sched_node);
            /* Lazy sweep: drop reaped/tombstoned stale nodes (see pick_next). */
            uint32_t st = __atomic_load_n((uint32_t*)&t->state, __ATOMIC_ACQUIRE);
            if (t->tid == 0 || st == THREAD_STATE_UNUSED || st == THREAD_STATE_ZOMBIE) {
                cpu_sched_unlink_locked(cs, t);
                continue;
            }
            if (t == cs->idle || t->sched_sticky) {
                continue;
            }
            /* Stealing moves the thread to another CPU, so it must be one the
             * thread is allowed to run on.  Without this, work stealing quietly
             * overrides every affinity decision the placement path made. */
            if (!cpu_sched_affinity_allows(t, to_cpu)) {
                continue;
            }
            cpu_sched_unlink_locked(cs, t);
            return t;
        }
    }
    return NULL;
}

struct thread* cpu_sched_try_steal(uint32_t my_cpu_id) {
    /* Start scan from the next CPU so each AP preferentially targets a
     * different victim, preventing all APs from racing over CPU 0's queue. */
    for (uint32_t n = 1; n < g_cpu_count; n++) {
        uint32_t i = (my_cpu_id + n) % g_cpu_count;
        if (i == my_cpu_id) {
            continue;
        }
        cpu_sched_t* remote = &g_cpus[i].sched;
        /* Deliberately unlocked: a cheap "is there anything here at all" probe
         * before paying for the trylock.  Relaxed rather than plain because the
         * owning CPU writes it under ITS lock, which this reader does not hold --
         * a stale answer is fine (the value is re-read under the lock below), a
         * torn or compiler-reordered one is not. */
        if (!__atomic_load_n(&remote->ready_bitmap, __ATOMIC_RELAXED)) {
            continue;
        }
        if (!ksync_spinlock_try_lock(&remote->lock)) {
            continue;
        }
        struct thread* t = NULL;
        if (remote->ready_bitmap) {
            t = cpu_sched_steal_pick(remote, my_cpu_id);
        }
        /* ksync_spinlock_try_lock does not call preempt_disable/spinlock_irq_save,
         * so the release must use the matching no-IRQ variant. */
        ksync_spinlock_unlock_noirq(&remote->lock);
        if (t) {
            /* Advisory placement hint, written after the remote lock is dropped
             * and read unlocked by the placement path -- relaxed keeps it a
             * defined race-free access without pretending it is synchronised. */
            __atomic_store_n(&t->last_cpu, my_cpu_id, __ATOMIC_RELAXED);
            cpu_local()->steal_count++;
            return t;
        }
    }
    return NULL;
}
