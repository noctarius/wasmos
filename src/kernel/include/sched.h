#ifndef WASMOS_SCHED_H
#define WASMOS_SCHED_H

/* Distinct result codes for process_schedule_once so the caller can tell a
 * normal re-schedule (a thread ran and blocked/exited/yielded) from a genuine
 * "could not dispatch even the idle thread" fallthrough (a panic-worthy bug).
 * 0 = dispatched a thread that voluntarily yielded (still runnable). */
enum {
    SCHED_OK = 0,         /* dispatched; thread YIELDED (still ready)        */
    SCHED_R_MAXCOUNT = 1, /* PROCESS_MAX_COUNT == 0 (never in practice)      */
    SCHED_R_PICK = 2,     /* not even idle was dispatchable (a real bug)     */
    SCHED_R_NOTREADY = 3, /* picked thread not in READY state                */
    SCHED_R_CTX = 4,      /* run context missing                            */
    SCHED_R_ROOT = 5,     /* target root page table missing                 */
    SCHED_R_ZOMBIE = 6,   /* owning process is zombie/exiting                */
    SCHED_R_RANDONE = 7,  /* dispatched; thread BLOCKED/EXITED (normal loop) */
    SCHED_R_STALE = 8,    /* non-idle thread reaped mid-flight (normal race)  */
    SCHED_R_CLAIMED = 9,  /* another CPU won the READY->RUNNING claim (race)  */
};

#include <stdint.h>
#include "sync/spinlock.h"
#include "sched_list.h"

/* Anti-starvation: once a priority band (or higher) has been dispatched this
 * many times consecutively, the scheduler yields one slot to the next occupied
 * lower band, so high-priority workers cannot permanently starve the services
 * they cooperate with.  Declared here rather than in sched_thread.c so tests
 * assert against the same constant the policy uses.
 *
 * The realised donation cycle is STREAK+2 dispatches, not STREAK+1: the dispatch
 * that returns to the high band after a donation takes the
 * last_dispatched_prio > prio arm in cpu_sched_pick_next, which resets the
 * streak rather than counting itself as the first of the new run.  So the high
 * band receives STREAK+1 consecutive slots per donated slot.  This is measured
 * and pinned by the A1/A3 cases in tests/unit/test_sched_runqueue.c, which
 * derive their expectations from this constant.  Deliberate: that same arm
 * cannot distinguish "returning from a donation" from "a genuinely higher band
 * just arrived", where resetting is correct, and tightening the cycle changes
 * fairness under sustained load. */
#define SCHED_ANTISTARVATION_STREAK 4

#define SCHED_PRIO_MAX 7  /* number of priority levels */
#define SCHED_PRIO_BITS 7 /* bitmask width; one bit per level */

typedef enum {
    SCHED_PRIO_REALTIME = 0,   /* IRQ handler workers, timer callbacks */
    SCHED_PRIO_DRIVER = 1,     /* native drivers */
    SCHED_PRIO_SERVICE = 2,    /* system services (device-manager, fs-manager) */
    SCHED_PRIO_SYSTEM = 3,     /* kernel services (process-manager, chardev) */
    SCHED_PRIO_WASM = 4,       /* default WASM processes */
    SCHED_PRIO_BACKGROUND = 5, /* batch / background jobs */
    SCHED_PRIO_IDLE = 6,       /* idle process only */
} sched_prio_t;

struct thread;

/* One CPU's run queue. Every field is guarded by ->lock EXCEPT the two the
 * enqueue path claims first: a thread's membership is decided by the atomic
 * thread_t::on_rq exchange before any queue lock is taken, because the claiming
 * CPU cannot know in advance which queue's lock protects the node. */
typedef struct cpu_sched_s {
    ksync_spinlock_t lock;
    uint8_t ready_bitmap;                   /* bit i set ↔ ready_list[i] non-empty */
    list_head_t ready_list[SCHED_PRIO_MAX]; /* one FIFO per priority */
    /* Threads linked into each band. Maintained against thread_t::rq_prio, the
     * band a node actually joined -- never against its current sched_prio. */
    uint32_t thread_count[SCHED_PRIO_MAX];
    struct thread* running; /* currently executing thread */
    /* This CPU's idle thread. Read from other CPUs (load accounting and
     * steal refusal), so leaving it NULL on an AP makes that CPU look busy and
     * its idle thread stealable. */
    struct thread* idle;
    /* Anti-starvation bookkeeping.  PER-CPU, and only correct that way: it
     * describes what THIS CPU has been dispatching.  A global counter would
     * race (plain bytes written by every CPU under different locks), advance N
     * times too fast on an N-CPU machine so the demotion fires far more often
     * than SCHED_ANTISTARVATION_STREAK implies, and let one CPU's dispatches
     * decide another's band. */
    uint8_t last_dispatched_prio;
    uint8_t high_prio_streak;
} cpu_sched_t;

/* Initialise the per-CPU scheduler state (called once per CPU at boot). */
void cpu_sched_init(cpu_sched_t* cs);

/* Enqueue a READY thread.  Sends a reschedule hint if the new thread
 * has higher priority than the currently running one.
 * Takes cs->lock itself, so the caller must hold no queue lock. Idempotent
 * through the on_rq claim: a thread already queued anywhere is dropped rather
 * than double-linked. Idle threads and non-READY threads are refused and
 * counted via sched_debug_note. Prefer sched_enqueue_thread, which routes every
 * dispatch through one place. */
void cpu_sched_enqueue(cpu_sched_t* cs, struct thread* t);

/* Remove a thread from whichever priority bucket it is in.
 * Caller must hold cs->lock. */
void cpu_sched_dequeue(cpu_sched_t* cs, struct thread* t);

/* Remove a thread from whatever per-CPU run-queue currently holds it, if any.
 * Locates the owning queue and takes its cs->lock itself; idempotent (no-op if
 * the thread is not enqueued).  Used by the reap path (which cannot know which
 * CPU last enqueued the thread) to guarantee a reaped thread is unlinked before
 * its slot is reset. */
void cpu_sched_remove_thread(struct thread* t);

/* Return the highest-priority ready thread, or cs->idle if none.
 * Caller must hold cs->lock.  Unlinks the thread it returns, so the caller owns
 * it and must either dispatch it or re-enqueue it -- the anti-starvation
 * bookkeeping above is also advanced by this call. */
struct thread* cpu_sched_pick_next(cpu_sched_t* cs);

/* Take exclusive ownership of `t` for a dispatch on the calling CPU, as the
 * atomic READY -> RUNNING edge.  Returns 1 to the single CPU that may proceed to
 * context_switch into t->ctx and 0 to every other caller.
 *
 * cpu_sched_pick_next unlinks under cs->lock and releases on_rq, but the
 * dispatcher does not name the thread in current_thread until later, so between
 * those two points the thread is READY, on no queue and claimed by no CPU:
 * cpu_sched_enqueue accepts it, a second CPU picks it, and both reach dispatch.
 * This edge is the exclusion.  Two CPUs resuming one process_context_t run on
 * one kernel stack and resume a torn rip.
 *
 * The loser must NOT re-queue the thread: the winner owns it and re-enqueues or
 * parks it when its dispatch ends. */
int cpu_sched_claim_for_dispatch(struct thread* t);

/* Mark the current CPU as needing a reschedule. */
void sched_set_need_resched(void);

/* Wake a blocked thread: wait for its blocking_transition to clear,
 * set state READY, and enqueue it.
 * Enqueues on the CALLING CPU's queue and takes only that lock. Silently does
 * nothing when the thread is not BLOCKED (a stale wake arriving after it
 * already resumed) or when the wake/block handshake hands the enqueue to the
 * thread's own completion path -- in which case the state change is still made
 * so that path has something to enqueue. Never blocks. */
void sched_wake_thread(struct thread* t);

/* Perform an enqueue that cpu_sched_enqueue deferred while this thread was some
 * CPU's current_thread.  Called by the dispatcher once it stops naming the
 * thread, on the CPU that was holding it.  Does nothing when no enqueue is owed,
 * when another CPU already claimed the debt, or when the thread is no longer
 * runnable -- so it is safe to call after every dispatch and safe to call twice.
 *
 * Pair with cpu_sched_enqueue: exactly one of the two sides links the thread. */
void sched_settle_deferred_enqueue(struct thread* t);

/* Enqueue anything left owed, called by a CPU that has found nothing to run.
 * Covers the one ordering settle cannot: a claim published just after the
 * holding CPU looked for one.  Costs a single load when nothing is owed, which
 * is every dispatch but a handful.  With it, the machine cannot go idle while a
 * runnable thread sits on no run queue. */
void sched_sweep_owed_enqueues(void);

/* Drop any outstanding owed-enqueue claim on `t` WITHOUT enqueuing it, and
 * subtract its debt.  For a slot being released to the allocator
 * (thread_reset_slot); every other consumer of a claim wants to act on it. */
void sched_drop_owed_enqueue(struct thread* t);

/* Scheduler tripwires, as counters rather than only log lines.  Each tripwire
 * rate-limits its own logging to powers of two, so past the first few hits the
 * log cannot distinguish "fired" from "fired but suppressed"; the counters
 * always can.  They are global and monotonic, hence sched_debug_reset() for
 * tests that need a clean baseline. */
typedef enum {
    SCHED_DEBUG_GHOST_HEAD = 0,
    SCHED_DEBUG_ENQUEUE_IDLE,
    SCHED_DEBUG_BAD_PRIO,
    SCHED_DEBUG_ENQUEUE_NON_READY,
    SCHED_DEBUG_DOUBLE_LINK,
    SCHED_DEBUG_ENQUEUE_FROM_NON_READY,
    SCHED_DEBUG_INIT_ON_QUEUED,
    SCHED_DEBUG_REMOVE_GAVE_UP,
    SCHED_DEBUG_SET_PRIO_QUEUED,
    /* Enqueues refused because the thread was executing on some CPU.  A normal
     * outcome, not a defect, so its report is rate-limited and this counter is
     * the honest total. */
    SCHED_DEBUG_ENQUEUE_CURRENT,
    /* Ready transitions refused because the owning process was already exiting
     * or ZOMBIE.  Also a normal outcome: no caller holds anything that excludes
     * a concurrent kill/exit, so a sibling-requeue can always find the owner
     * gone.  It was a kpanic ("set_ready zombie") until the counter replaced it;
     * a fatal report of a race the scheduler is built to absorb turned a
     * survivable interleaving into a dead machine. */
    SCHED_DEBUG_SET_READY_EXITING,
    /* Dispatches refused for the same reason, at the other half of the same
     * transition pair.  process_set_running already reported this by returning
     * 0 -- "it raced to a terminal state and must NOT be dispatched" -- and its
     * callers already honoured that, so the kpanic one line above the return was
     * fatal about a case the code otherwise handled. */
    SCHED_DEBUG_SET_RUNNING_EXITING,
    SCHED_DEBUG_EVENT_COUNT
} sched_debug_event_t;

/* Record that `ev` fired; returns the count BEFORE this hit so callers can apply
 * the same power-of-two rate limit the in-file tripwires use.  Public so paths
 * outside sched_thread.c can report scheduler-contract violations through the
 * same counters. */
uint32_t sched_debug_note(sched_debug_event_t ev);
/* Times `ev` has fired since boot (or since the last reset). */
uint32_t sched_debug_count(sched_debug_event_t ev);
/* Zero every counter.  Also re-seeds the placement round-robin cursors, which
 * are otherwise unresettable statics that force placement tests to assert on
 * distribution rather than on the CPU actually chosen. */
void sched_debug_reset(void);

/* Assign a default scheduler priority based on process flags.  The flags are
 * tested in the order idle, kernel worker, driver, native service, so the first
 * one set wins; none set gives SCHED_PRIO_WASM. */
sched_prio_t sched_default_prio(int is_idle, int is_kernel_worker, int is_driver,
                                int is_native_service);

/*
 * Initialise the scheduler fields of a freshly spawned thread.
 * Must be called for every thread before its first enqueue.
 *
 * Sets the priority band, affinity (any CPU), the context canaries and the
 * detached run-queue linkage. The thread must NOT be queued: re-initialising a
 * linked node self-links it while a queue still points at it, which is
 * unrecoverable for that band. A queued thread is therefore unlinked first and
 * counted as SCHED_DEBUG_INIT_ON_QUEUED -- recovery for a caller bug, not a
 * supported way to re-band a thread. Takes no lock of its own (the recovery
 * unlink takes the owning queue's).
 */
void sched_thread_init(struct thread* t, sched_prio_t prio);

/*
 * Try to steal a ready thread from another CPU's queue.  Uses trylock to
 * avoid deadlock; returns NULL if no work is available or all remote queues
 * are busy.  my_cpu_id is the calling CPU's index into g_cpus[].
 */
struct thread* cpu_sched_try_steal(uint32_t my_cpu_id);

/*
 * Return the index of the CPU with the lightest ready-queue load.
 * Used at spawn time to distribute new processes across CPUs.
 */
uint32_t cpu_sched_pick_target_cpu(void);

/*
 * Choose a target CPU for a specific thread.
 * When prefer_last_cpu is non-zero and last_cpu is valid for the thread's
 * affinity mask, preserve that placement. Otherwise choose the lightest
 * allowed CPU.
 */
uint32_t cpu_sched_pick_target_cpu_for_thread(const struct thread* t, uint8_t prefer_last_cpu);

/*
 * Enqueue a freshly spawned thread on the least-loaded CPU and set
 * last_cpu accordingly.  Use only for the initial spawn enqueue; all
 * subsequent re-queues go through sched_enqueue_thread / sched_wake_thread.
 * The thread must already be READY and fully initialised by sched_thread_init;
 * takes the target queue's lock itself and does not block.
 */
void sched_spawn_thread(struct thread* t);

#endif /* WASMOS_SCHED_H */
