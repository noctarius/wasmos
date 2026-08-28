#ifndef WASMOS_PROCESS_H
#define WASMOS_PROCESS_H

#include <stdint.h>
#include <stddef.h>

#include "sync/spinlock.h"
#include "sched_event.h"
#include "wasmos_app.h"

#define PROCESS_MAX_COUNT 48 /* fixed g_processes[] slot count; spawn fails past it */
/* Bytes of process_t::name_storage, NUL included. A longer name is not
 * truncated silently: process_copy_name refuses it and the spawn fails. */
#define PROCESS_NAME_MAX 64
/* Timer ticks a thread runs before preemption. Bands are FIFO within a
 * priority level, so this is the round-robin quantum inside one band -- it does
 * not affect which band the scheduler picks. */
#define PROCESS_DEFAULT_SLICE_TICKS 5u
#define PROCESS_STACK_SIZE 524288u /* bytes of kernel stack per process */
/* Sentinel stored in thread_t::ctx_canary_pre/post either side of the saved
 * register context. Every dispatch and preemption of a non-worker thread
 * re-checks both; a mismatch is unrecoverable and panics ("ctx_canary_tripped")
 * rather than resuming a context that may have been overwritten. */
#define PROCESS_CTX_CANARY_VALUE 0xC0FFEE0DD15EA5EULL

/* Release all per-pid state owned by the active WASM runtimes for an exiting
 * process.  Called once from process_reap(), alongside wasm3_heap_release /
 * native_driver_heap_release.  Real impls live in the runtime TUs; the other
 * runtime build provides a no-op so common code can call them unconditionally. */
void warp_release_pid(uint32_t pid);
void wasm3_release_pid(uint32_t pid);

/* Saved execution state of one thread, written and read by context_switch /
 * context_switch_to in arch/x86_64/context_switch.S. The assembly addresses
 * every field by hard-coded byte displacement, which is why the asserts below
 * pin the layout: reordering a field silently reinterprets registers.
 *
 * `rsp` is the ring-0 (kernel) stack pointer; `user_rsp` is the ring-3 one and
 * matters only when cs says ring 3, in which case the resume goes through iretq
 * rather than the ret fast path. `root_table` is the physical CR3 value loaded
 * for this context; 0 means "not yet resolved" and the dispatcher fills it in
 * from the owning mm context. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs;
    uint64_t ss;
    uint64_t user_rsp;
    uint64_t root_table;
} process_context_t;

_Static_assert(offsetof(process_context_t, r15) == 0, "process_context_t r15 offset mismatch");
_Static_assert(offsetof(process_context_t, rdi) == 64, "process_context_t rdi offset mismatch");
_Static_assert(offsetof(process_context_t, rsp) == 120, "process_context_t rsp offset mismatch");
_Static_assert(offsetof(process_context_t, rip) == 128, "process_context_t rip offset mismatch");
_Static_assert(offsetof(process_context_t, rflags) == 136,
               "process_context_t rflags offset mismatch");
_Static_assert(offsetof(process_context_t, cs) == 144, "process_context_t cs offset mismatch");
_Static_assert(offsetof(process_context_t, ss) == 152, "process_context_t ss offset mismatch");
_Static_assert(offsetof(process_context_t, user_rsp) == 160,
               "process_context_t user_rsp offset mismatch");
_Static_assert(offsetof(process_context_t, root_table) == 168,
               "process_context_t root_table offset mismatch");

/* The register block an interrupt stub builds on the kernel stack: the GPRs it
 * pushed, followed by the five words the CPU itself pushed (rip, cs, rflags,
 * and — only on a privilege change — user_rsp/user_ss). Layout is pinned by the
 * asserts below because cpu_isr.S builds it by hand.
 *
 * process_preempt_from_irq both reads this frame (to snapshot the interrupted
 * thread) and REWRITES rip/cs/user_rsp/user_ss in place to redirect the iretq
 * into the scheduler trampoline. user_rsp/user_ss carry meaningful values only
 * when (cs & 3) == 3; a ring-0 interrupt leaves them as whatever the stub
 * captured, which is why the preempt path declines kernel-mode frames. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t user_ss;
} irq_frame_t;

_Static_assert(offsetof(irq_frame_t, r15) == 0, "irq_frame_t r15 offset mismatch");
_Static_assert(offsetof(irq_frame_t, rax) == 112, "irq_frame_t rax offset mismatch");
_Static_assert(offsetof(irq_frame_t, rip) == 120, "irq_frame_t rip offset mismatch");
_Static_assert(offsetof(irq_frame_t, user_rsp) == 144, "irq_frame_t user_rsp offset mismatch");

/* Process-slot lifecycle. Every write to process_t::state goes through the CAS
 * in process.c's process_transit, which rejects any edge not listed here:
 *
 *   UNUSED|DEAD -> NEW -> READY|RUNNING|BLOCKED|ZOMBIE
 *   READY|RUNNING|BLOCKED -> each other, or -> ZOMBIE
 *   ZOMBIE -> REAPING -> DEAD
 *
 * The three properties that fall out and that callers rely on: a free slot can
 * never jump straight to a schedulable state, ZOMBIE is monotonic (so "this
 * process is dead" never un-decides), and only the CPU that wins the
 * ZOMBIE->REAPING CAS frees the slot. UNUSED is produced only by the pristine
 * table init, never as a transition target. */
typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    /* Exited, exit_status valid, resources not yet released. Reported by `ps`
     * as "zmb" so unreaped children stay visible. */
    PROCESS_STATE_ZOMBIE,
    /* ALIVE covers READY/RUNNING/BLOCKED for code that only needs to
     * distinguish live vs. dead. */
    PROCESS_STATE_ALIVE = PROCESS_STATE_READY,
    /* REAPING: the reap claim is held by exactly one CPU, which is tearing the
     * slot down. Not schedulable, not claimable, and skipped by the `ps`
     * enumerators so a half-freed slot is never described. */
    PROCESS_STATE_REAPING = 6,
    /* NEW: slot claimed + initialising; never schedulable; sole target of a
     * free-slot claim (UNUSED/DEAD -> NEW) and sole source of ->LIVE. */
    PROCESS_STATE_NEW = 7,
    /* DEAD: fully reaped, resources freed, slot reclaimable. Distinct from
     * UNUSED (pristine, never allocated) but equivalent for claim purposes.
     * A generation ends here; the next allocation runs DEAD -> NEW. */
    PROCESS_STATE_DEAD = 8,
} process_state_t;

/* What a process entry point reports back to the dispatcher when it returns (or
 * passes to process_yield). It decides what the scheduler does with the thread
 * once control is back in scheduler context. */
typedef enum {
    /* Voluntary yield, still runnable: re-marked READY, re-enqueued, and
     * flagged sched_sticky so work-stealing leaves it on this CPU. */
    PROCESS_RUN_YIELDED = 0,
    /* Nothing to run (no current process or no entry point). Treated like a
     * yield by the dispatcher; the idle thread is never enqueued. */
    PROCESS_RUN_IDLE = 1,
    /* Parked: the thread already moved itself to THREAD_STATE_BLOCKED inside
     * sched_event_wait. The dispatcher only settles the wake/block handshake.
     * Legacy callers that return this WITHOUT having blocked are recovered as a
     * yield rather than being stranded. */
    PROCESS_RUN_BLOCKED = 2,
    /* The process is finished: marks it exited, tombstones every owner thread
     * and wakes its waiters. On a non-main kernel worker it retires just that
     * thread instead. */
    PROCESS_RUN_EXITED = 3,
    /* Only this thread is finished. Its joiner is woken; the process exits only
     * when this was the last live thread. */
    PROCESS_RUN_THREAD_EXITED = 4
} process_run_result_t;

/* Why a process is in PROCESS_STATE_BLOCKED. Advisory bookkeeping surfaced by
 * `ps` -- the authoritative per-thread reason is thread_t::block_reason. */
typedef enum {
    PROCESS_BLOCK_NONE = 0,
    PROCESS_BLOCK_IPC = 1,
    PROCESS_BLOCK_WAIT = 2 /* in process_wait or process_thread_join */
} process_block_reason_t;

struct process;
/* Main entry point of a kernel process. Invoked once per dispatch from the
 * scheduler trampoline (not once per process lifetime), so it must be written
 * as one iteration of an event loop and return a process_run_result_t saying
 * what to do next. `arg` is the pointer handed to process_spawn*, borrowed and
 * never freed by the kernel. */
typedef process_run_result_t (*process_entry_t)(struct process* process, void* arg);
/* Entry point of a kernel worker thread (process_thread_spawn_worker_internal).
 * Unlike process_entry_t it runs on the worker's own kernel stack and is
 * expected to loop internally; returning ends the thread. */
typedef process_run_result_t (*process_thread_worker_entry_t)(struct process* process, uint32_t tid,
                                                              void* arg);

/* One slot of the fixed g_processes[] table. A pointer to one stays valid only
 * while the caller has reason to believe the process is alive: once it is
 * reaped the slot is handed to an unrelated spawn. Nothing here is guarded by
 * a per-process lock; ->state is the only field written with a CAS, and the
 * table lock covers slot claiming alone. */
typedef struct process {
    uint32_t pid; /* 0 in a free slot; monotonic, never reused within a boot */
    uint32_t parent_pid;
    uint32_t context_id; /* mm/IPC/capability context; 0 once reaped */
    uint32_t main_tid;   /* thread created with the process; 0 while spawning */
    /* Threads ever created for this process, minus those already joined or
     * reaped, and how many of those are not yet ZOMBIE. Plain counters bumped
     * by the dispatcher and the spawn paths -- atomic only on the shared idle
     * process, which APs install into concurrently at bring-up. */
    uint32_t thread_count;
    uint32_t live_thread_count;
    /* Latched by process_mark_exited before the ZOMBIE transition, so it reads
     * 1 slightly ahead of ->state; the scheduler treats either as "do not
     * requeue". */
    /* Read cross-CPU by the scheduler's refusal guards while the exiting CPU
     * writes it, so EVERY access goes through __atomic_load_n/__atomic_store_n.
     * Mixing a plain write with an atomic read is a data race whether or not the
     * generated code happens to look right, and the compiler is entitled to
     * elide exactly the refusal those guards exist to make. */
    uint8_t exiting;
    process_state_t state;               /* written only via process.c's CAS */
    process_block_reason_t block_reason; /* meaningful in PROCESS_STATE_BLOCKED */
    /* Vestigial: only ever cleared. The live waiter bookkeeping is
     * thread_t::wait_target_pid, which process_wait sets and
     * process_wake_waiters matches against. */
    uint32_t wait_target_pid;
    int32_t exit_status; /* valid from ZOMBIE onward */
    /* Round-robin quantum template. The values the scheduler actually decrements
     * are the per-thread copies; these are what a freshly spawned thread starts
     * from. */
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    uint8_t is_idle;     /* the idle process; never enqueued, never reaped */
    uint8_t in_hostcall; /* set across a host call; suppresses IRQ preemption */
    /* Reap the zombie automatically once nothing is waiting on it, instead of
     * holding the slot for a process_wait / PROC_IPC_WAIT that will never come. */
    uint8_t auto_reap;
    /* A reap was requested and refused because a CPU was mid-dispatch of one of
     * this process's threads. The refusal is short-lived but its requester does
     * not necessarily come back -- process_reap_zombie_pid is a one-shot from the
     * PM -- so the flag makes the dispatch that caused the refusal responsible
     * for retrying it. Without it a refused reap strands the slot forever. */
    uint8_t reap_requested;
    uint8_t needs_runtime_lock;     /* take runtime_lock around every entry call */
    uint8_t ready;                  /* the child has announced readiness (notify_ready) */
    uint8_t require_explicit_ready; /* PM must not treat spawn alone as ready */
    char runtime_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1]; /* "KERNEL", "WARP", ... */
    /* Template context filled at spawn and copied into the main thread; also
     * refreshed from the interrupted frame on preemption. The context the
     * scheduler actually saves and restores is thread_t::ctx, and the canaries
     * that are checked are the thread's -- these two are initialised and then
     * left alone. */
    uint64_t ctx_canary_pre;
    process_context_t ctx;
    uint64_t ctx_canary_post;
    /* Kernel stack of the main thread, as higher-half VAs: [stack_base,
     * stack_top) is usable, with one unmapped guard page either side.
     * stack_alloc_base_phys is the physical base of the whole allocation
     * (guards included) and is what the reaper frees; stack_pages counts the
     * usable pages only. */
    uintptr_t stack_base;
    uintptr_t stack_top;
    uintptr_t stack_alloc_base_phys;
    uint32_t stack_pages;
    process_entry_t entry; /* NULL makes the process undispatchable */
    void* arg;             /* borrowed; passed to entry unchanged */
    char name_storage[PROCESS_NAME_MAX];
    const char* name; /* points into name_storage, or NULL in a free slot */
    /* Runtime reentrancy guard for subsystems that require single-threaded
     * process entry (currently the built-in WASM runtimes). Worker threads
     * (is_kernel_worker) never acquire this. */
    ksync_spinlock_t runtime_lock;
    uint32_t runtime_lock_owner; /* TID of current runtime-lock occupant; 0 = free */
    /* Initialised at spawn but currently unused: process_wait parks the calling
     * THREAD (process_set_blocked + thread_t::wait_target_pid) and
     * process_wake_waiters scans the thread table, so nothing waits on or
     * signals this event.
     * TODO: either route the parent-waits-on-child path through this event or
     * drop the field; as it stands it suggests a wake path that does not
     * exist. */
    sched_event_t wait_event;
    /* Transfer-buffer id (child-owned) holding this process's wasmos_spawn_info_t
     * header + args blob, or 0 if none. Returned by the wasmos_spawn_info_buffer()
     * hostcall so the child can read its startup contract. */
    uint32_t spawn_info_buffer_id;
} process_t;

/* Snapshot filled by process_info_at_stats for `ps`-style reporting. Every
 * field is sampled without a lock and can be stale by the time the caller reads
 * it; none of it is load-bearing for scheduling decisions. */
typedef struct {
    uint32_t state;        /* process_state_t as a plain word */
    uint32_t block_reason; /* process_block_reason_t as a plain word */
    /* NOT NUL-terminated: exactly WASMOS_APP_SUBSYSTEM_TAG_LEN bytes copied out
     * of the process's runtime_tag, which is one byte longer. */
    char runtime_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN];
    uint32_t thread_count;
    uint32_t live_thread_count;
    /* Thread running on the CALLING CPU if it belongs to this process, else 0 —
     * so a process running on another CPU reports 0 rather than its tid. */
    uint32_t current_tid;
    uint32_t context_id;
    uint64_t cpu_ticks;                 /* summed ticks_total over owner threads */
    uint64_t vm_total_bytes;            /* sum of the mm context's declared regions */
    uint64_t thread_kstack_total_bytes; /* main stack plus every worker kstack */
    uint64_t heap_committed_bytes;      /* wasm3 plus native-driver heaps */
    /* Currently a copy of vm_total_bytes; there is no resident-page accounting
     * yet (see the TODO in process_info_at_stats). */
    uint64_t rss_est_bytes;
    uint32_t last_cpu; /* CPU the MAIN thread last ran on; 0 if it is gone */
} process_stats_t;

/*
 * sched_enqueue_thread — enqueue a READY thread on the scheduler.
 * All call sites must use this instead of touching cpu_sched_t directly so
 * the dispatch point is a single place.
 *
 * Enqueues on the CALLING CPU's run queue and takes that queue's lock itself,
 * so the caller must hold no run-queue lock. Idempotent: the on_rq claim inside
 * cpu_sched_enqueue drops a thread that is already queued somewhere, which is
 * the only sound "already queued?" test available to a CPU that holds no lock
 * over the owning queue. A NULL thread, a thread that is not READY, or one
 * still current on some CPU is refused (and counted through sched_debug_note).
 * Never blocks. `caller` is only a diagnostic return address.
 */
struct thread;
#include "sched.h"
void sched_enqueue_thread_from(struct thread* t, uintptr_t caller);
static inline void sched_enqueue_thread(struct thread* t) {
    sched_enqueue_thread_from(t, (uintptr_t)__builtin_return_address(0));
}

/* Reset the process table, this CPU's scheduler state, the thread table and the
 * futex table. BSP only, once, before any AP is up. */
void process_init(void);
void process_ap_init(void); /* per-AP scheduler state init; call before enabling AP timer */
/*
 * Spawn family. All of these return 0 with *out_pid set, or -1 (no packed code:
 * these are kernel-internal, not a subsystem boundary). -1 covers a NULL entry
 * or out_pid, an exhausted table (PROCESS_MAX_COUNT), a failed mm-context or
 * kernel-stack allocation, and a name longer than PROCESS_NAME_MAX-1.
 *
 * A failure after the slot has been claimed leaves that slot in
 * PROCESS_STATE_NEW -- unschedulable, and not reclaimed. Take the table lock
 * internally; the caller must hold neither it nor any run-queue lock. None of
 * them block.
 *
 * process_spawn parents the child to the CPU's current process;
 * process_spawn_as names the parent explicitly (0 = orphan).
 */
int process_spawn(const char* name, process_entry_t entry, void* arg, uint32_t* out_pid);
int process_spawn_as(uint32_t parent_pid, const char* name, process_entry_t entry, void* arg,
                     uint32_t* out_pid);
/* As process_spawn_as, but the child is published BLOCKED and its main thread
 * is never enqueued, so no CPU can dispatch it until process_unpark_pid. Use
 * this whenever post-spawn setup (capabilities, priority, cwd) must land before
 * the child's first instruction. */
int process_spawn_as_parked(uint32_t parent_pid, const char* name, process_entry_t entry, void* arg,
                            uint32_t* out_pid);
/* process_spawn_as_parked plus process_set_require_explicit_ready, so the PM
 * treats the child as started only once it calls notify_ready. */
int process_spawn_as_ready_gated_parked(uint32_t parent_pid, const char* name,
                                        process_entry_t entry, void* arg, uint32_t* out_pid);
/* Make a parked process runnable and enqueue its main thread on the CALLING
 * CPU (placement is then left to work-stealing). Returns 0 on success AND when
 * the main thread was already awake -- the two are not distinguished -- or -1
 * if the pid or its main thread is gone. */
int process_unpark_pid(uint32_t pid);
/* Create the singleton idle process and install its main thread as the BSP's
 * idle thread. Boot-time only: it claims a slot without the table lock, and
 * returns -1 if an idle process already exists. */
int process_spawn_idle(const char* name, process_entry_t entry, void* arg, uint32_t* out_pid);
/* Add one idle thread to the existing idle process, pinned by affinity to
 * cpu_id, and install it as that CPU's idle thread. Never enqueued: idle
 * threads are reached only through cpu_sched_pick_next's per-CPU fallback.
 * Returns 0, or -1 if there is no idle process or cpu_id is 0 / out of range. */
int process_spawn_idle_ap(uint32_t cpu_id);
/* Compatibility shim: spawns a worker whose entry immediately returns
 * PROCESS_RUN_THREAD_EXITED, i.e. a thread that exists only long enough to
 * exit. Prefer process_thread_spawn_worker_internal. */
int process_thread_spawn_internal(uint32_t owner_pid, const char* name, uint32_t* out_tid);
/* Add a ring-0 kernel worker thread to owner_pid at SCHED_PRIO_SYSTEM, with its
 * own guarded kernel stack. Spawned BLOCKED and only promoted to READY once its
 * scheduler state is complete, then enqueued. Workers never take the process
 * runtime_lock. Returns 0 with *out_tid set, else -1 (owner missing, exiting or
 * not yet LIVE; no free thread slot; stack allocation failed). Does not block. */
int process_thread_spawn_worker_internal(uint32_t owner_pid, const char* name,
                                         process_thread_worker_entry_t entry, void* arg,
                                         uint32_t* out_tid);
/* Add a ring-3 thread to owner_pid: entry_rip/user_stack_top are user VAs in
 * that process's address space, and user_stack_top is rounded DOWN to 16 bytes
 * rather than refused. Runs at SCHED_PRIO_WASM on its own kernel stack.
 * Returns 0 with *out_tid set, else -1 (owner missing/exiting, zero rip or
 * stack, no free slot, stack allocation failed, or the owner has no root
 * table). */
int process_thread_spawn_user_internal(uint32_t owner_pid, const char* name, uint64_t entry_rip,
                                       uint64_t user_stack_top, uint32_t* out_tid);
/* Look up by pid / context id. Lock-free table scans: NULL if there is no such
 * live process. The returned pointer is only valid while the caller has reason
 * to believe that process is alive -- a reap makes the slot reusable. */
process_t* process_get(uint32_t pid);
process_t* process_find_by_context(uint32_t context_id);
/* pid dispatched on the calling CPU, or 0 while the scheduler itself is running. */
uint32_t process_current_pid(void);
/* Record the status a later exit will publish. Does not itself exit or wake
 * anything; the value is read when the process is marked exited. */
void process_set_exit_status(process_t* process, int32_t exit_status);
/* No-op. Blocking is a per-thread concern now and lives in sched_event_wait;
 * this does nothing and a caller relying on it to block will spin instead.
 * TODO: delete once the remaining call sites stop calling it. */
void process_block_on_ipc(process_t* process);
/* Latch process_t::ready. The flag is level-triggered and polled by the PM, so
 * setting it before any parent is waiting is fine -- the next pm_poll_spawn
 * iteration picks it up. */
void process_notify_ready(process_t* process);
void process_set_require_explicit_ready(process_t* process);
/*
 * Wait for a child to exit. TRI-STATE, not 0-on-success:
 *   0  the child was already a zombie; *out_exit_status is set and the child
 *      has been reaped (the slot is gone on return)
 *   1  the caller's CURRENT THREAD has been parked (THREAD_BLOCK_WAIT_PROCESS);
 *      it must return PROCESS_RUN_BLOCKED so the scheduler completes the block,
 *      and retry the call after being woken
 *  -1  refused: NULL process, target_pid 0 or equal to the caller, no such
 *      process, or the target is not this process's child
 * Only the parent may wait. Blocking is per-thread: the other threads of the
 * process keep running.
 */
int process_wait(process_t* process, uint32_t target_pid, int32_t* out_exit_status);
/*
 * Join a sibling thread of the same process. TRI-STATE:
 *   0   the target was already a zombie; *out_exit_status is set and the target
 *       has been reaped
 *   1   the calling thread has been parked (THREAD_BLOCK_WAIT_THREAD); return
 *       PROCESS_RUN_BLOCKED and retry after the wake
 *   < 0 a packed abi/errors.yaml code -- WASMOS_INVAL (NULL/0 tid, or joining
 *       self), WASMOS_ERR_KERNEL_NO_CALLER, WASMOS_ERR_THREAD_NOT_FOUND,
 *       WASMOS_ERR_THREAD_NOT_OWNER (either side belongs to another process),
 *       WASMOS_ERR_THREAD_JOIN_FAILED (already detached), or
 *       WASMOS_ERR_THREAD_BUSY (another thread is already joining)
 * Packed codes are negative, so the three cases cannot collide.
 */
int process_thread_join(process_t* process, uint32_t target_tid, int32_t* out_exit_status);
/* Mark a thread detached: it becomes unjoinable and is reaped automatically when
 * it exits. Reaps it immediately if it is already a zombie. Returns 0, or the
 * same packed codes as process_thread_join for a bad tid, a foreign owner, or a
 * thread someone else is already joining. */
int process_thread_detach(process_t* process, uint32_t target_tid);
/* Terminate a process: publishes the exit status, tombstones its threads, wakes
 * its waiters, and auto-reaps if that was requested. Returns 0 on success and
 * also when the target was ALREADY a zombie; -1 if there is no such pid, if it
 * is the caller's own pid (a process cannot kill itself here), or if the caller
 * is a process that is not the target's parent. Kernel callers
 * (current_pid == 0) bypass the parent check. */
int process_kill(uint32_t pid, int32_t exit_status);
/* Read a terminated process's exit status. INVERTED-LOOKING but deliberate:
 * 0 means "it has exited and *out_exit_status is set", 1 means "still alive,
 * nothing written", -1 means no such pid or a NULL out pointer. Callers use
 * != 0 as the liveness predicate. */
int process_get_exit_status(uint32_t pid, int32_t* out_exit_status);
/* Enable/disable auto-reap, then try it immediately: a process that is already
 * a zombie with no waiter is reaped before this returns, invalidating its slot.
 * Returns 0, or -1 for an unknown/free pid. */
int process_set_auto_reap(uint32_t pid, uint8_t enabled);
/* Reap a specific zombie by pid (CAS-guarded); used by the PM WAIT path to free
 * a child's slot after its exit status has been delivered.  No-op if not a
 * still-unreaped zombie. */
void process_reap_zombie_pid(uint32_t pid);
/* Wake one blocked thread through the full wake/block handshake (state change
 * plus enqueue, whichever side wins the claim). Returns 1 if the thread was
 * BLOCKED and a wake was attempted, 0 for tid 0, an unknown tid, or a thread in
 * any other state -- so 0 is "nothing to do", not an error. Non-zero-on-success:
 * do not read it as a status code. */
int process_wake_thread(uint32_t tid);
/*
 * Run one scheduling round on the calling CPU: pick a thread, switch to it, and
 * return once it yields, blocks or exits. Returns one of the SCHED_* codes in
 * sched.h -- SCHED_OK only for a voluntary yield, SCHED_R_RANDONE for the
 * ordinary blocked/exited outcome, and the rest for conditions the boot loop
 * classifies. Only SCHED_R_PICK / _CTX / _ROOT / _MAXCOUNT are bugs; _NOTREADY,
 * _ZOMBIE, _STALE and _CLAIMED are races that the caller simply retries.
 * Must be called with no run-queue lock held; it takes them itself.
 */
int process_schedule_once(void);
/* Switch from the current thread back into the scheduler, reporting `result`.
 * Returns to the caller only when the thread is dispatched again -- so it
 * blocks unless `result` is a yield. Requires a current process; a no-op
 * otherwise. Do not call with a spinlock held. */
void process_yield(process_run_result_t result);
/* Timer-tick accounting for the calling CPU: charges one tick to the running
 * thread, requests a reschedule when its slice runs out, and advances the
 * resched-stall watchdog. Called from the timer IRQ; no-op while the scheduler
 * itself is running. */
void process_tick(void);
int process_should_resched(void);
void process_set_need_resched(void);
void process_clear_resched(void);
/*
 * Turn a pending reschedule into an actual one on IRQ return. Snapshots `frame`
 * into the current thread's saved context and REWRITES the frame's
 * rip/cs/user_rsp/user_ss so the iretq lands in the ring-0 scheduler
 * trampoline. Returns 1 when the frame was rewritten (the caller must iretq
 * without further changes) and 0 when preemption was declined -- no resched
 * pending, preemption disabled, inside the scheduler or a context switch,
 * inside a host call, an interrupted RING-0 frame, the PM outside a
 * pm_preempt_safe region, or a frame that failed validation.
 */
int process_preempt_from_irq(irq_frame_t* frame);
/*
 * Per-CPU nesting counter that gates IRQ-driven preemption; it does NOT touch
 * the interrupt flag and does not stop a thread from blocking voluntarily.
 * preempt_enable saturates at zero rather than underflowing, so an unbalanced
 * enable is silently absorbed. critical_section_enter/leave are aliases kept
 * for call sites that read better that way.
 */
void preempt_disable(void);
void preempt_enable(void);
int preempt_is_enabled(void); /* non-zero iff the depth is 0 */
uint32_t preempt_disable_depth(void);
void critical_section_enter(void);
void critical_section_leave(void);
/* Voluntary preemption point: yields only if a reschedule is already pending.
 * Blocks for as long as the thread stays descheduled; returns immediately when
 * there is no current process or nothing pending. */
void preempt_safepoint(void);
/* Bracket a region of the process manager in which IRQ preemption is allowed.
 * The PM is otherwise never preempted from an IRQ, since it is the one process
 * whose mid-request state other processes are blocked on. Nesting counter, per
 * CPU; leave saturates at zero. */
void pm_preempt_safe_enter(void);
void pm_preempt_safe_leave(void);
/* Resched stalls seen on the CALLING CPU plus the global count of invalid trap
 * frames. Monotonic; a non-zero value means the watchdog fired at least once. */
uint64_t process_watchdog_issue_count(void);
/* Slots that process_info_at_stats will enumerate: everything except free
 * (UNUSED/DEAD) and in-reap (REAPING) slots, so zombies ARE counted. */
uint32_t process_count_active(void);
/* Threads queued on the CALLING CPU's run queue, summed over priority bands
 * under that queue's lock. Not a system-wide total. */
uint32_t process_ready_count(void);
/*
 * Enumerate live processes by dense index, 0 upward, until -1 is returned.
 * *out_name borrows the process's own storage and stays valid only while that
 * process lives. The index is over a table that other CPUs mutate, so entries
 * can shift between calls; this is reporting, not a stable iterator.
 *
 * process_info_at and process_info_at_ex skip zombies; process_info_at_stats
 * includes them (reported as PROCESS_STATE_ZOMBIE), so the two families do NOT
 * share an index space. Return 0 on success, -1 for a NULL out pointer or an
 * index past the end.
 */
int process_info_at(uint32_t index, uint32_t* out_pid, const char** out_name);
int process_info_at_ex(uint32_t index, uint32_t* out_pid, uint32_t* out_parent_pid,
                       const char** out_name);
int process_info_at_stats(uint32_t index, uint32_t* out_pid, uint32_t* out_parent_pid,
                          const char** out_name, process_stats_t* out_stats);
/* Require (or stop requiring) the per-process runtime lock around each entry
 * call. Returns 0, or -1 for an unknown pid. Takes effect from the next
 * dispatch; it does not affect a call already in progress. */
int process_set_runtime_lock_required(uint32_t pid, uint8_t required);
/* Set the subsystem tag reported by `ps` ("KERNEL", "WARP", "WASM3", ...) and,
 * under WARP, used by the dispatcher to decide whether linear memory needs
 * re-syncing. Returns 0, or -1 for an unknown pid, a NULL tag, or a tag longer
 * than WASMOS_APP_SUBSYSTEM_TAG_LEN (which leaves it truncated). */
int process_set_runtime_tag(uint32_t pid, const char* tag);
/* Set the scheduler priority band (SCHED_PRIO_*) of a process's main thread.
 * Only valid before the child is first scheduled (e.g. on a freshly parked
 * process, before unpark): the thread must not yet be enqueued in a runqueue.
 * Enforced, not advisory -- a queued thread is REFUSED with -1 (and counted as
 * SCHED_DEBUG_SET_PRIO_QUEUED) rather than re-banded silently. Also -1 for
 * prio >= SCHED_PRIO_MAX, an unknown pid, or a process with no main thread. */
int process_set_main_prio(uint32_t pid, uint8_t prio);
/* Convert a process's main thread to ring 3: installs the user CS/SS, the given
 * user RIP/RSP and the process's own root table, after stripping the low
 * identity slot from that root so kernel-low addresses are unreachable from
 * user mode. Returns 0, or -1 for an unknown pid, a zero rip/rsp, a kernel
 * stack outside the higher half, or a root table that fails the no-low-slot
 * verification. Only meaningful before the process is first dispatched. */
int process_set_user_entry(uint32_t pid, uint64_t rip, uint64_t user_rsp);

#ifdef WASMOS_PROCESS_TEST_SEAMS
/* Host-test entry to the static lifecycle transitions. Compiled out of the
 * kernel entirely; it adds no behaviour of its own and no hook into any
 * scheduler path, it only makes two functions callable from a suite that links
 * process.c.
 *
 * They exist because both transitions are reachable in production ONLY through
 * a caller that has already filtered the target's state (a BLOCKED waiter, a
 * READY sibling), so the interleaving each one absorbs -- the target moving to
 * another state between that filter and the transition -- cannot be produced
 * from outside process.c on a host. Driving the transition directly asserts the
 * same contract without a race: the caller's filter is not what makes it safe.
 *
 * Return values are the transitions' own: 1 = the owner permits this thread to
 * be made runnable/dispatched, 0 = the owner raced to a terminal state and the
 * caller must not enqueue/dispatch. NOT "did the thread's state change". */
int process_test_set_ready(process_t* proc, struct thread* thread);
int process_test_set_running(process_t* proc, struct thread* thread);
#endif

#endif
