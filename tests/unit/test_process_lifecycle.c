/* test_process_lifecycle.c — process.c's lifecycle state machine, driven from
 * real threads.
 *
 * Regression: 2026-08-22-set-ready-on-exiting-owner -- a worker thread exiting
 * while its own process was concurrently being killed panicked the kernel
 * ("set_ready zombie"). process_schedule_once_impl's PROCESS_RUN_THREAD_EXITED
 * branch decrements live_thread_count, finds a surviving sibling and makes it
 * READY; nothing between that decision and process_set_ready excludes another
 * CPU marking the process exited, so the guard inside process_set_ready fired
 * on a legitimate interleaving and took the machine down. The window is a few
 * instructions wide, so it presented as an unreproducible ~1-in-N CI boot
 * failure in the boot selftests and could not be reproduced under
 * single-threaded TCG at all (40 clean local boots).
 *
 * Why this suite and not test_sched_concurrency.c: that suite has the same
 * pthread-as-CPU harness but links only sched_thread.c and STUBS the process
 * layer (process_set_need_resched, thread_transit, thread_table_at). The
 * lifecycle state machine it stubs away is exactly what panicked, so it could
 * never have caught this. This suite links the real process.c, which is what
 * WASMOS_PROCESS_TEST_SEAMS exists for: it replaces process.c's six inline-asm
 * sites -- two stack switches, sti, the higher-half self-relocation probe and
 * the scheduler-context register capture -- none of which a lifecycle question
 * depends on.
 *
 * WASMOS_HOST_TEST_SMP routes cpu_local() through a _Thread_local, so a pthread
 * genuinely IS a CPU: it has its own cpu_local, its own run queue, and it
 * contends for the same locks and atomics the kernel does.
 *
 * The suite is in two layers, and the division is deliberate.
 *
 * The CONTRACT cases drive process.c's two lifecycle transitions directly, via
 * the WASMOS_PROCESS_TEST_SEAMS entries in process.h, and cannot miss. They own
 * the per-branch coverage: each transition's refusal under an exiting owner, the
 * promotion of a BLOCKED target and the clearing of its block_reason, the
 * inertness of a requeue aimed at a READY one, and the rule that a promotion
 * never overwrites a live dispatch claim. They are single-threaded: the
 * interleavings these transitions absorb are stated as a starting state rather
 * than raced for, because every production caller filters its target's state
 * first and the windows are a few instructions wide -- unreachable from outside
 * process.c on a host, and NOT what makes the transitions safe.
 *
 * The SOAK proves the real interleaving is survived end to end. Both halves of
 * the transition pair are counted (SCHED_DEBUG_SET_READY_EXITING,
 * SCHED_DEBUG_SET_RUNNING_EXITING) and it asserts their SUM is non-zero, so a run
 * that never reached the window fails rather than passing vacuously -- the
 * failure mode a race test normally has.
 *
 * The sum, and not each half, because of how narrow one of them is. set_running
 * (the dispatch half) is hit hundreds of times per run at every width: once an
 * owner is exiting, every attempt to dispatch one of its threads lands there.
 * set_ready (the requeue half, the one the CI panic named) is hit 0-7 times per
 * 300 rounds, because process_mark_exited publishes `exiting` four statements
 * before it marks the owner's threads (the store to ->exiting, then
 * block_reason, wait_target_pid and the force-transit to ZOMBIE, then
 * thread_mark_owner_exited), and a requeue arriving after that finds no READY
 * sibling and parks instead. Landing in that sub-window -- what process.h
 * describes as "1 slightly ahead of ->state" -- is luck, and structurally so: the
 * retiring worker below observes `exiting` at the window's FIRST statement and
 * then has to travel all the way back through the dispatch return before the
 * killer reaches its fourth. Aiming the two more tightly cannot change that, and
 * widening the window itself would mean a behavioural hook inside a hot path.
 * The per-branch claim therefore rests on the contract case above; the soak's job
 * is the race, and the printout reports both counters so a reader can see which
 * half a given run explored.
 *
 * Either half was a kpanic before the fix, so the suite is red-to-green on the
 * family: with the guards restored it aborts with "set_running zombie" at every
 * width (verified at 2 and 4 CPUs, exit 134).
 *
 * The assertions themselves are deterministic throughout: they must hold after
 * ANY interleaving. Only which interleavings get explored varies.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "process.h"
#include "thread.h"
#include "sched.h"
#include "arch/x86_64/smp.h"

/* ------------------------------------------------------------------ harness */

/* The CPU table the linked scheduler and process layer see. cpu_local() is the
 * WASMOS_HOST_TEST_SMP arm in arch/x86_64/smp.h, reading g_host_cpu_local -- a
 * _Thread_local pointer into g_cpus[]. A pthread picks which CPU it is by
 * calling be_cpu() once; every kernel call it makes afterwards runs on that
 * CPU. A worker that never calls be_cpu() leaves the pointer NULL and the first
 * cpu_local() dereference faults. */
cpu_local_t g_cpus[WASMOS_MAX_CPUS];
uint32_t g_cpu_count = 4;
_Thread_local cpu_local_t* g_host_cpu_local;

/* CPU count is a build parameter so the gate can run the same race at several
 * widths, exactly as test_sched_concurrency.c does. It is not cosmetic here
 * either: every CPU but one runs a scheduler loop, which is what the kernel
 * does, so widening it adds dispatchers that can retire a thread of the SAME
 * process concurrently -- the shape the CI capture had, where the panic landed
 * on CPU 0 while CPU 3 was spawning workers. More CPUs than host cores is a
 * feature: the preemption that causes explores interleavings a 1:1 mapping
 * never reaches. */
#ifndef WASMOS_TEST_NCPU
#define WASMOS_TEST_NCPU 4
#endif
#if WASMOS_TEST_NCPU > WASMOS_MAX_CPUS
#error "WASMOS_TEST_NCPU exceeds WASMOS_MAX_CPUS"
#endif
#if WASMOS_TEST_NCPU < 2
#error "the race needs at least one dispatcher and one killer"
#endif
#define NCPU WASMOS_TEST_NCPU
/* The killer is the last CPU; every lower-numbered CPU dispatches. */
#define KILLER_CPU (NCPU - 1u)

/* Rounds of the kill race. Each round is a whole spawn/exit/reap cycle, so this
 * is bounded by process-table churn rather than atomics throughput. The count is
 * not load-bearing -- the refusal assertion is -- but it has to be large enough
 * to land inside a window a few instructions wide.
 *
 * The ceiling is not arbitrary, but it is no longer about a bug. It used to bound
 * the soak below the slot-recycle race, which is now fixed (thread_t::
 * dispatch_ref). It stays because round throughput tracks the host's spare cores
 * rather than the kernel -- see the assertion notes further down -- so a larger
 * number would buy variance, not coverage. */
#define KILL_ROUNDS 300

/* Dispatch attempts, as a BOUND rather than a count. sched_spawn_thread
 * places a new worker on a CPU of its own choosing, so a worker is usually on
 * some other CPU's queue and only reaches this one by work-stealing, which
 * cpu_sched_try_steal does from inside the scheduler loop. A fixed handful of
 * dispatches therefore misses the worker most of the time; the loops below spin
 * up to this bound and stop as soon as the worker has retired. */
#define DISPATCH_BOUND 2000u

/* The healthy case waits for ONE retirement and has no later round to make up
 * for a miss, so it gets a far larger bound than a soak round does. It is a
 * ceiling, not a cost: the loop stops the moment the worker retires, and only a
 * core-starved host ever walks far into it. */
#define HEALTHY_DISPATCH_BOUND 200000u

static int g_failures;
static int g_checks;

/* stubs_kpanic.c declares these weak so a suite can supply its own. Discard
 * them: process.c reports every refusal here through the rate-limited writer,
 * and on a soak of this size that buries the suite's own output. The refusals
 * are asserted through sched_debug_count(), which is the honest total anyway
 * (the log is power-of-two rate-limited and cannot be counted from). */
void serial_printf(const char* fmt, ...) {
    (void)fmt;
}
void serial_printf_unlocked(const char* fmt, ...) {
    (void)fmt;
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* Make the calling pthread act as CPU `id` for every kernel call afterwards. */
static void be_cpu(uint32_t id) {
    g_host_cpu_local = &g_cpus[id];
}

/* The idle thread's entry. Like idle_main it never really executes -- an idle
 * thread is not a kernel worker, so it goes through the inert context_switch --
 * but it has to EXIST, for a reason that is not obvious and cost this harness
 * several dead ends: work-stealing in process_schedule_once_impl runs only when
 * cpu_sched_pick_next returned this CPU's idle thread. With no idle thread a CPU
 * whose queue is empty returns SCHED_R_PICK and never steals, so every worker
 * sched_spawn_thread placed on another CPU's queue is unreachable forever. */
static process_run_result_t idle_entry(process_t* process, void* arg) {
    (void)process;
    (void)arg;
    return PROCESS_RUN_IDLE;
}

static void harness_init(void) {
    uint32_t idle_pid = 0;

    memset(g_cpus, 0, sizeof(g_cpus));
    for (uint32_t i = 0; i < NCPU; ++i) {
        g_cpus[i].cpu_id = i;
        g_cpus[i].self = &g_cpus[i];
        g_cpus[i].started = 1;
    }
    g_cpu_count = NCPU;
    be_cpu(0);
    process_init();

    /* Same order as kernel.c: the BSP's idle thread comes from
     * process_spawn_idle, then each AP initialises its own per-CPU scheduler
     * state and adds its own idle thread. process_ap_init and
     * process_spawn_idle_ap both reach through cpu_local(), so the caller has to
     * BE that CPU while they run; done here while still single-threaded, which
     * is the ordering the kernel requires anyway -- before anything can dispatch
     * or enqueue on that CPU. Skipping process_ap_init leaves that CPU's run
     * queue with uninitialised list heads and the first cross-CPU enqueue walks
     * a NULL. */
    if (process_spawn_idle("idle", idle_entry, 0, &idle_pid) != 0) {
        printf("  [FATAL] no idle process; nothing can steal\n");
        return;
    }
    for (uint32_t i = 1; i < NCPU; ++i) {
        be_cpu(i);
        process_ap_init();
        if (process_spawn_idle_ap(i) != 0) {
            printf("  [FATAL] no idle thread for cpu %u\n", (unsigned)i);
        }
    }
    be_cpu(0);
}

/* Shared soak state. Plain types with explicit __atomic_* orderings, which is
 * how the kernel itself expresses shared scheduler state (see
 * sched_wake_claim_enqueue in thread.h).
 *
 * g_target_pid is the process every thread is currently working on;
 * g_worker_retiring is the aiming mechanism. The requeue that panicked runs
 * immediately after the retiring worker returns, so the kill has to arrive
 * between the dispatch that started that worker and the requeue. Publishing
 * only the pid puts the kill BEFORE the dispatch, where process_set_running
 * refuses it and the worker never runs at all -- an unaimed soak reports
 * thousands of set_running refusals and no set_ready ones. The worker therefore
 * announces that it is inside the window and spins briefly; the killer waits for
 * that announcement. */
static uint32_t g_target_pid;
static uint32_t g_worker_retiring;
static uint32_t g_soak_done;
static uint32_t g_kills_landed;
static uint32_t g_workers_retired;

/* Iterations the retiring worker spins for. Not a timeout: it retires either
 * way, so a killer that never arrives costs a round rather than hanging. */
#define RETIRE_SPIN 200000u

/* ---------------------------------------------------------- test processes */

/* Never actually executes: only threads with is_kernel_worker take the
 * WASMOS_PROCESS_TEST_SEAMS path in process_schedule_once_impl, so a main thread
 * is dispatched through context_switch_high, which the platform stub models as
 * an inert "dispatched and yielded". The process still has to be RUNNABLE --
 * a BLOCKED owner is never dispatched at all and its workers never get picked
 * up -- which is why the target is spawned normally rather than parked. Two
 * worker threads play both roles the race needs. */
static process_run_result_t idle_main(process_t* process, void* arg) {
    (void)process;
    (void)arg;
    return PROCESS_RUN_YIELDED;
}

/* The surviving sibling: stays READY forever, so it is what
 * process_first_owner_ready_thread returns when the other worker retires.
 * Whether it may be made READY while the owner is dying is the whole question. */
static process_run_result_t sibling_worker(process_t* process, uint32_t tid, void* arg) {
    (void)process;
    (void)tid;
    (void)arg;
    return PROCESS_RUN_YIELDED;
}

/* Set by the retiring worker as it enters, cleared by the killer once it has
 * acted. This is the aiming mechanism: the requeue that panicked happens
 * immediately AFTER this worker returns, so the kill has to arrive between the
 * dispatch that started it and that requeue. Publishing only the pid puts the
 * kill BEFORE the dispatch instead, where process_set_running refuses it and
 * the worker never runs at all -- which is why an unaimed soak reports
 * thousands of set_running refusals and no set_ready ones. */

/* Retires the first time it runs, taking the THREAD_EXITED branch -- the site
 * that requeues a sibling and panicked. The spin deliberately widens a window
 * that is a few instructions wide in the kernel; without it the interleaving is
 * reachable in principle and not in practice. */
static process_run_result_t exit_immediately_worker(process_t* process, uint32_t tid, void* arg) {
    (void)tid;
    (void)arg;
    __atomic_fetch_add(&g_workers_retired, 1u, __ATOMIC_RELAXED);
    /* Tell the killer a worker is inside the window. */
    __atomic_store_n(&g_worker_retiring, 1u, __ATOMIC_RELEASE);
    /* Then wait for the owner to actually start dying, and retire INTO that.
     * This is the CI interleaving stated as an ordering rather than left to
     * chance: the requeue that panicked runs immediately after this returns, so
     * returning once `exiting` is visible puts process_set_ready in exactly the
     * state that used to be fatal. Aiming a race deliberately is the point of a
     * regression test -- the claim is not "races are absent", it is "this
     * interleaving is handled" -- and it is what makes the refusal count below
     * assertable instead of probabilistic. Bounded, so a round where no kill
     * arrives costs a spin rather than hanging. */
    for (uint32_t i = 0; i < RETIRE_SPIN; ++i) {
        if (process && __atomic_load_n(&process->exiting, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    return PROCESS_RUN_THREAD_EXITED;
}

/* Spawns a parked target with a surviving sibling worker and a retiring worker.
 * Returns 0 and sets *out_pid, or -1 when the process table is full. */
/* Retiring workers per target. One kill per target lands while several of them
 * are inside their window at once (one per dispatching CPU), so a single kill
 * yields several passes through the requeue instead of one -- and it does so
 * without more spawn/reap churn, which is what the round ceiling above is
 * really bounding. */
#define EXITERS_PER_TARGET 6u

static int spawn_race_target(uint32_t* out_pid) {
    uint32_t pid = 0;
    uint32_t tid = 0;

    if (process_spawn("race-target", idle_main, 0, &pid) != 0) {
        return -1;
    }
    /* The surviving sibling must exist before any exiter retires: it is what
     * process_first_owner_ready_thread returns, and with no READY sibling the
     * THREAD_EXITED branch parks the process instead of taking the requeue. */
    if (process_thread_spawn_worker_internal(pid, "race-sibling", sibling_worker, 0, &tid) != 0) {
        (void)process_kill(pid, 0);
        return -1;
    }
    for (uint32_t i = 0; i < EXITERS_PER_TARGET; ++i) {
        if (process_thread_spawn_worker_internal(
                pid, "race-exiter", exit_immediately_worker, 0, &tid) != 0) {
            break; /* fewer exiters is fine; zero is not, and the sibling is up */
        }
    }
    *out_pid = pid;
    return 0;
}

/* --------------------------------------- the transitions, driven directly */

/* Spawns a live process with one kernel-worker thread and hands back that
 * thread, unlinked from every run queue.
 *
 * Unlinked because that is the disposition a dispatcher sees: cpu_sched_pick_next
 * unlinks a thread before the dispatcher claims it, so "is it linked" afterwards
 * is an observation about what the transition under test did, not a leftover from
 * the spawn. Returns 0, or -1 when the process table is full.
 *
 * The worker never runs in these cases -- nothing dispatches -- so its entry is
 * irrelevant; sibling_worker is reused for it. */
static int spawn_transition_target(uint32_t* out_pid, thread_t** out_thread) {
    uint32_t pid = 0;
    uint32_t tid = 0;
    thread_t* thread = 0;

    if (process_spawn("xition-target", idle_main, 0, &pid) != 0) {
        return -1;
    }
    if (process_thread_spawn_worker_internal(pid, "xition-worker", sibling_worker, 0, &tid) != 0) {
        (void)process_kill(pid, 0);
        return -1;
    }
    thread = thread_get(tid);
    if (!thread) {
        (void)process_kill(pid, 0);
        return -1;
    }
    cpu_sched_remove_thread(thread);
    *out_pid = pid;
    *out_thread = thread;
    return 0;
}

/* Kill and reap, so a case cannot starve the next one of table slots. The
 * dispatch loop is what lets the owner's threads retire; 16 rounds is ample with
 * no other CPU competing for them. */
static void drop_transition_target(uint32_t pid) {
    (void)process_kill(pid, 0);
    for (uint32_t i = 0; i < 16u; ++i) {
        (void)process_schedule_once();
    }
    process_reap_zombie_pid(pid);
}

/* Regression: 2026-08-23-set-ready-demotes-a-running-thread -- process_set_ready
 * promoted its target with an unconditional thread_set_state, so a sibling
 * requeue aimed at a thread another CPU had just claimed for dispatch wrote
 * READY over RUNNING.
 *
 * What that costs: RUNNING *is* the exclusive dispatch claim
 * (cpu_sched_claim_for_dispatch is a READY->RUNNING CAS), so overwriting it
 * re-arms the claim -- a second CPU then wins it on a thread that is already
 * executing, and two CPUs resume one process_context_t on one kernel stack. That
 * is the torn-rip cpu_exception the claim was introduced to stop. The enqueue
 * side's last-resort guard is "state != READY, skip", so a demotion silences
 * that too and the executing thread can be linked into a ready queue as well.
 *
 * Driven directly because every production call site filters its target's state
 * first (a BLOCKED waiter, a READY sibling): the demotion is reachable only when
 * the target moves between that filter and the transition, a window a few
 * instructions wide that cannot be produced from outside process.c. The
 * filtering is not what makes the transition safe, which is exactly the claim
 * this case pins.
 *
 * The window modelled here is the one inside a dispatch: the claim is taken and
 * cpu_local()->current_thread is NOT yet published (process_schedule_once_impl
 * publishes it only after process_set_running). Inside it the RUNNING state is
 * the only record anywhere that the thread is spoken for -- sched_enqueue_thread's
 * holder scan cannot see it -- which is why losing that state is unrecoverable
 * rather than merely untidy. */
static void s_promotion_never_destroys_a_dispatch_claim(void) {
    uint32_t pid = 0;
    thread_t* t = 0;

    sched_debug_reset();
    be_cpu(0);
    if (spawn_transition_target(&pid, &t) != 0) {
        CHECK(0, "spawned a target with one worker thread");
        return;
    }

    CHECK(cpu_sched_claim_for_dispatch(t) == 1, "a dispatcher took the thread's dispatch claim");
    CHECK(t->state == THREAD_STATE_RUNNING, "the claim is recorded as RUNNING");

    int runnable = process_test_set_ready(process_get(pid), t);

    /* The owner is healthy, so the wake is permitted and its caller still owes
     * the wake/block handshake. A 0 here is the OTHER half of this pair of
     * regressions (fixed in d8d6bf3958): the return value answers "may this
     * owner's thread be made runnable", not "did this call change the thread's
     * state", and reporting the latter suppresses sched_wake_claim_enqueue and
     * loses the wake for precisely the target that is executing. */
    CHECK(runnable == 1, "the transition reported that the owner permits the wake");
    CHECK(t->state == THREAD_STATE_RUNNING, "the dispatch claim survived the promotion");

    /* And the enqueue a permitted wake performs must not link it either. Observed
     * BEFORE the second claim attempt below, which would otherwise repair the
     * state it is meant to catch: a claim that wrongly succeeds writes RUNNING
     * back, and both assertions here would then pass on a demoted thread. */
    if (sched_wake_claim_enqueue(t)) {
        sched_enqueue_thread(t);
    }
    CHECK(t->on_rq == 0, "an executing thread was not linked into a ready queue");
    CHECK(sched_debug_count(SCHED_DEBUG_ENQUEUE_FROM_NON_READY) > 0,
          "the enqueue was refused because the thread was not READY");

    CHECK(cpu_sched_claim_for_dispatch(t) == 0,
          "no second CPU can claim a thread that is already executing");

    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_NONE);
    drop_transition_target(pid);
}

/* The half the guard above must not break: a genuinely BLOCKED target is
 * promoted, and its block_reason goes with the state.
 *
 * Regression: 2026-08-22-promote-leaves-stale-block-reason (fixed 06b77e3c43) --
 * promoting with the state alone left the waiter READY still carrying the reason
 * it had blocked for, and the wait paths read that reason and put it straight
 * back to sleep. */
static void s_promotion_wakes_a_blocked_target_and_clears_its_reason(void) {
    uint32_t pid = 0;
    thread_t* t = 0;

    sched_debug_reset();
    be_cpu(0);
    if (spawn_transition_target(&pid, &t) != 0) {
        CHECK(0, "spawned a target with one worker thread");
        return;
    }

    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_WAIT_PROCESS);
    CHECK(process_test_set_ready(process_get(pid), t) == 1, "the owner permits the wake");
    CHECK(t->state == THREAD_STATE_READY, "a blocked target is promoted to READY");
    CHECK(t->block_reason == THREAD_BLOCK_NONE, "and its block reason is cleared with the state");
    /* The promotion recorded WHO performed it. Asserted as non-zero rather than
     * against an address, which would pin the linker's layout: the failure mode
     * worth catching is a breadcrumb that is never written at all, since the stall
     * dump would then print a plausible-looking zero forever and the next reader
     * would conclude nothing promoted the thread. */
    CHECK(t->ready_by != 0, "and the promotion recorded its call site");

    drop_transition_target(pid);
}

/* An already-READY target -- the sibling-requeue case, where the scan that found
 * the thread found it READY -- must be reported as permitted so the caller
 * completes the handshake, and must not be touched. */
static void s_promotion_of_a_ready_target_is_permitted_and_inert(void) {
    uint32_t pid = 0;
    thread_t* t = 0;

    sched_debug_reset();
    be_cpu(0);
    if (spawn_transition_target(&pid, &t) != 0) {
        CHECK(0, "spawned a target with one worker thread");
        return;
    }

    CHECK(t->state == THREAD_STATE_READY, "the spawned worker is READY");
    CHECK(process_test_set_ready(process_get(pid), t) == 1,
          "requeueing a READY sibling is permitted");
    CHECK(t->state == THREAD_STATE_READY, "and leaves it READY");

    drop_transition_target(pid);
}

/* Regression: 2026-08-22-set-ready-on-exiting-owner -- the panic this suite was
 * built for, driven as a contract rather than as a race.
 *
 * The soak below reproduces the real interleaving, but reaches the set_ready half
 * of the pair only 0-7 times in 300 rounds: process_kill marks the owner's
 * threads within a few instructions of setting `exiting`, so a requeue arriving
 * afterwards finds no READY sibling and parks the process instead of transitioning
 * it. Reaching it needs the sub-window process.h describes as "`exiting` 1
 * slightly ahead of ->state", which the soak can only be lucky enough to land in
 * -- which is why its assertion is on the SUM of the two counters and cannot
 * speak for either half alone. Publishing the flag here reproduces that window
 * exactly, so both halves are covered by a case that cannot miss.
 *
 * ZOMBIE is deliberately not poked in: it is refused twice over (the state test
 * here, then process_force_transit, which has no ZOMBIE->READY edge), and the CI
 * panic's cause was the `exiting` flag -- the process was still in a live state
 * when the transition arrived. */
static void s_exiting_owner_refuses_both_transitions(void) {
    uint32_t pid = 0;
    thread_t* t = 0;
    process_t* proc = 0;

    sched_debug_reset();
    be_cpu(0);
    if (spawn_transition_target(&pid, &t) != 0) {
        CHECK(0, "spawned a target with one worker thread");
        return;
    }
    proc = process_get(pid);
    if (!proc) {
        CHECK(0, "the target is resident");
        return;
    }

    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_NONE);
    __atomic_store_n(&proc->exiting, 1u, __ATOMIC_RELEASE);

    CHECK(process_test_set_ready(proc, t) == 0, "a requeue under an exiting owner is refused");
    CHECK(sched_debug_count(SCHED_DEBUG_SET_READY_EXITING) == 1, "and counted, not fatal");
    CHECK(t->state == THREAD_STATE_BLOCKED, "and left its target alone");

    CHECK(process_test_set_running(proc, t) == 0, "a dispatch under an exiting owner is refused");
    CHECK(sched_debug_count(SCHED_DEBUG_SET_RUNNING_EXITING) == 1, "and counted, not fatal");
    CHECK(t->state == THREAD_STATE_BLOCKED, "and left its target alone");

    __atomic_store_n(&proc->exiting, 0u, __ATOMIC_RELEASE);
    drop_transition_target(pid);
}

/* Regression: 2026-08-23-aborted-dispatch-strands-its-thread -- a dispatch that
 * aborts after its thread has been taken off a run queue leaves it on no queue,
 * because `dispatch_done` releases the slot claim and handles reaps but never
 * re-enqueues (`src/kernel/process.c`, the label body).
 *
 * By the time any of those aborts is reachable the thread is ALREADY unlinked:
 * cpu_sched_pick_next unlinks under the queue lock and cpu_sched_try_steal
 * unlinks from the remote queue, both before the dispatcher claims it. So the
 * run queue is not a place the thread can be "left"; it has to be put back, and
 * two of the aborts explicitly hand back only the STATE
 * (`thread_transit(RUNNING, READY)`) before jumping to the label.
 *
 * The abort driven here is the one that needs no race: process_set_running
 * refuses because the owner is `exiting`, which this case publishes directly.
 * The refusal itself is correct and is asserted elsewhere in this file -- the
 * question is what happens to the thread AFTER it.
 *
 * Why this is the shape to test now: six CI captures of a whole-session stall all
 * report the same thread as READY, on no run queue, with no wake token and no owed
 * enqueue (`stranded(ready,no-rq)=1`, gfx-compositor, `disp` frozen across every
 * sample; see docs/TASKS.md). That is the state an aborted dispatch leaves behind,
 * and no other mechanism in the tree has been shown to produce it.
 *
 * REACHABILITY is the open question this case does not settle, and it must not be
 * read as settling it. `exiting` is set in exactly one place
 * (`process_mark_exited`) and is followed by a forced transition to ZOMBIE, so a
 * thread stranded by THIS abort belongs to a process that is being torn down and
 * would be reaped anyway. What makes it worth pinning regardless: the strand is a
 * property of the abort path, not of the reason for the abort, and the other two
 * aborts (`SCHED_R_STALE`, and a claim lost to another CPU) reach the same label
 * by the same route. */
static void s_aborted_dispatch_leaves_its_thread_reachable(void) {
    uint32_t pid = 0;
    uint32_t tid = 0;
    thread_t* t = 0;
    process_t* proc = 0;

    sched_debug_reset();
    be_cpu(0);

    if (process_spawn("abort-target", idle_main, 0, &pid) != 0) {
        CHECK(0, "spawned a target");
        return;
    }
    if (process_thread_spawn_worker_internal(pid, "abort-worker", sibling_worker, 0, &tid) != 0) {
        CHECK(0, "spawned a worker");
        (void)process_kill(pid, 0);
        return;
    }
    t = thread_get(tid);
    proc = process_get(pid);
    if (!t || !proc) {
        CHECK(0, "the worker and its owner are resident");
        (void)process_kill(pid, 0);
        return;
    }

    /* The worker is READY and queued: process_thread_spawn_worker_internal
     * publishes it and calls sched_spawn_thread, which is the state a dispatcher
     * finds it in. */
    CHECK(t->state == THREAD_STATE_READY, "the worker is READY");
    CHECK(t->on_rq == 1, "and queued, so a dispatch has something to unlink");

    /* Publish the refusal condition without killing the process, so the abort is
     * the only thing that touches the worker. */
    __atomic_store_n(&proc->exiting, 1u, __ATOMIC_RELEASE);

    /* Drive the REAL dispatch path rather than re-implementing its sequence.
     * sched_spawn_thread may have placed the worker on another CPU's queue, so
     * this CPU reaches it by stealing, which only happens when pick_next returns
     * this CPU's idle thread -- hence a bounded loop rather than one call. Stops
     * as soon as the worker is off every queue, which is the moment the abort has
     * happened. */
    for (uint32_t i = 0; i < 20000u && t->on_rq; ++i) {
        (void)process_schedule_once();
    }

    CHECK(sched_debug_count(SCHED_DEBUG_SET_RUNNING_EXITING) > 0,
          "a dispatch of the worker was refused, so the abort was reached");

    /* What the abort actually does, recorded because it is the CI signature
     * exactly: READY (1), on no run queue, owed nothing, no wake token. */
    printf("  ... after the abort: state=%u on_rq=%u owed=%u wake=%u\n",
           (unsigned)t->state,
           (unsigned)t->on_rq,
           (unsigned)t->enqueue_owed,
           (unsigned)t->wake_pending);
    CHECK(t->state == THREAD_STATE_READY && !t->on_rq && !t->enqueue_owed && !t->wake_pending,
          "the abort left the thread runnable, unqueued and owed nothing");

    /* The invariant, and the reason it is conditional rather than absolute. A
     * runnable thread must be reachable -- queued, or owed an enqueue -- UNLESS
     * its owner is going away, in which case leaving it unqueued is deliberate
     * and the reaper collects it. Re-enqueueing it there would be re-picked and
     * re-refused at process_set_running forever.
     *
     * Written as a disjunction including the owner's state so it pins the SAFETY
     * ARGUMENT, not just today's behaviour: the day an abort strands a thread
     * whose owner is alive, this fails. That is the case six CI captures show and
     * that no test could previously express. */
    uint8_t owner_going_away =
        proc->state == PROCESS_STATE_ZOMBIE || __atomic_load_n(&proc->exiting, __ATOMIC_ACQUIRE);
    CHECK(t->on_rq || t->enqueue_owed || t->state != THREAD_STATE_READY || owner_going_away,
          "a thread left unqueued by an aborted dispatch belongs to a dying owner");

    /* And the kernel's own tripwire for that case must agree: it excludes a dying
     * owner, so this abort must NOT have reported one. A count here would mean
     * the tripwire over-reports and would bury the real signal in CI. */
    CHECK(sched_debug_count(SCHED_DEBUG_DISPATCH_LEFT_STRANDED) == 0,
          "the stranded-dispatch tripwire stayed silent for a dying owner");

    __atomic_store_n(&proc->exiting, 0u, __ATOMIC_RELEASE);
    drop_transition_target(pid);
}

/* Regression: 2026-08-23-wake-marks-without-claiming -- sched_wake_thread's
 * claim-lost arm marks the target READY and returns without leaving a claim, so
 * a wake that arrives while the target's blocking transition is in flight can be
 * lost entirely and the thread never runs again.
 *
 * The arm reads: if sched_wake_claim_enqueue returns 0, the completion path is
 * said to own the enqueue, so this side calls sched_mark_ready_if_live and
 * returns. That holds only while the completion path has not YET made its
 * decision. Once it has -- it clears blocking_transition, takes the token, reads
 * the state, sees BLOCKED and correctly declines to enqueue a blocked thread --
 * the mark lands after the last thing that would have acted on it. The thread is
 * then READY, on no run queue, with no wake token and no owed enqueue, and the
 * owed-enqueue sweep cannot recover it either because its gate is the global debt
 * counter and this thread carries no debt.
 *
 * This is the same defect the enqueue-current path already had and fixed with
 * sched_owe_enqueue, whose comment states the rule outright: a mark is not a
 * message, because a holder that has already run its check never sees it. The
 * fix there was to publish a CLAIM alongside the mark. This arm still publishes
 * only the mark.
 *
 * Found by the breadcrumb, not by reading: six CI captures showed a thread READY
 * on no run queue with nothing owing it, the tripwire narrowed it to the normal
 * dispatch exit with no enqueue ever attempted, and thread_t::ready_by then named
 * the promoter -- `ready_by=ffffffff80227a12 (sched_wake_thread)`, stranding the
 * ata driver's thread at disp=85. Two earlier hypotheses (the claim consumers,
 * an aborted dispatch) were refuted the same way.
 *
 * The interleaving is stated as a starting state rather than raced for: the
 * blocking transition is published, which is what sched_event_wait does before
 * yielding, and the completion path's decision is then simply not run -- exactly
 * the ordering where it has already declined. */
static void s_a_wake_that_defers_leaves_something_actionable(void) {
    uint32_t pid = 0;
    thread_t* t = 0;

    sched_debug_reset();
    be_cpu(0);
    if (spawn_transition_target(&pid, &t) != 0) {
        CHECK(0, "spawned a target with one worker thread");
        return;
    }

    /* The target is blocked with its transition published -- the state a thread is
     * in between sched_event_wait and its holder's completion handling. */
    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_IPC);
    __atomic_store_n(&t->blocking_transition, 1u, __ATOMIC_SEQ_CST);
    CHECK(t->on_rq == 0, "and not queued");

    sched_wake_thread(t);

    /* The wake must leave the thread ACTIONABLE, by one of the two mechanisms the
     * scheduler has: linked in a ready queue, or carrying an owed-enqueue claim
     * that a settle or the sweep will honour. A bare READY mark is neither -- it
     * is a note left for a reader who may already have gone. */
    printf("  ... after the deferred wake: state=%u on_rq=%u owed=%u wake=%u\n",
           (unsigned)t->state,
           (unsigned)t->on_rq,
           (unsigned)t->enqueue_owed,
           (unsigned)t->wake_pending);
    CHECK(t->on_rq || t->enqueue_owed,
          "a wake that declines to enqueue left a claim rather than only a mark");

    __atomic_store_n(&t->blocking_transition, 0u, __ATOMIC_SEQ_CST);
    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_NONE);
    drop_transition_target(pid);
}

/* Regression: 2026-08-23-settle-destroys-a-claim-it-declines -- a claim consumer
 * that declines to enqueue destroys the claim anyway, so the hand-off it was
 * carrying is lost and no later mechanism can recover it.
 *
 * sched_take_owed_enqueue's own comment states the contract it breaks:
 * "Consume the claim, returning 1 to the single caller that owns the enqueue."
 * Owning the enqueue and then not performing it is the contradiction --
 * sched_settle_deferred_enqueue takes the claim first and only then reads the
 * state, so a thread that is momentarily not enqueueable leaves the consumer
 * holding a debt it discards. Nothing recovers it: sched_sweep_owed_enqueues is
 * gated on g_enqueue_owed_count, which sched_take_owed_enqueue has already
 * decremented.
 *
 * This is the mechanism six CI captures point at, and it is also the hypothesis
 * this investigation refuted IN ERROR (by checking the dispatch exits, which do
 * enqueue unconditionally, rather than the consumers, which do not -- see
 * docs/TASKS.md). What this case pins is the consumer contract itself, which is
 * provable here regardless of how the CI strand is finally shown to arise.
 *
 * The claim is published the way the kernel publishes it -- an enqueue refused
 * because another CPU still names the thread as current -- rather than by poking
 * the field, so the test exercises the real protocol end to end. */
static void s_a_consumer_that_declines_keeps_the_claim(void) {
    uint32_t pid = 0;
    thread_t* t = 0;

    sched_debug_reset();
    be_cpu(0);
    if (spawn_transition_target(&pid, &t) != 0) {
        CHECK(0, "spawned a target with one worker thread");
        return;
    }

    /* Publish a claim through the real path: CPU 1 names the thread as current, so
     * cpu_sched_enqueue refuses to link it and records the debt instead. */
    g_cpus[1].current_thread = t;
    sched_enqueue_thread(t);
    CHECK(t->enqueue_owed == 1, "an enqueue refused for a running thread left a claim");
    CHECK(t->on_rq == 0, "and did not link it");

    /* The thread is no longer enqueueable -- it blocked again, which is the whole
     * reason the consumer has a state test at all. */
    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_IPC);
    g_cpus[1].current_thread = 0;

    sched_settle_deferred_enqueue(t);

    /* The consumer declined, correctly -- a BLOCKED thread must not be linked into
     * a ready queue. What it must NOT do is take the claim with it. Either the
     * thread is queued, or the debt is still outstanding for whoever can honour
     * it; a consumer that leaves neither has silently absorbed a wake. */
    printf("  ... after a declining settle: state=%u on_rq=%u owed=%u\n",
           (unsigned)t->state,
           (unsigned)t->on_rq,
           (unsigned)t->enqueue_owed);
    CHECK(t->on_rq || t->enqueue_owed,
          "a consumer that declined to enqueue left the claim outstanding");

    drop_transition_target(pid);
}

/* ------------------------------------------------------- the kill race soak */

/* Nothing here is serialised against dispatch. Spawn, kill and reap all run
 * concurrently with every dispatcher, which is the point: this suite used to need
 * a park barrier around spawn and reap because a slot could be recycled under a
 * CPU mid-dispatch, and thread_t::dispatch_ref closed that. Re-adding a barrier
 * would hide a regression in it. */

static void* dispatcher_thread(void* arg) {
    uint32_t cpu = (uint32_t)(uintptr_t)arg;
    be_cpu(cpu);
    while (!__atomic_load_n(&g_soak_done, __ATOMIC_ACQUIRE)) {
        (void)process_schedule_once();
    }
    return 0;
}

/* One scheduler loop per CPU other than this thread's (CPU 0), which is what the
 * kernel runs and what a test needs for a reason worth stating: sched_spawn_thread
 * places a new worker on a CPU of its own choosing, and work-stealing in
 * process_schedule_once_impl only fires when cpu_sched_pick_next returned the
 * CPU's IDLE thread. CPU 0's queue holds the target's forever-yielding main
 * thread, so CPU 0 never goes idle and never steals -- a worker placed on CPU 2
 * is unreachable from CPU 0 for as long as the process lives, however many times
 * this thread dispatches. Draining every queue from its own CPU is the fix; it is
 * also simply what the real system does. `upto` is exclusive, so the soak can
 * hold the last CPU back for the killer. */
static pthread_t g_dispatchers[WASMOS_MAX_CPUS];
static uint32_t g_dispatchers_upto;

static void start_dispatchers(uint32_t upto) {
    __atomic_store_n(&g_soak_done, 0u, __ATOMIC_RELEASE);
    g_dispatchers_upto = upto;
    for (uint32_t cpu = 1; cpu < upto; ++cpu) {
        pthread_create(&g_dispatchers[cpu], 0, dispatcher_thread, (void*)(uintptr_t)cpu);
    }
}

static void stop_dispatchers(void) {
    __atomic_store_n(&g_soak_done, 1u, __ATOMIC_RELEASE);
    for (uint32_t cpu = 1; cpu < g_dispatchers_upto; ++cpu) {
        pthread_join(g_dispatchers[cpu], 0);
    }
    g_dispatchers_upto = 0;
}

static void* killer_thread(void* arg) {
    (void)arg;
    be_cpu(KILLER_CPU);
    while (!__atomic_load_n(&g_soak_done, __ATOMIC_ACQUIRE)) {
        uint32_t pid = __atomic_load_n(&g_target_pid, __ATOMIC_ACQUIRE);
        if (pid == 0 || __atomic_load_n(&g_worker_retiring, __ATOMIC_ACQUIRE) == 0u) {
            continue; /* no target, or its worker is not inside the window yet */
        }
        if (process_kill(pid, 0) == 0) {
            __atomic_fetch_add(&g_kills_landed, 1u, __ATOMIC_RELAXED);
        }
        /* Release the worker's spin and stop targeting this pid: one aimed kill
         * per window, not a tight loop on an already-dead process. */
        __atomic_store_n(&g_worker_retiring, 0u, __ATOMIC_RELEASE);
        (void)__atomic_compare_exchange_n(
            &g_target_pid, &pid, 0u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return 0;
}

/* The bug, driven directly. On the unfixed tree a dispatcher reaches
 * process_set_ready (or process_set_running) with the owner already `exiting`
 * and kpanics; the harness's kpanic aborts, so this case does not report a
 * failure, it kills the binary -- which is exactly what it did in CI.
 *
 * The assertions are deterministic: they must hold after ANY interleaving. Only
 * which interleavings get explored varies, which is why the refusal count is
 * asserted non-zero -- otherwise a run that never entered the window would look
 * like a pass. */
static void s_kill_races_the_lifecycle_transitions(void) {
    pthread_t killer;
    uint32_t rounds_run = 0;
    uint32_t spawn_failures = 0;

    sched_debug_reset();
    __atomic_store_n(&g_target_pid, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_worker_retiring, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_soak_done, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kills_landed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_workers_retired, 0u, __ATOMIC_RELEASE);

    /* CPU 0 stays with this thread: it owns spawning and reaping, so the
     * process table has one producer and the soak cannot exhaust it by racing
     * itself. Every other CPU below the killer runs a scheduler loop. */
    be_cpu(0);
    start_dispatchers(KILLER_CPU); /* the last CPU is the killer's */
    pthread_create(&killer, 0, killer_thread, 0);

    /* Bounded by COMPLETED rounds, not by loop iterations. A spawn can fail for
     * want of a recycled slot -- a reap is refused while any thread of that
     * process is still being dispatched, and under heavy oversubscription (more
     * NCPU than host cores, which is the interesting regime) a dispatch holds its
     * reference for a long wall-clock time, so the table runs short in bursts.
     * Counting attempts instead would make the soak's strength depend on the host
     * core count: a 4-core CI runner at NCPU=16 spent 182 of 300 iterations on
     * failed spawns and did only 118 rounds, while an 10-core dev box did 300.
     * The attempt cap is the liveness guard -- if slots never come back this ends
     * rather than spins -- and `spawn_failures` is reported so the throttling
     * stays visible. */
    uint32_t attempts = 0;
    while (rounds_run < KILL_ROUNDS && attempts < KILL_ROUNDS * 16u) {
        uint32_t pid = 0;

        attempts++;
        if (spawn_race_target(&pid) != 0) {
            spawn_failures++;
            (void)process_schedule_once();
            continue;
        }

        __atomic_store_n(&g_worker_retiring, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_target_pid, pid, __ATOMIC_RELEASE);

        /* Dispatch alongside the other CPUs until this target's exiters have all
         * had a turn (or the bound is hit), then stop aiming at this pid. */
        uint32_t retired_before = __atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < DISPATCH_BOUND; ++i) {
            (void)process_schedule_once();
            if (__atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE) - retired_before >=
                EXITERS_PER_TARGET) {
                break;
            }
        }
        __atomic_store_n(&g_target_pid, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_worker_retiring, 0u, __ATOMIC_RELEASE);

        /* Whether or not the killer got there, the round's process must end up
         * gone -- otherwise the table fills and later rounds stop spawning. */
        (void)process_kill(pid, 0);
        for (uint32_t i = 0; i < 16u; ++i) {
            (void)process_schedule_once();
        }
        process_reap_zombie_pid(pid);
        rounds_run++;
    }

    stop_dispatchers();
    pthread_join(killer, 0);

    uint32_t refused_ready = sched_debug_count(SCHED_DEBUG_SET_READY_EXITING);
    uint32_t refused_running = sched_debug_count(SCHED_DEBUG_SET_RUNNING_EXITING);

    printf("  ... cpus=%u rounds=%u spawn_retries=%u retired=%u kills=%u"
           " set_ready_refused=%u set_running_refused=%u\n",
           (unsigned)NCPU,
           rounds_run,
           spawn_failures,
           __atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE),
           __atomic_load_n(&g_kills_landed, __ATOMIC_ACQUIRE),
           refused_ready,
           refused_running);

    /* The soak has to have actually run, or every assertion below is trivially
     * satisfiable. The EVENT counts are the real floor -- retirements and kills are
     * the work this suite is about -- and the round count is only a coarse "the
     * loop made progress" check. Both are set far below what any host produces and
     * exist to catch a soak that silently stopped doing anything.
     *
     * Deliberately not "every round": how many rounds fit depends on the host's
     * core count, not on the kernel. A reap is refused while any thread of that
     * process is still being dispatched, and the fewer real cores back the NCPU
     * pthreads, the longer each dispatch holds its reference in wall-clock terms,
     * so the process table runs short in bursts. Measured on a 10-core host under
     * 5 concurrent instances (~8x oversubscription): 148-300 rounds against
     * 732-1501 retirements. The events stay plentiful; the round count does
     * not. Asserting the round count would make this suite report the runner's
     * spare capacity rather than the kernel's behaviour. */
    CHECK(rounds_run >= KILL_ROUNDS / 4u, "the soak completed a quarter of its rounds");
    CHECK(__atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE) >= 100u,
          "workers retired in quantity");
    CHECK(__atomic_load_n(&g_kills_landed, __ATOMIC_ACQUIRE) >= 20u,
          "the killer landed kills in quantity");

    /* The exploration assertion. Reaching either transition with an owner that is
     * already exiting is the interleaving that used to panic; a non-zero count
     * proves this run entered that window and refused instead of dying, and a
     * zero count means this arm explored nothing and its soak has demonstrated
     * only that the churn does not corrupt anything.
     *
     * Asserted from width 4 up, and REPORTED at width 2, because width 2 does not
     * drive the race hard enough to promise a hit. start_dispatchers(KILLER_CPU)
     * spawns no dispatcher threads at all there -- its loop is `for cpu = 1;
     * cpu < 1` -- so the only dispatcher is this thread, which also owns every
     * spawn, kill and reap and is therefore inside process_schedule_once for a
     * fraction of the run, against a full-time killer. Measured across three CI
     * runs: 0, 2 and 9 refusals at width 2, versus 76-110 at widths 4 and 8. A
     * gate whose verdict is a coin flip reports the runner's scheduling, not the
     * kernel's behaviour, and it already turned a docs-only commit red on main.
     *
     * Nothing is lost by not asserting it here. Widths 4 and 8 assert it on the
     * same code, and the refusal behaviour itself is proved outright by the
     * contract cases above, which cannot miss. What width 2 is really for is the
     * concurrent kill/retire churn against a single dispatcher, and the work
     * counters above are what establish that it ran. */
    if (NCPU >= 4) {
        CHECK(refused_ready + refused_running > 0,
              "a lifecycle transition raced an exit and was refused rather than fatal");
    } else if (refused_ready + refused_running == 0) {
        printf("  ... note: width %u explored no exiting-owner transition this run;"
               " the contract cases cover the branches\n",
               (unsigned)NCPU);
    }
}

/* The same target with no killer: a worker retiring under a healthy owner must
 * still requeue its sibling, and its siblings must still be dispatchable. This
 * is the half the fix must not break -- a set_ready that refused
 * unconditionally would satisfy the case above and silently stop scheduling
 * every multi-threaded process. */
static void s_healthy_owner_still_runs_and_requeues(void) {
    uint32_t pid = 0;

    sched_debug_reset();
    be_cpu(0);
    start_dispatchers(NCPU); /* no killer in this case: every other CPU dispatches */

    CHECK(spawn_race_target(&pid) == 0, "spawned a target with a sibling and a retiring worker");

    uint32_t retired_before = __atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < HEALTHY_DISPATCH_BOUND; ++i) {
        (void)process_schedule_once();
        if (__atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE) != retired_before) {
            break;
        }
    }

    CHECK(__atomic_load_n(&g_workers_retired, __ATOMIC_ACQUIRE) > retired_before,
          "the retiring worker actually ran");

    process_t* proc = process_get(pid);
    CHECK(proc != 0, "the process is still resident");
    if (proc) {
        CHECK(proc->state != PROCESS_STATE_ZOMBIE, "a retiring worker did not kill its owner");
    }
    CHECK(sched_debug_count(SCHED_DEBUG_SET_READY_EXITING) == 0,
          "no ready transition was refused with no kill in flight");
    CHECK(sched_debug_count(SCHED_DEBUG_SET_RUNNING_EXITING) == 0,
          "no dispatch was refused with no kill in flight");
    /* Every dispatch here ends under a healthy owner, so every one of them must
     * leave its thread reachable -- queued, owed an enqueue, or not runnable. This
     * is the invariant a stranded compositor violates in CI, asserted over the
     * hundreds of dispatches this case performs, and it doubles as the guard that
     * the tripwire does not over-report: a false positive on an ordinary dispatch
     * would fail here rather than flooding a CI log. */
    CHECK(sched_debug_count(SCHED_DEBUG_DISPATCH_LEFT_STRANDED) == 0,
          "no dispatch left a runnable thread unreachable under a healthy owner");

    (void)process_kill(pid, 0);
    for (uint32_t i = 0; i < 16u; ++i) {
        (void)process_schedule_once();
    }
    process_reap_zombie_pid(pid);
    stop_dispatchers();
}

int main(void) {
    harness_init();

    /* Deterministic contract cases first: they need no dispatchers, and a
     * failure in one of them explains a soak failure below. */
    s_promotion_never_destroys_a_dispatch_claim();
    s_promotion_wakes_a_blocked_target_and_clears_its_reason();
    s_promotion_of_a_ready_target_is_permitted_and_inert();
    s_exiting_owner_refuses_both_transitions();
    s_aborted_dispatch_leaves_its_thread_reachable();
    s_a_wake_that_defers_leaves_something_actionable();
    s_a_consumer_that_declines_keeps_the_claim();

    s_healthy_owner_still_runs_and_requeues();
    s_kill_races_the_lifecycle_transitions();

    printf("test_process_lifecycle: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
