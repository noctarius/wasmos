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
#define SCHED_ANTISTARVATION_STREAK 4
static uint8_t g_last_dispatched_prio = SCHED_PRIO_IDLE;
static uint8_t g_high_prio_streak = 0;

/* ffs_table[bitmap] = index of lowest set bit (highest priority), or 0xFF.
 * Covers all 128 valid 7-bit bitmap values. */
static const uint8_t ffs_table[128] = {
    0xFF, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0,
    1,    0, 2, 0, 1, 0, 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0,
    2,    0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0,
    1,    0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5, 0, 1, 0, 2, 0, 1, 0,
    3,    0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
};

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

static uint32_t cpu_sched_load_on(uint32_t cpu_id) {
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
    cs->nr_threads = 0;
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
static void cpu_sched_unlink_locked(cpu_sched_t* cs, thread_t* t, uint8_t prio) {
    list_head_del(&t->sched_node);
    /* DIAGNOSTIC: after list_head_del the band must no longer reach this node.
     * If the head still points at it, this queue's chain was spliced through a
     * node whose neighbours belong to a different list -- the ghost that gets
     * re-picked on every dispatch.  Repair the head so the CPU is not wedged,
     * and report it: this fires at the moment of corruption, not minutes later
     * at the dispatch site. */
    if (cs->ready_list[prio].next == &t->sched_node) {
        static uint32_t ghost_seen;
        uint32_t gn = __atomic_fetch_add(&ghost_seen, 1u, __ATOMIC_RELAXED);
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
        cs->ready_bitmap &= (uint8_t)(~(1u << prio));
    }
    t->rq = 0;
    /* Release the claim last: an enqueuer spinning on the exchange must not be
     * able to start linking this node until the unlink above has retired. */
    __atomic_store_n(&t->on_rq, 0, __ATOMIC_RELEASE);
}

void cpu_sched_enqueue(cpu_sched_t* cs, thread_t* t) {
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
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
                t->block_reason = THREAD_BLOCK_NONE;
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
    if (t->state != THREAD_STATE_READY) {
        static uint32_t bad_enqueue_seen;
        uint32_t n = __atomic_fetch_add(&bad_enqueue_seen, 1u, __ATOMIC_RELAXED);
        if ((n & (n - 1u)) == 0u) {
            serial_printf_unlocked("[sched] enqueue non-ready tid=%u owner=%u state=%u block=%u "
                                   "caller=%016llx (n=%u, skipped)\n",
                                   (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state,
                                   (unsigned)t->block_reason,
                                   (unsigned long long)(uintptr_t)__builtin_return_address(0),
                                   (unsigned)(n + 1u));
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
    ksync_spinlock_lock(&cs->lock);
    if (__atomic_exchange_n(&t->on_rq, 1, __ATOMIC_ACQ_REL)) {
        ksync_spinlock_unlock(&cs->lock);
        return; /* already queued somewhere */
    }
    uint8_t prio = t->sched_prio;
    /* DIAGNOSTIC: holding the claim, this node MUST be detached -- every unlink
     * releases the claim only after list_head_del has retired.  A linked node
     * here means someone unlinked without releasing, or the node is still in
     * another queue, i.e. we are one instruction from splicing two lists through
     * it.  Refuse the link rather than corrupt the queue, and name the caller. */
    if (!list_head_empty(&t->sched_node)) {
        static uint32_t double_link_seen;
        uint32_t dn = __atomic_fetch_add(&double_link_seen, 1u, __ATOMIC_RELAXED);
        if ((dn & (dn - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] claimed node still linked tid=%u owner=%u state=%u prio=%u rq=%p "
                "cs=%p caller=%016llx (n=%u)\n",
                (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state, (unsigned)prio,
                (void*)t->rq, (void*)cs, (unsigned long long)(uintptr_t)__builtin_return_address(0),
                (unsigned)(dn + 1u));
        }
        /* Release the claim we just took before bailing.  Returning while still
         * holding it would strand the thread: no queue holds it, and every later
         * enqueue would lose the exchange and drop the insert forever. */
        __atomic_store_n(&t->on_rq, 0, __ATOMIC_RELEASE);
        ksync_spinlock_unlock(&cs->lock);
        return;
    }
    t->rq = cs;
    list_head_add_tail(&cs->ready_list[prio], &t->sched_node);
    cs->thread_count[prio]++;
    cs->ready_bitmap |= (uint8_t)(1u << prio);
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
        if (t->rq == cs) {
            cpu_sched_unlink_locked(cs, t, t->sched_prio);
            ksync_spinlock_unlock(&cs->lock);
            return;
        }
        ksync_spinlock_unlock(&cs->lock);
    }
    serial_printf_unlocked("[sched] remove_thread gave up tid=%u owner=%u state=%u\n",
                           (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state);
}

void sched_enqueue_thread_from(thread_t* t, uintptr_t caller) {
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        if (g_cpus[i].current_thread == t) {
            serial_printf_unlocked("[sched] enqueue current tid=%u owner=%u caller_cpu=%u "
                                   "holder_cpu=%u state=%u caller=%016llx\n",
                                   (unsigned)t->tid, (unsigned)t->owner_pid,
                                   (unsigned)cpu_local()->cpu_id, (unsigned)i, (unsigned)t->state,
                                   (unsigned long long)caller);
            if (sched_mark_ready_if_live(t)) {
                t->block_reason = THREAD_BLOCK_NONE;
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
        static uint32_t bad_from_seen;
        uint32_t n = __atomic_fetch_add(&bad_from_seen, 1u, __ATOMIC_RELAXED);
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
    cpu_sched_unlink_locked(cs, t, t->sched_prio);
}

thread_t* cpu_sched_pick_next(cpu_sched_t* cs) {
    /* Caller holds cs->lock. */
    int prio = cpu_sched_highest_prio(cs);
    if (prio == 0xFF) {
        g_high_prio_streak = 0;
        g_last_dispatched_prio = SCHED_PRIO_IDLE;
        /* Return the per-CPU idle thread.  Each CPU has its own, so no two
         * CPUs ever dispatch the same idle thread simultaneously. */
        return cpu_local()->idle_thread;
    }

    /* Anti-starvation: if we have dispatched SCHED_ANTISTARVATION_STREAK
     * threads at priority <= prio and a lower-priority band also has work,
     * yield one slot to that band.  This keeps higher-priority workers from
     * permanently starving the WASM services they cooperate with. */
    if ((int)g_last_dispatched_prio <= prio && g_high_prio_streak >= SCHED_ANTISTARVATION_STREAK) {
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
            g_high_prio_streak = 0;
        } else {
            g_high_prio_streak++;
        }
    } else if ((int)g_last_dispatched_prio <= prio) {
        g_high_prio_streak++;
    } else {
        g_high_prio_streak = 0;
    }
    g_last_dispatched_prio = (uint8_t)prio;

    /* Lazy per-CPU sweep: walk this band and DROP any node whose thread is no
     * longer READY (reaped -> UNUSED, or tombstoned -> ZOMBIE).  A thread is
     * only ever marked non-READY while it is off this queue, but a reap can
     * reset/zombie a still-enqueued sibling; dropping it here (under our own
     * cs->lock) is the sole mechanism needed to keep such nodes off the
     * dispatcher — no cross-CPU removal, no reaper touching our queue.  Returns
     * the first genuinely-READY thread, or idle if the band held only stale
     * nodes. */
    list_head_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &cs->ready_list[prio]) {
        thread_t* t = list_entry(pos, thread_t, sched_node);
        cpu_sched_unlink_locked(cs, t, (uint8_t)prio);
        uint32_t st = __atomic_load_n((uint32_t*)&t->state, __ATOMIC_ACQUIRE);
        if (t->tid == 0 || st == THREAD_STATE_UNUSED || st == THREAD_STATE_ZOMBIE) {
            continue;
        }
        return t;
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
     * our half of the handshake before reading the completion path's. */
    if (!sched_wake_claim_enqueue(t)) {
        /* Completion path owns the enqueue; leave it something to enqueue. */
        if (sched_mark_ready_if_live(t)) {
            t->block_reason = THREAD_BLOCK_NONE;
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

    /* Priority preemption: if we just made a thread runnable that outranks what
     * this CPU is currently running, request a reschedule so it preempts at the
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
    t->sched_prio = (uint8_t)prio;
    t->cpu_affinity = ~0u;
    t->last_cpu = 0;
    /* A slot handed back by the allocator must not still be linked into a ready
     * queue: re-initialising the node here would self-link it while that queue
     * still points at it, splicing the list through a node two owners now
     * mutate.  thread_reset_slot -> cpu_sched_remove_thread guarantees the
     * unlink happened; clearing the claim keeps the fresh incarnation
     * enqueueable. */
    /* DIAGNOSTIC: re-initialising sched_node here while the thread is still
     * linked into a ready queue self-links the node under the queue's nose --
     * the head keeps pointing at it, and the band is then spliced through a node
     * with two owners (the "ghost head" report).  Name the call site so we know
     * WHICH spawn path handed back a still-queued thread. */
    if (!list_head_empty(&t->sched_node) || __atomic_load_n(&t->on_rq, __ATOMIC_ACQUIRE)) {
        static uint32_t init_linked_seen;
        uint32_t in = __atomic_fetch_add(&init_linked_seen, 1u, __ATOMIC_RELAXED);
        if ((in & (in - 1u)) == 0u) {
            serial_printf_unlocked(
                "[sched] init on queued tid=%u owner=%u state=%u on_rq=%u oldprio=%u newprio=%u "
                "rq=%p linked=%u caller=%016llx (n=%u)\n",
                (unsigned)t->tid, (unsigned)t->owner_pid, (unsigned)t->state, (unsigned)t->on_rq,
                (unsigned)t->sched_prio, (unsigned)prio, (void*)t->rq,
                (unsigned)(!list_head_empty(&t->sched_node)),
                (unsigned long long)(uintptr_t)__builtin_return_address(0), (unsigned)(in + 1u));
        }
        /* Unlink properly instead of orphaning the node under the queue. */
        cpu_sched_remove_thread(t);
    }
    t->on_rq = 0;
    t->rq = 0;
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
    /* Round-robin counter: on ties (all CPUs equally loaded) we rotate the
     * starting search index so spawns spread evenly instead of always
     * accumulating on CPU 0. */
    static uint32_t g_spawn_rr = 0;
    uint32_t start = g_spawn_rr % g_cpu_count;
    uint32_t best = start;
    uint32_t best_load = UINT32_MAX;

    for (uint32_t n = 0; n < g_cpu_count; n++) {
        uint32_t i = (start + n) % g_cpu_count;
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
    static uint32_t g_affine_rr = 0;

    if (t) {
        allowed_mask &= t->cpu_affinity;
        if (allowed_mask == 0u) {
            allowed_mask = online_mask;
        }
        if (prefer_last_cpu && t->last_cpu < g_cpu_count &&
            (allowed_mask & (1u << t->last_cpu)) != 0u) {
            return t->last_cpu;
        }
    }

    uint32_t start = (g_cpu_count > 0u) ? (g_affine_rr % g_cpu_count) : 0u;
    uint32_t best = 0u;
    uint32_t best_load = UINT32_MAX;
    for (uint32_t n = 0; n < g_cpu_count; ++n) {
        uint32_t cpu_id = (start + n) % g_cpu_count;
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
static thread_t* cpu_sched_steal_pick(cpu_sched_t* cs) {
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
                cpu_sched_unlink_locked(cs, t, (uint8_t)prio);
                continue;
            }
            if (t == cs->idle || t->sched_sticky) {
                continue;
            }
            cpu_sched_unlink_locked(cs, t, (uint8_t)prio);
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
        if (!remote->ready_bitmap) {
            continue;
        }
        if (!ksync_spinlock_try_lock(&remote->lock)) {
            continue;
        }
        struct thread* t = NULL;
        if (remote->ready_bitmap) {
            t = cpu_sched_steal_pick(remote);
        }
        /* ksync_spinlock_try_lock does not call preempt_disable/spinlock_irq_save,
         * so we must release with the matching no-IRQ variant. */
        ksync_spinlock_unlock_noirq(&remote->lock);
        if (t) {
            t->last_cpu = my_cpu_id;
            cpu_local()->steal_count++;
            return t;
        }
    }
    return NULL;
}
