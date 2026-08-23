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
 * What a passing run means: the window was ENTERED and refused, not merely
 * absent. Both halves of the transition pair are counted
 * (SCHED_DEBUG_SET_READY_EXITING, SCHED_DEBUG_SET_RUNNING_EXITING) and the soak
 * asserts their SUM is non-zero, so a run that never reached the window fails
 * rather than passing vacuously -- the failure mode a race test normally has.
 *
 * Be precise about which half this reliably drives. set_running (the dispatch
 * half) is hit hundreds of times per run at every width: once an owner is
 * exiting, every attempt to dispatch one of its threads lands there. set_ready
 * (the requeue half, the one the CI panic named) is hit only occasionally --
 * 0 or 1 per 300 rounds -- because process_kill marks the owner's threads
 * shortly after setting `exiting`, so the requeue finds no READY sibling and
 * parks instead. Reaching set_ready needs the sub-window where `exiting` is
 * visible but the threads are not yet marked, which process.h:217 describes as
 * "1 slightly ahead of ->state". The sum is therefore what is asserted; the
 * printout reports both so a reader can see which half a given run explored.
 *
 * Either half was a kpanic before the fix, so the suite is red-to-green on the
 * family: with the guards restored it aborts with "set_running zombie" at every
 * width (verified at 2 and 4 CPUs, exit 134).
 *
 * The assertions themselves are deterministic: they must hold after ANY
 * interleaving. Only which interleavings get explored varies.
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

    /* The decisive assertion. Reaching either transition with an owner that is
     * already exiting is the interleaving that used to panic; a non-zero count
     * proves this run entered that window and refused instead of dying, and a
     * zero count means the soak never got there and has demonstrated nothing. */
    CHECK(refused_ready + refused_running > 0,
          "a lifecycle transition raced an exit and was refused rather than fatal");
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

    (void)process_kill(pid, 0);
    for (uint32_t i = 0; i < 16u; ++i) {
        (void)process_schedule_once();
    }
    process_reap_zombie_pid(pid);
    stop_dispatchers();
}

int main(void) {
    harness_init();

    s_healthy_owner_still_runs_and_requeues();
    s_kill_races_the_lifecycle_transitions();

    printf("test_process_lifecycle: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
