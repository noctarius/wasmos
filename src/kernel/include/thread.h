#ifndef WASMOS_THREAD_H
#define WASMOS_THREAD_H

#include <stdint.h>
#include "process.h"

#include "sched_list.h"
#include "sched_event.h"

/* Slots in the fixed global thread table, shared by ALL processes: this is a
 * system-wide ceiling, not a per-process one, and a spawn past it fails. */
#define THREAD_MAX_COUNT 128
/* Bytes of thread_t::name_storage, NUL included. A longer name is refused
 * (the spawn fails) rather than truncated. */
#define THREAD_NAME_MAX 64

/* Thread lifecycle. Legal edges, enforced by thread_transit and thread_set_state:
 *
 *   UNUSED -> NEW -> READY|BLOCKED
 *   READY|RUNNING|BLOCKED -> each other, or -> ZOMBIE
 *   ZOMBIE -> UNUSED   (the reaper only)
 *
 * ZOMBIE is monotonic, which is what makes "all of this process's threads are
 * zombie" a stable observation for the reap gate. thread_reset_slot is the one
 * writer that bypasses the table, and the only path that may reach UNUSED from
 * NEW (spawn abort) as well as from ZOMBIE. */
typedef enum {
    THREAD_STATE_UNUSED = 0, /* DEAD/free: slot reclaimable; zero-initialised = free. */
    THREAD_STATE_READY,      /* runnable; may or may not currently be on a run queue */
    THREAD_STATE_RUNNING,    /* dispatched on some CPU */
    THREAD_STATE_BLOCKED,    /* parked: on an event wait list, or spawned parked */
    THREAD_STATE_ZOMBIE,     /* exited, exit_status valid, awaiting reap */
    THREAD_STATE_NEW, /* claimed + initialising; never schedulable; sole source of ->READY/->BLOCKED
                       */
} thread_state_t;

typedef enum {
    THREAD_BLOCK_NONE = 0,
    THREAD_BLOCK_IPC,
    THREAD_BLOCK_WAIT_PROCESS, /* in process_wait; wait_target_pid names the child */
    THREAD_BLOCK_WAIT_THREAD,  /* in process_thread_join */
    THREAD_BLOCK_EVENT,        /* blocked on a sched_event_t wait */
} thread_block_reason_t;

/* One slot of the global thread table. Most fields are written by whichever CPU
 * currently owns the thread (its dispatcher, or the spawn path before the
 * thread is published) and read without a lock; the exceptions are called out
 * individually below. thread_get / thread_owner_tid_at take the table lock but
 * return a raw pointer, so a slot can in principle be reaped between the lookup
 * and the caller's use -- callers rely on holding a reason to believe the
 * thread is alive, exactly as with process_t. */
typedef struct thread {
    uint32_t tid;       /* 0 in a free slot; monotonic, not reused within a boot */
    uint32_t owner_pid; /* owning process; the address space is the process's */
    /* Written with a CAS by thread_transit, and under the table lock by
     * thread_set_state / thread_wake_if_blocked. Both reject illegal edges. */
    thread_state_t state;
    thread_block_reason_t block_reason; /* meaningful in THREAD_STATE_BLOCKED */
    /* Kernel worker: runs its own worker_entry on its own stack rather than the
     * process entry point, skips the process runtime_lock, and is exempt from
     * the saved-context validation the dispatcher applies to other threads. */
    uint8_t is_kernel_worker;
    uint8_t blocking_transition; /* RUNNING→BLOCKED in-progress (atomic) */
    uint8_t wake_pending;        /* waker's half of the wake/block Dekker (atomic) */
    /* This thread's own kernel stack, as higher-half VAs: [kstack_base,
     * kstack_top) usable, one unmapped guard page either side.
     * kstack_alloc_base_phys is the physical base of the whole allocation and
     * is what the reaper frees; kstack_pages counts usable pages only. Zero for
     * a main thread, which runs on the process's stack instead. */
    uintptr_t kstack_base;
    uintptr_t kstack_top;
    uintptr_t kstack_alloc_base_phys;
    uint32_t kstack_pages;
    uintptr_t worker_entry; /* process_thread_worker_entry_t; kernel workers only */
    void* worker_arg;       /* borrowed; passed through unchanged */
    /* Round-robin quantum. ticks_remaining is decremented by the timer tick on
     * the running CPU and reloaded from time_slice_ticks at dispatch; a zero
     * time_slice_ticks at dispatch is a scheduler invariant failure and panics.
     * ticks_total is the lifetime tick count `ps` reports. */
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    /* Dispatches of THIS thread, one per context_switch into it.  ticks_total
     * cannot serve as a progress signal in its place: it only advances when a
     * timer interrupt lands on the thread, so an event-driven service that runs
     * briefly and often stays at 0 for its whole life.  This moves every time
     * the thread runs, which is what makes "did it run between two snapshots?"
     * answerable -- the question a wedged machine turns on. */
    uint64_t dispatch_count;
    /* The context the scheduler actually saves and restores for this thread.
     * ctx.cs decides ring-0 (ret) versus ring-3 (iretq) resume. */
    process_context_t ctx;
    uint32_t wait_target_pid; /* child pid, while blocked in process_wait */
    uint32_t join_waiter_tid; /* tid joining this thread; 0 = none, so a second joiner
                               * can be refused with WASMOS_ERR_THREAD_BUSY */
    uint8_t detached;         /* unjoinable; reaped automatically on exit */
    int32_t exit_status;      /* valid from ZOMBIE onward */
    /* Saved per-CPU wasm3 heap binding, carried across a block so a thread that
     * resumes on a DIFFERENT CPU does not inherit that CPU's stale binding. */
    uint32_t wasm3_heap_bound_pid;
#ifdef WASMOS_WASM_RUNTIME_WARP
    /* Per-thread WARP ring-3 call state.  A ring-3 export call can block in a
     * hostcall (e.g. blocking IPC), yield, and later resume/migrate to a
     * different CPU, so this state must travel with the thread rather than live
     * in cpu_local.  Set just before the ring-3 IRET and consumed by the
     * WARP_RETURN syscall handler via __builtin_longjmp. */
    uint64_t warp_r3_old_cr3; /* CR3 to restore when the ring-3 call returns */
    uint8_t warp_r3_active;   /* 1 while a ring-3 call is in progress */
    void* warp_r3_jbuf[5];    /* setjmp checkpoint for WARP_RETURN */
#endif
    char name_storage[THREAD_NAME_MAX];
    const char* name; /* points into name_storage, or NULL in a free slot */
    /* Per-thread context canaries (moved from process_t).  Both must equal
     * PROCESS_CTX_CANARY_VALUE; the dispatcher and the preemption path check
     * them around ctx and panic on a mismatch rather than resuming. */
    uint64_t ctx_canary_pre;
    uint64_t ctx_canary_post;
    /* Scheduler priority and CPU placement. */
    uint8_t sched_prio;          /* SCHED_PRIO_* */
    uint8_t sched_sticky;        /* 1 if last run was a voluntary yield (poller);
                                  * work-stealing skips it so idle CPUs do not
                                  * thrash re-running it. Cleared on dispatch. */
    uint32_t cpu_affinity;       /* allowed CPU bitmask; ~0u = any */
    uint32_t last_cpu;           /* CPU where thread last ran */
    uint64_t sched_timeout_tick; /* deadline tick for a timed wait; 0 = none */
    /* Run-queue membership.  on_rq is the single atomic fact "this thread's
     * sched_node is linked into some ready_list", claimed by an exchange BEFORE
     * any queue lock is taken and released only AFTER the unlink completes under
     * the owning queue's lock.  Because per-CPU queues have per-CPU locks,
     * inferring membership from "is sched_node self-linked?" would read the
     * node under a *different* lock than the one protecting it; the claim
     * removes that cross-lock read.
     * rq names the owning queue so the reap path can unlink without knowing
     * which CPU last enqueued the thread.  Valid only while on_rq is 1. */
    uint8_t on_rq;
    struct cpu_sched_s* rq;
    /* The band this thread was actually LINKED into, recorded at enqueue under
     * the queue lock.  Unlink accounting must use this rather than sched_prio:
     * a priority change while queued would otherwise drain a band the node was
     * never in, leaking the real band's counter and leaving its ready bit set
     * over an empty list -- a bit cpu_sched_highest_prio then selects forever.
     * Valid only while on_rq is 1. */
    uint8_t rq_prio;
    /* Intrusive linkage for scheduler lists. */
    list_head_t sched_node; /* linkage in cpu_sched_t.ready_list */
    list_head_t event_node; /* linkage in sched_event_t.wait_list */
    /* Event blocking state.  wait_event is written with release/acquire atomics
     * because sched_timeout_fire loads it WITHOUT any event lock in order to
     * learn which lock to take, then re-validates under it. Cleared under the
     * event's lock by whichever waker detaches the thread. */
    sched_event_t* wait_event; /* event this thread is blocked on */
    uint32_t pend_state;       /* SCHED_PEND_* set by waker */
    uint64_t pend_data;        /* value from waker (futex retcode, etc.) */
    /* Waiters block here until this thread exits. join_waiter_tid above still
     * records WHO is joining, so that a second joiner can be refused. */
    sched_event_t join_event;
} thread_t;

/* Wake/block Dekker.  A waker on one CPU and the blocked-yield completion path
 * (PROCESS_RUN_BLOCKED in process_schedule_once_impl) can race around the same
 * thread; exactly one of them must enqueue it.  Both sides publish their own
 * flag BEFORE reading the other's, and both use seq_cst — which also supplies
 * the StoreLoad barrier that x86 does not give for free.  That makes "waker
 * sees blocking_transition set" and "completion sees wake_pending clear"
 * mutually exclusive, so the wake can never be dropped.  wake_pending is a
 * claim token: whoever swaps it to 0 owns the enqueue, so it also cannot be
 * done twice.
 *
 * Use these instead of open-coding the handshake — a site that reads the other
 * side's flag without publishing first strands the thread READY on no run
 * queue, and every CPU then idles forever with runnable work outstanding.
 *
 * sched_wake_claim_enqueue is the WAKER's half: returns 1 if this caller now
 * owns the enqueue and must perform it, 0 if the blocking thread's own
 * completion path will. sched_block_complete_claim is the completion half, run
 * once the context has been saved: returns 1 if a waker deferred an enqueue to
 * it, 0 if there is nothing pending. Neither blocks and neither touches
 * ->state; the caller does that. */
static inline int sched_wake_claim_enqueue(thread_t* t) {
    __atomic_store_n(&t->wake_pending, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t->blocking_transition, __ATOMIC_SEQ_CST)) {
        return 0; /* completion path will consume the token and enqueue */
    }
    return __atomic_exchange_n(&t->wake_pending, 0, __ATOMIC_SEQ_CST) ? 1 : 0;
}

static inline int sched_block_complete_claim(thread_t* t) {
    __atomic_store_n(&t->blocking_transition, 0, __ATOMIC_SEQ_CST);
    return __atomic_exchange_n(&t->wake_pending, 0, __ATOMIC_SEQ_CST) ? 1 : 0;
}

/* Reset the whole thread table to free slots. BSP only, once, from
 * process_init, before any other thread exists -- it is the one caller that
 * does not hold the table lock. */
void thread_init(void);
/* thread_spawn_in_owner with initial_state == THREAD_STATE_READY. */
int thread_spawn_main(uint32_t owner_pid, const char* name, uint32_t* out_tid);
/*
 * Claim and initialise a thread slot for owner_pid, publishing it in
 * initial_state (READY or BLOCKED) once fully built. Returns 0 with *out_tid
 * set, or -1 for owner_pid 0, a NULL out_tid, an exhausted table, or a name
 * longer than THREAD_NAME_MAX-1.
 *
 * The slot carries NO stack and NO scheduler state: the caller must allocate a
 * kernel stack and call sched_thread_init before the thread is enqueued. Spawn
 * BLOCKED whenever that setup follows, because a READY thread is a legal wake
 * target on every CPU and an enqueue landing before sched_thread_init corrupts
 * the run queue. Takes the thread-table lock; does not block.
 */
int thread_spawn_in_owner(uint32_t owner_pid, const char* name, thread_state_t initial_state,
                          thread_block_reason_t initial_reason, uint32_t* out_tid);
/* Look up by tid under the table lock, or NULL for tid 0, an unknown tid, or a
 * free slot. The pointer is returned without the lock, so it is valid only
 * while the caller has reason to believe the thread is alive. */
thread_t* thread_get(uint32_t tid);
/* Return the thread table slot at `index` (0..THREAD_MAX_COUNT-1), or NULL.
 * For table-wide scans (e.g. the scheduler timeout sweep). */
thread_t* thread_table_at(uint32_t index);
/* First live thread in table order belonging to owner_pid, or NULL. That is the
 * lowest-indexed slot, which is the main thread only while the process has not
 * reused a lower slot for a worker -- use process_t::main_tid where the main
 * thread specifically is meant. */
thread_t* thread_find_main_for_pid(uint32_t owner_pid);
/* Enumerate owner_pid's live threads by dense index, 0 upward, until -1.
 * Returns 0 with *out_tid set, or -1 for owner_pid 0, a NULL out_tid, or an
 * index past the end. Takes the table lock per call, so the set can change
 * between calls; every caller in-tree tolerates that. */
int thread_owner_tid_at(uint32_t owner_pid, uint32_t index, uint32_t* out_tid);
/* Tombstone every thread of owner_pid: record exit_status and drive each to
 * ZOMBIE through the state machine (idempotent for threads already there).
 * Does not free stacks or slots -- thread_reap_owner does that. */
void thread_mark_owner_exited(uint32_t owner_pid, int32_t exit_status);
/* Free every thread slot of owner_pid, unlinking each from whatever run queue
 * still holds it first. Called from the process reap path, which owns the
 * process slot exclusively. Lock order: takes the thread-table lock and, inside
 * it, the owning run queue's lock (via cpu_sched_remove_thread) -- so never
 * call it while holding a cpu_sched_t lock. */
void thread_reap_owner(uint32_t owner_pid);
/* Set state and block_reason under the thread-table lock. Illegal edges per the
 * thread state machine -- notably any attempt to leave ZOMBIE, which would
 * resurrect a thread the reaper is tearing down -- are silently dropped, as is
 * an unknown tid, so the caller learns nothing about what happened. Use
 * thread_transit instead wherever the outcome decides a run-queue side effect. */
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason);

/* Attempt an atomic (CAS) transition `from`->`to`, taking no lock. Returns 1 iff
 * the state was exactly `from` and is now `to`; 0 if another CPU changed it
 * first or the edge is illegal per the thread state machine. Writes only
 * ->state: callers set block_reason and perform run-queue side effects
 * themselves, and only on success. */
int thread_transit(thread_t* t, thread_state_t from, thread_state_t to);

/* Atomically move a thread BLOCKED->READY under the table lock. Returns 1 when
 * the state changed, 0 if the thread was not BLOCKED or not found. Does NOT
 * enqueue: the caller must do that when this returns 1. */
int thread_wake_if_blocked(uint32_t tid);
/* Record an exit status under the table lock without changing state. An
 * unknown tid is silently ignored. */
void thread_set_exit_status(uint32_t tid, int32_t exit_status);
/* Free one thread slot: unlinks it from any run queue, then resets it to
 * UNUSED. No state check -- it will reset a thread in ANY state, so callers
 * must already know the thread is finished (a zombie, or a spawn being aborted
 * before publication). Silently ignores an unknown tid. Same lock order as
 * thread_reap_owner: table lock, then the run queue's. */
void thread_reap(uint32_t tid);
/* Publish the calling CPU's current thread (tid 0 clears it). Resolves the tid
 * through the table, so an unknown tid leaves current_thread NULL. */
void thread_set_current(uint32_t tid);
/* tid of the thread running on the calling CPU, or 0 in scheduler context. */
uint32_t thread_current_tid(void);

#endif
