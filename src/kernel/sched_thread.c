#ifdef WASMOS_SCHED_THREADABLE

#include "sched.h"
#include "thread.h"
#include "process.h"
#include "spinlock.h"
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
static uint8_t g_high_prio_streak     = 0;

/* ffs_table[bitmap] = index of lowest set bit (highest priority), or 0xFF.
 * Covers all 128 valid 7-bit bitmap values. */
static const uint8_t ffs_table[128] = {
    0xFF, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
       4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
};

static inline int
cpu_sched_highest_prio(const cpu_sched_t *cs)
{
    uint8_t bm = cs->ready_bitmap & 0x7Fu;
    if (bm == 0) {
        return 0xFF;
    }
    return (int)ffs_table[bm];
}

static inline uint32_t
cpu_sched_online_mask(void)
{
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

static uint32_t
cpu_sched_load_on(uint32_t cpu_id)
{
    cpu_sched_t *cs = &g_cpus[cpu_id].sched;
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

void
cpu_sched_init(cpu_sched_t *cs)
{
    spinlock_init(&cs->lock);
    cs->ready_bitmap = 0;
    for (int i = 0; i < SCHED_PRIO_MAX; i++) {
        list_head_init(&cs->ready_list[i]);
        cs->thread_count[i] = 0;
    }
    cs->running     = 0;
    cs->idle        = 0;
    cs->nr_threads  = 0;
}

void
cpu_sched_enqueue(cpu_sched_t *cs, thread_t *t)
{
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        if (g_cpus[i].current_thread == t) {
            serial_printf_unlocked("[sched] enqueue current tid=%u owner=%u caller_cpu=%u holder_cpu=%u state=%u\n",
                                   (unsigned)t->tid,
                                   (unsigned)t->owner_pid,
                                   (unsigned)cpu_local()->cpu_id,
                                   (unsigned)i,
                                   (unsigned)t->state);
            /* Thread is still running on another CPU.  Mark it ready so the
             * owning CPU re-enqueues when its timeslice or blocking-yield
             * completes (see PROCESS_RUN_BLOCKED handler).  Never halt here
             * under production SMP IPC load. */
            t->state = THREAD_STATE_READY;
            t->block_reason = THREAD_BLOCK_NONE;
            return;
        }
    }
    spinlock_lock(&cs->lock);
    /* SMP wake/block races can reach enqueue from multiple CPUs for the same
     * READY thread.  sched_node self-links when detached from any list, so
     * use that as the single source of truth and drop duplicate inserts. */
    if (!list_head_empty(&t->sched_node)) {
        spinlock_unlock(&cs->lock);
        return;
    }
    uint8_t prio = t->sched_prio;
    list_head_add_tail(&cs->ready_list[prio], &t->sched_node);
    cs->thread_count[prio]++;
    cs->ready_bitmap |= (uint8_t)(1u << prio);
    spinlock_unlock(&cs->lock);
}

void
sched_enqueue_thread_from(thread_t *t, uintptr_t caller)
{
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        if (g_cpus[i].current_thread == t) {
            serial_printf_unlocked("[sched] enqueue current tid=%u owner=%u caller_cpu=%u holder_cpu=%u state=%u caller=%016llx\n",
                                   (unsigned)t->tid,
                                   (unsigned)t->owner_pid,
                                   (unsigned)cpu_local()->cpu_id,
                                   (unsigned)i,
                                   (unsigned)t->state,
                                   (unsigned long long)caller);
            t->state = THREAD_STATE_READY;
            t->block_reason = THREAD_BLOCK_NONE;
            return;
        }
    }
    cpu_sched_enqueue(cpu_sched(), t);
}

void
cpu_sched_dequeue(cpu_sched_t *cs, thread_t *t)
{
    /* Caller holds cs->lock. */
    uint8_t prio = t->sched_prio;
    list_head_del(&t->sched_node);
    if (--cs->thread_count[prio] == 0) {
        cs->ready_bitmap &= (uint8_t)(~(1u << prio));
    }
}

thread_t *
cpu_sched_pick_next(cpu_sched_t *cs)
{
    /* Caller holds cs->lock. */
    int prio = cpu_sched_highest_prio(cs);
    if (prio == 0xFF) {
        g_high_prio_streak     = 0;
        g_last_dispatched_prio = SCHED_PRIO_IDLE;
        /* Return the per-CPU idle thread.  Each CPU has its own, so no two
         * CPUs ever dispatch the same idle thread simultaneously. */
        return cpu_local()->idle_thread;
    }

    /* Anti-starvation: if we have dispatched SCHED_ANTISTARVATION_STREAK
     * threads at priority <= prio and a lower-priority band also has work,
     * yield one slot to that band.  This keeps higher-priority workers from
     * permanently starving the WASM services they cooperate with. */
    if ((int)g_last_dispatched_prio <= prio &&
        g_high_prio_streak >= SCHED_ANTISTARVATION_STREAK) {
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

    thread_t *t = list_first_entry(&cs->ready_list[prio], thread_t, sched_node);
    list_head_del(&t->sched_node);
    if (--cs->thread_count[prio] == 0) {
        cs->ready_bitmap &= (uint8_t)(~(1u << prio));
    }
    return t;
}

void
sched_set_need_resched(void)
{
    /* Delegate to the existing process.c resched flag. */
    extern void process_set_need_resched(void);
    process_set_need_resched();
}

void
sched_wake_thread(thread_t *t)
{
    if (!t) {
        return;
    }

    /* Single-CPU race: a sender can wake the thread after ipc_recv_for()
     * registered it but before the blocked yield path has finished saving
     * context and cleared blocking_transition.  Spinning here deadlocks the
     * current CPU.  Mark it READY and let the blocked-yield completion path
     * enqueue it once the transition finishes. */
    if (__atomic_load_n(&t->blocking_transition, __ATOMIC_ACQUIRE)) {
        t->state = THREAD_STATE_READY;
        t->block_reason = THREAD_BLOCK_NONE;
        return;
    }

    /* After the blocked-yield transition completes, only a true BLOCKED->READY
     * transition should enqueue the thread.  A stale remote wake that arrives
     * after the thread resumed RUNNING must be ignored. */
    if (!thread_wake_if_blocked(t->tid)) {
        return;
    }
    if (list_head_empty(&t->sched_node)) {
#if WASMOS_SCHED_CALLER_CPU_BIAS
        /* Pull the receiver onto the waker's CPU queue. */
        t->last_cpu = cpu_local()->cpu_id;
#endif
        /* Always enqueue locally — no remote spinlock in the hot IPC path.
         * With bias OFF, last_cpu is left as-is so threads stay on whatever
         * CPU they last ran on; work-stealing handles redistribution. */
        cpu_sched_enqueue(cpu_sched(), t);
    }
}

void
sched_thread_init(thread_t *t, sched_prio_t prio)
{
    t->ctx_canary_pre  = PROCESS_CTX_CANARY_VALUE;
    t->ctx_canary_post = PROCESS_CTX_CANARY_VALUE;
    t->sched_prio      = (uint8_t)prio;
    t->cpu_affinity    = ~0u;
    t->last_cpu        = 0;
    list_head_init(&t->sched_node);
    list_head_init(&t->event_node);
    sched_event_init(&t->join_event, SCHED_EVENT_TYPE_JOIN);
    t->wait_event = 0;
    t->pend_state = SCHED_PEND_NONE;
    t->pend_data  = 0;
}

sched_prio_t
sched_default_prio(int is_idle,
                   int is_kernel_worker,
                   int is_driver,
                   int is_native_service)
{
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

uint32_t
cpu_sched_pick_target_cpu(void)
{
    /* Round-robin counter: on ties (all CPUs equally loaded) we rotate the
     * starting search index so spawns spread evenly instead of always
     * accumulating on CPU 0. */
    static uint32_t g_spawn_rr = 0;
    uint32_t start     = g_spawn_rr % g_cpu_count;
    uint32_t best      = start;
    uint32_t best_load = UINT32_MAX;

    for (uint32_t n = 0; n < g_cpu_count; n++) {
        uint32_t i = (start + n) % g_cpu_count;
        uint32_t load = cpu_sched_load_on(i);
        if (load < best_load) {
            best_load = load;
            best      = i;
        }
    }
    g_spawn_rr++;
    return best;
}

uint32_t
cpu_sched_pick_target_cpu_for_thread(const thread_t *t, uint8_t prefer_last_cpu)
{
    uint32_t online_mask = cpu_sched_online_mask();
    uint32_t allowed_mask = online_mask;
    static uint32_t g_affine_rr = 0;

    if (t) {
        allowed_mask &= t->cpu_affinity;
        if (allowed_mask == 0u) {
            allowed_mask = online_mask;
        }
        if (prefer_last_cpu &&
            t->last_cpu < g_cpu_count &&
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

void
sched_spawn_thread(struct thread *t)
{
    uint32_t target = cpu_sched_pick_target_cpu_for_thread(t, 0);
    t->last_cpu = target;
    cpu_sched_enqueue(&g_cpus[target].sched, t);
}

struct thread *
cpu_sched_try_steal(uint32_t my_cpu_id)
{
    /* Start scan from the next CPU so each AP preferentially targets a
     * different victim, preventing all APs from racing over CPU 0's queue. */
    for (uint32_t n = 1; n < g_cpu_count; n++) {
        uint32_t i = (my_cpu_id + n) % g_cpu_count;
        if (i == my_cpu_id) {
            continue;
        }
        cpu_sched_t *remote = &g_cpus[i].sched;
        if (!remote->ready_bitmap) {
            continue;
        }
        if (!spinlock_try_lock(&remote->lock)) {
            continue;
        }
        struct thread *t = NULL;
        if (remote->ready_bitmap) {
            t = cpu_sched_pick_next(remote);
            if (t == remote->idle) {
                t = NULL;
            }
        }
        /* spinlock_try_lock does not call preempt_disable/spinlock_irq_save,
         * so we must release with the matching no-IRQ variant. */
        spinlock_unlock_noirq(&remote->lock);
        if (t) {
            t->last_cpu = my_cpu_id;
            cpu_local()->steal_count++;
            return t;
        }
    }
    return NULL;
}

#endif /* WASMOS_SCHED_THREADABLE */
