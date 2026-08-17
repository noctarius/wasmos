/* test_sched_runqueue.c — host-side invariant tests for the real sched_thread.c.
 *
 * This drives the ACTUAL scheduler source, not a model of it, with the host
 * standing in for the CPUs: cpu_local() resolves through g_host_cpu_local (see
 * the WASMOS_HOST_TEST_SMP arm in arch/x86_64/smp.h), so act_as(n) makes the
 * following calls run "on CPU n".  That gives deterministic multi-CPU coverage
 * with no threads, which is what a unit gate wants -- every ordering here is
 * one the kernel can actually produce, just chosen rather than raced for.
 *
 * The centrepiece is check_invariants(): run-queue corruption is a structural
 * violation that a walk of the queues catches at the moment it happens, whereas
 * on hardware it only surfaces later as a wedged picker.  Each regression test
 * below reproduces a bug that reached main; each rejection test pins an input
 * the API must refuse.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "sched.h"
#include "thread.h"
#include "arch/x86_64/smp.h"

/* ------------------------------------------------------------------ harness */

/* The CPU table the linked scheduler sees, and the three symbols it resolves it through.
 *
 * cpu_local() is the WASMOS_HOST_TEST_SMP arm in arch/x86_64/smp.h, which reads
 * g_host_cpu_local -- a pointer into g_cpus[]. Everything here runs on one thread, so
 * act_as(n) simply re-points it and the following calls execute "on CPU n".
 *
 * g_cpu_count is what the scheduler treats as the online CPU count: it bounds the steal
 * scan and is the modulus of both placement pickers. harness_reset() puts it back to 4;
 * the degenerate-count cases mutate it and restore it themselves. Being ONLINE is a
 * further condition on top of it: cpu_sched_online_mask walks entries below g_cpu_count
 * and admits only those with started != 0, with CPU 0 always in. That mask is what
 * affinity is tested against, which is why harness_reset sets started on every entry. */
cpu_local_t g_cpus[WASMOS_MAX_CPUS];
uint32_t g_cpu_count = 4;
_Thread_local cpu_local_t* g_host_cpu_local;

#define POOL_MAX 32
static thread_t g_pool[POOL_MAX];

static int g_failures;
static int g_checks;

/* Captured tripwire output.  The scheduler reports invariant violations rather
 * than halting, so tests assert on what it said.
 *
 * Both writers are captured, and WHICH one a report arrived through is itself
 * asserted (case D7).  serial_printf takes the serial spinlock; the _unlocked
 * form does not, so a concurrent writer on another CPU interleaves mid-string
 * and corrupts both lines -- which on a real boot has cost a test marker that
 * the harness matches byte for byte.  A tripwire fires on a live system, not
 * from a fault handler, so it must use the locking writer. */
#define LOG_MAX 64
#define LOG_LINE 256
static char g_log[LOG_MAX][LOG_LINE];
static int g_log_count;
static int g_log_unlocked_count;

static void log_capture(const char* fmt, va_list ap) {
    if (g_log_count < LOG_MAX) {
        vsnprintf(g_log[g_log_count], LOG_LINE, fmt, ap);
        g_log_count++;
    }
}

void serial_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_capture(fmt, ap);
    va_end(ap);
}

void serial_printf_unlocked(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_log_unlocked_count++;
    log_capture(fmt, ap);
    va_end(ap);
}

/* Counting, so the priority-preemption arm of sched_wake_thread is observable. */
static int g_resched_requests;
void process_set_need_resched(void) {
    g_resched_requests++;
}

/* sched_thread_init initialises the thread's join_event.  Mirrored rather than
 * linked: the real sched_event.c drags in the timer and thread table, and no
 * test here exercises events. */
void sched_event_init(sched_event_t* ev, sched_event_type_t type) {
    ksync_spinlock_init(&ev->lock);
    list_head_init(&ev->wait_list);
    ev->cnt = 0;
    ev->type = type;
}

/* The CAS half of thread.c's thread_transit, with the same acquire/release
 * semantics: the scheduler relies on LOSING this race, not on being the only
 * writer, so a stub that always succeeds would hide exactly the bugs these tests
 * exist for.  thread.c also gates the edge through thread_transition_legal();
 * every edge the scheduler attempts here (RUNNING/BLOCKED -> READY) is legal
 * there, so the bare CAS is faithful for this harness. */
int thread_transit(thread_t* t, thread_state_t from, thread_state_t to) {
    uint32_t expected = (uint32_t)from;
    if (!t) {
        return 0;
    }
    return __atomic_compare_exchange_n(
        (uint32_t*)&t->state, &expected, (uint32_t)to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Mirrors thread.c: the tid==0 guard, the BLOCKED-only precondition and the
 * block_reason clear.  An approximation here is worse than no test -- a stub
 * that only transitioned state fails W1's "block reason cleared" assertion
 * against a correct kernel. */
int thread_wake_if_blocked(uint32_t tid) {
    if (tid == 0) {
        return 0;
    }
    for (int i = 0; i < POOL_MAX; ++i) {
        if (g_pool[i].tid != tid) {
            continue;
        }
        thread_t* t = &g_pool[i];
        if (t->state != THREAD_STATE_BLOCKED) {
            return 0;
        }
        t->state = THREAD_STATE_READY;
        t->block_reason = THREAD_BLOCK_NONE;
        return 1;
    }
    return 0;
}

/* The scheduler's deferred-enqueue sweep walks the thread table, which this
 * harness owns rather than links: g_pool IS the table here.  Mirrors the real
 * contract -- an in-range slot is returned whatever its state, out of range is
 * NULL -- so the sweep sees exactly the slots a case built. */
thread_t* thread_table_at(uint32_t index) {
    if (index >= POOL_MAX) {
        return 0;
    }
    return &g_pool[index];
}

/* Run every subsequent call as `cpu`. The choice is sticky until the next act_as() or
 * harness_reset() (which returns to CPU 0), which matters because several helpers here
 * -- pick_on in particular -- leave it changed. `cpu` is not bounds-checked and must be
 * below WASMOS_MAX_CPUS. */
static void act_as(uint32_t cpu) {
    g_host_cpu_local = &g_cpus[cpu];
}

/* Whether any captured tripwire line contains `needle`, matched as a case-sensitive
 * substring -- so a needle for a hex value must be spelled the way the scheduler formats
 * it. Only lines captured since the last log_reset() are searched, and only the first
 * LOG_MAX of them: once the buffer fills, serial_printf_unlocked stops recording, and a
 * later report would go unseen. */
static int saw(const char* needle) {
    for (int i = 0; i < g_log_count; ++i) {
        if (strstr(g_log[i], needle)) {
            return 1;
        }
    }
    return 0;
}

/* Discard the captured lines so a case can assert on what one operation reported. It
 * does not touch the sched_debug counters -- harness_reset clears those through
 * sched_debug_reset -- and the two are asserted on separately: the counter is the
 * contract, since a tripwire LINE is rate-limited to powers of two. */
static void log_reset(void) {
    g_log_count = 0;
}

/* Counts every evaluation into g_checks and, on failure, counts g_failures and prints
 * `msg` with the file and line. A failed check does NOT end the case: the remaining
 * assertions still run, against the state the failure left behind, and check_invariants()
 * reports into the same counters without the macro. main prints the totals and exits
 * non-zero if any check failed. `msg` names the property being asserted, in the
 * affirmative -- it is printed when that property does not hold. */
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ fixtures */

/* Mimics thread_reset_slot: the node is left CANONICALLY DETACHED.  A zero-fill
 * is not enough -- list_head_empty() tests next == head, so a NULL next reads as
 * LINKED, and the linkage tripwires then report a fresh slot as still queued. */
static thread_t* mk_thread(int idx, sched_prio_t prio, thread_state_t state) {
    thread_t* t = &g_pool[idx];
    memset(t, 0, sizeof(*t));
    t->tid = (uint32_t)idx + 1u;
    t->owner_pid = 100u + (uint32_t)idx;
    list_head_init(&t->sched_node);
    sched_thread_init(t, prio);
    t->state = state;
    return t;
}

/* Per-case fixture reset, called first by every case. Zeroes the CPU table and the
 * thread pool, re-initialises every queue, marks every CPU online, installs a per-CPU
 * idle thread, clears the captured log, the tripwire counters and the placement
 * round-robin cursors, zeroes the resched counter, restores g_cpu_count to 4, and leaves
 * the caller acting as CPU 0.
 *
 * It does NOT clear g_checks or g_failures: those are cumulative for the run and are
 * what main reports. Threads are not handed out here either -- a case builds the ones it
 * needs with mk_thread(), which recycles pool slots by index. */
static void harness_reset(void) {
    memset(g_cpus, 0, sizeof(g_cpus));
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        cpu_sched_init(&g_cpus[i].sched);
        g_cpus[i].cpu_id = i;
        /* Mark every CPU online.  cpu_sched_online_mask() counts only CPUs with
         * started != 0, and affinity treats "mask names no online CPU" as no
         * constraint -- so leaving this clear makes every affinity test pass
         * vacuously through the fallback rather than exercising the rule. */
        g_cpus[i].started = 1;
    }
    g_cpu_count = 4;
    /* Per-CPU idle threads, as the real bringup installs them.  They live
     * outside the pool so they are never confused with test threads. */
    static thread_t idle[WASMOS_MAX_CPUS];
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        memset(&idle[i], 0, sizeof(idle[i]));
        idle[i].tid = 9000u + i;
        list_head_init(&idle[i].sched_node);
        sched_thread_init(&idle[i], SCHED_PRIO_IDLE);
        idle[i].state = THREAD_STATE_READY;
        g_cpus[i].idle_thread = &idle[i];
        g_cpus[i].sched.idle = &idle[i];
    }
    log_reset();
    sched_debug_reset(); /* tripwire counters AND the placement round-robin cursors */
    g_resched_requests = 0;
    act_as(0);
}

/* -------------------------------------------------------- invariant checker */

/* Walks every band of every CPU and asserts the structural facts the run queue
 * must always satisfy.  The walk is bounded so a cycle -- what a band spliced
 * through a two-owner node produces -- reports and terminates instead of hanging
 * the test.  The I<n> labels below name invariants, not the I<n> test cases in
 * the registration table. */
static void check_invariants(const char* where) {
    int seen_count[POOL_MAX + 1];
    memset(seen_count, 0, sizeof(seen_count));

    for (uint32_t c = 0; c < g_cpu_count; ++c) {
        cpu_sched_t* cs = &g_cpus[c].sched;
        for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
            list_head_t* head = &cs->ready_list[p];
            uint32_t walked = 0;
            int bit = (cs->ready_bitmap & (1u << p)) != 0;

            for (list_head_t* n = head->next; n != head; n = n->next) {
                if (++walked > POOL_MAX + 2u) {
                    g_failures++;
                    printf("  [FAIL] %s: cycle in cpu%u band%d\n", where, c, p);
                    break;
                }
                /* I6: doubly-linked consistency. */
                if (n->next->prev != n || n->prev->next != n) {
                    g_failures++;
                    printf("  [FAIL] %s: broken links cpu%u band%d\n", where, c, p);
                    break;
                }
                thread_t* t = list_entry(n, thread_t, sched_node);
                if (t->tid >= 1u && t->tid <= POOL_MAX) {
                    seen_count[t->tid]++;
                }
                /* I7: only READY threads belong in a ready queue. */
                if (t->state != THREAD_STATE_READY) {
                    g_failures++;
                    printf("  [FAIL] %s: queued non-READY tid=%u state=%u\n",
                           where,
                           t->tid,
                           (unsigned)t->state);
                }
                /* I2/I3: the claim and its queue pointer must agree with linkage. */
                if (!t->on_rq) {
                    g_failures++;
                    printf("  [FAIL] %s: linked but unclaimed tid=%u\n", where, t->tid);
                }
                if (t->rq != cs) {
                    g_failures++;
                    printf("  [FAIL] %s: rq mismatch tid=%u\n", where, t->tid);
                }
            }
            /* I4: the ready bit must track list emptiness, not the counter. */
            if (bit != (walked != 0)) {
                g_failures++;
                printf("  [FAIL] %s: cpu%u band%d bit=%d walked=%u\n", where, c, p, bit, walked);
            }
            /* I5: the counter is a statistic, but must still match the list. */
            if (cs->thread_count[p] != walked) {
                g_failures++;
                printf("  [FAIL] %s: cpu%u band%d count=%u walked=%u\n",
                       where,
                       c,
                       p,
                       cs->thread_count[p],
                       walked);
            }
        }
    }

    /* Bit 7 is not a band: cpu_sched_highest_prio masks it away, so a set bit
     * there is unreachable state that would index past the end of ffs_table's
     * 128 entries if the mask were ever dropped. */
    for (uint32_t c = 0; c < g_cpu_count; ++c) {
        if ((g_cpus[c].sched.ready_bitmap & 0x80u) != 0u) {
            g_failures++;
            printf("  [FAIL] %s: cpu%u has bit 7 set in ready_bitmap\n", where, c);
        }
        if (g_cpus[c].sched.last_dispatched_prio > SCHED_PRIO_IDLE) {
            g_failures++;
            printf("  [FAIL] %s: cpu%u last_dispatched_prio=%u out of range\n",
                   where,
                   c,
                   (unsigned)g_cpus[c].sched.last_dispatched_prio);
        }
    }

    /* An idle thread must never be reachable from a ready list -- it is
     * dispatched only through the pick_next fallback, so a linked one could be
     * running as both the fallback and a queued thread. */
    for (uint32_t c = 0; c < g_cpu_count; ++c) {
        for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
            list_head_t* head = &g_cpus[c].sched.ready_list[p];
            for (list_head_t* n = head->next; n != head; n = n->next) {
                if (list_entry(n, thread_t, sched_node)->tid >= 9000u) {
                    g_failures++;
                    printf("  [FAIL] %s: idle thread linked on cpu%u band%d\n", where, c, p);
                }
            }
        }
    }

    /* I1: a thread is in at most one queue, exactly once. */
    for (int i = 1; i <= POOL_MAX; ++i) {
        if (seen_count[i] > 1) {
            g_failures++;
            printf("  [FAIL] %s: tid=%d linked %d times\n", where, i, seen_count[i]);
        }
        thread_t* t = &g_pool[i - 1];
        if (t->tid == (uint32_t)i && t->on_rq && seen_count[i] == 0) {
            g_failures++;
            printf("  [FAIL] %s: tid=%d claimed but on no queue\n", where, i);
        }
    }
    g_checks++;
}

/* Dispatch one thread on `cpu` and put it straight back, which is what a
 * fairness or ordering test wants: the band composition stays fixed so only the
 * dispatch ORDER is under test. Returns what was dispatched. */
static thread_t* pick_and_requeue(uint32_t cpu);

/* Dispatch one thread from `cpu`'s queue, taking and releasing that queue's lock exactly
 * as the scheduler loop does. The caller is switched to `cpu` for the call and STAYS
 * there afterwards, which is load-bearing: cpu_sched_pick_next answers its idle fallback
 * from cpu_local(), so picking on a remote queue returns the CALLER's idle thread.
 * Returns whatever pick_next returned -- a thread, an idle thread, or NULL when no idle
 * is installed. */
static thread_t* pick_on(uint32_t cpu) {
    act_as(cpu);
    cpu_sched_t* cs = &g_cpus[cpu].sched;
    ksync_spinlock_lock(&cs->lock);
    thread_t* t = cpu_sched_pick_next(cs);
    ksync_spinlock_unlock(&cs->lock);
    return t;
}

static thread_t* pick_and_requeue(uint32_t cpu) {
    thread_t* t = pick_on(cpu);
    if (t && t != g_cpus[cpu].idle_thread) {
        t->state = THREAD_STATE_READY;
        cpu_sched_enqueue(&g_cpus[cpu].sched, t);
    }
    return t;
}

/* ------------------------------------------------------------ positive cases */

static void test_enqueue_pick_roundtrip(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    check_invariants("enqueue");
    CHECK(t->on_rq == 1, "enqueue claims the thread");
    CHECK(t->rq == &g_cpus[0].sched, "enqueue publishes rq");
    CHECK((g_cpus[0].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) != 0, "band bit set");

    thread_t* got = pick_on(0);
    CHECK(got == t, "pick returns the enqueued thread");
    CHECK(t->on_rq == 0, "pick releases the claim");
    check_invariants("after pick");
    CHECK((g_cpus[0].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) == 0, "band bit cleared");
}

static void test_priority_and_fifo(void) {
    harness_reset();
    thread_t* low = mk_thread(0, SCHED_PRIO_BACKGROUND, THREAD_STATE_READY);
    thread_t* hi1 = mk_thread(1, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    thread_t* hi2 = mk_thread(2, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, low);
    cpu_sched_enqueue(&g_cpus[0].sched, hi1);
    cpu_sched_enqueue(&g_cpus[0].sched, hi2);
    check_invariants("three queued");

    CHECK(pick_on(0) == hi1, "highest priority first, FIFO within band");
    CHECK(pick_on(0) == hi2, "second of the high band");
    CHECK(pick_on(0) == low, "lower band last");
    check_invariants("drained");
}

static void test_empty_queue_yields_per_cpu_idle(void) {
    harness_reset();
    CHECK(pick_on(0) == g_cpus[0].idle_thread, "cpu0 gets its own idle");
    CHECK(pick_on(1) == g_cpus[1].idle_thread, "cpu1 gets its own idle");
    CHECK(g_cpus[0].idle_thread != g_cpus[1].idle_thread, "idle threads are per-CPU");
}

static void test_remove_thread_crosses_cpus(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(2);
    cpu_sched_enqueue(&g_cpus[2].sched, t);
    check_invariants("queued on cpu2");

    /* The reap path runs wherever the reaper happens to be, and must follow
     * t->rq to the owning queue rather than assume the local one. */
    act_as(0);
    cpu_sched_remove_thread(t);
    check_invariants("removed from cpu0");
    CHECK(t->on_rq == 0, "claim released by remove");
    CHECK(list_head_empty(&t->sched_node), "node detached by remove");
    CHECK(g_cpus[2].sched.thread_count[SCHED_PRIO_WASM] == 0, "owning queue count drops");
}

static void test_remove_thread_not_queued_is_noop(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_remove_thread(t);
    cpu_sched_remove_thread(NULL);
    check_invariants("noop removes");
    CHECK(t->on_rq == 0, "still unclaimed");
}

/* ---------------------------------- Kind 1: bugs that reached main */

/* Regression (aa3db5c160).  process_thread_spawn_worker_internal published a
 * thread READY before its sched_node existed; a wake in that window enqueued
 * it, and sched_thread_init's list_head_init then self-linked the node while
 * the band still pointed at it -- the ghost that gets re-picked forever. */
static void test_init_on_queued_is_reported(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    log_reset();

    sched_thread_init(t, SCHED_PRIO_WASM); /* the corrupting write */
    CHECK(sched_debug_count(SCHED_DEBUG_INIT_ON_QUEUED) == 1,
          "re-init of a queued node is counted");
}

/* Regression (10f5f3eaa1).  thread_reset_slot freed a slot whose sched_node was
 * still linked; the allocator handed it to the next spawn and the band was
 * spliced through a node with two owners.  cpu_sched_remove_thread exists to
 * make the unlink precede the reset -- this asserts recycling is then clean. */
static void test_recycled_slot_is_unlinked_first(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    cpu_sched_remove_thread(t); /* what thread_reset_slot must do first */
    log_reset();
    thread_t* reborn = mk_thread(0, SCHED_PRIO_SERVICE, THREAD_STATE_READY);
    CHECK(sched_debug_count(SCHED_DEBUG_INIT_ON_QUEUED) == 0,
          "a properly unlinked slot re-inits silently");
    cpu_sched_enqueue(&g_cpus[0].sched, reborn);
    check_invariants("recycled slot requeued");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 0, "old band left empty");
}

/* on_rq and linkage must agree: a linked node is claimed, and a remove of some
 * OTHER thread may not clear this one's claim.  The pair matters because a node
 * that is linked while unclaimed is linkable a second time -- the shape produced
 * when the claim was taken before t->rq was published and remove_thread read
 * rq == 0 as "not queued".  The in-flight (on_rq=1, rq=0) window itself is
 * pinned by R1, not here. */
static void test_claim_and_linkage_never_disagree(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->on_rq == 1 && !list_head_empty(&t->sched_node), "linked implies claimed");

    /* remove_thread must never write on_rq for a thread it did not unlink. */
    thread_t* other = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[1].sched, other);
    act_as(3);
    cpu_sched_remove_thread(other);
    CHECK(t->on_rq == 1, "unrelated remove leaves this claim intact");
    check_invariants("claim integrity");
}

/* The counter underflowed past zero, so ready_bitmap could never clear and the
 * picker was wedged on a band forever.  The bit is now derived from list
 * emptiness, so a drifted counter must not be able to wedge anything. */
static void test_drifted_counter_cannot_wedge_a_band(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] = 7; /* forced drift */

    CHECK(pick_on(0) == t, "still dispatches");
    CHECK((g_cpus[0].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) == 0,
          "band goes idle despite the bogus counter");
    CHECK(pick_on(0) == g_cpus[0].idle_thread, "picker is not wedged on the band");
}

/* list_head_empty() tests next == head, so a zero-filled node (next == NULL)
 * reads as LINKED.  Every "is this thread queued?" answer depends on
 * thread_reset_slot leaving the node canonically detached instead. */
static void test_zero_filled_node_reads_as_linked(void) {
    harness_reset();
    thread_t raw;
    memset(&raw, 0, sizeof(raw));
    CHECK(!list_head_empty(&raw.sched_node), "zero-filled node is NOT detached");
    list_head_init(&raw.sched_node);
    CHECK(list_head_empty(&raw.sched_node), "list_head_init makes it detached");
}

/* ------------------------------------- Kind 2: inputs the API must refuse */

static void test_enqueue_refuses_non_ready(void) {
    const thread_state_t bad[] = {
        THREAD_STATE_BLOCKED, THREAD_STATE_ZOMBIE, THREAD_STATE_RUNNING, THREAD_STATE_NEW};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, bad[i]);
        cpu_sched_enqueue(&g_cpus[0].sched, t);
        CHECK(list_head_empty(&t->sched_node), "non-READY thread is not linked");
        CHECK(t->on_rq == 0, "non-READY thread is not claimed");
        /* The log LINE is not a contract: the tripwire is rate-limited to
         * powers of two, so within one uninterrupted run of refusals only the
         * 1st, 2nd, 4th, 8th ... print -- otherwise the storm this guards
         * against would flood serial at scheduler speed.  The COUNTER is the
         * contract, and harness_reset() zeroes it through sched_debug_reset(),
         * so every refusal must show up as exactly one count. */
        CHECK(sched_debug_count(SCHED_DEBUG_ENQUEUE_NON_READY) == 1,
              "the refusal is counted -- every time, not just when the rate limiter allows a line");
        check_invariants("after refused enqueue");
    }
}

static void test_double_enqueue_links_once(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    cpu_sched_enqueue(&g_cpus[0].sched, t); /* same CPU */
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, t); /* and a different one */
    check_invariants("double enqueue");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 1, "counted once");
    CHECK(g_cpus[1].sched.thread_count[SCHED_PRIO_WASM] == 0, "not stolen into cpu1");
}

static void test_pick_drops_stale_nodes_and_falls_back(void) {
    harness_reset();
    thread_t* a = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* b = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, a);
    cpu_sched_enqueue(&g_cpus[0].sched, b);
    /* Tombstoned while queued, which a concurrent reap does. */
    a->state = THREAD_STATE_ZOMBIE;
    b->state = THREAD_STATE_UNUSED;

    CHECK(pick_on(0) == g_cpus[0].idle_thread, "band of stale nodes yields idle");
    CHECK((g_cpus[0].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) == 0, "stale band cleared");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 0, "stale nodes dropped");
}

static void test_null_inputs_are_safe(void) {
    harness_reset();
    cpu_sched_enqueue(&g_cpus[0].sched, NULL);
    cpu_sched_remove_thread(NULL);
    check_invariants("null inputs");
}

/* ------------------------------------------------------------------- steal */

static void test_steal_takes_work_and_respects_exclusions(void) {
    harness_reset();
    thread_t* victim = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, victim);

    act_as(0);
    thread_t* stolen = cpu_sched_try_steal(0);
    CHECK(stolen == victim, "steal pulls work off a loaded remote");
    check_invariants("after steal");
    CHECK(stolen != NULL && stolen->on_rq == 0, "stolen thread is unclaimed and off-queue");

    /* Nothing left anywhere: steal must report empty, not invent work. */
    CHECK(cpu_sched_try_steal(0) == NULL, "steal returns NULL when all remotes are empty");
}

static void test_steal_skips_sticky_and_idle(void) {
    harness_reset();
    thread_t* poller = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    poller->sched_sticky = 1;
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, poller);

    act_as(0);
    CHECK(cpu_sched_try_steal(0) == NULL, "a sticky poller stays on its home CPU");
    CHECK(poller->on_rq == 1, "and remains queued there");
    check_invariants("sticky preserved");
}

/* --------------------------------------------------------- cpu_affinity */

/* Affinity has three enforcement points -- placement, the enqueue redirect and
 * the steal scan -- and they must agree.  With only placement honouring the
 * mask, enqueue parks a woken thread on the CALLING CPU and steal moves it
 * anywhere, so the mask set at spawn is silently overridden the first time the
 * thread blocks or an idle CPU goes looking for work. */

static void test_affinity_governs_placement(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 2;
    CHECK(cpu_sched_pick_target_cpu_for_thread(t, 0) == 2, "placement picks an allowed CPU");
}

static void test_affinity_redirects_a_forbidden_enqueue(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 3;

    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t); /* the waker's CPU, which is forbidden */
    check_invariants("redirected enqueue");
    CHECK(t->rq == &g_cpus[3].sched, "enqueue redirected to an allowed CPU");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 0, "forbidden queue untouched");
    CHECK(t->on_rq == 1, "and the thread is still queued, not dropped");
}

static void test_affinity_blocks_a_steal(void) {
    harness_reset();
    thread_t* pinned = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    pinned->cpu_affinity = 1u << 1;
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, pinned);

    act_as(0);
    CHECK(cpu_sched_try_steal(0) == NULL, "CPU0 cannot steal a thread pinned to CPU1");
    CHECK(pinned->on_rq == 1, "the pinned thread stays queued where it belongs");
    check_invariants("steal blocked by affinity");
    /* CPU1 itself may still take it. */
    CHECK(pick_on(1) == pinned, "its own CPU still dispatches it");
}

static void test_affinity_naming_no_online_cpu_is_not_a_constraint(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 15; /* nothing online matches */
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->on_rq == 1, "an unsatisfiable mask must not strand the thread");
    check_invariants("unsatisfiable affinity");
}

/* ------------------------------------------------------------- priority */

static void test_antistarvation_demotes_after_streak(void) {
    harness_reset();
    /* A high band with more work than the streak allows, plus a waiting low band. */
    for (int i = 0; i < SCHED_ANTISTARVATION_STREAK + 2; ++i) {
        cpu_sched_enqueue(&g_cpus[0].sched, mk_thread(i, SCHED_PRIO_DRIVER, THREAD_STATE_READY));
    }
    thread_t* low = mk_thread(20, SCHED_PRIO_BACKGROUND, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, low);

    int saw_low = 0;
    for (int i = 0; i < SCHED_ANTISTARVATION_STREAK + 2 && !saw_low; ++i) {
        if (pick_on(0) == low) {
            saw_low = 1;
        }
    }
    CHECK(saw_low, "a lower band gets a slot instead of starving");
    check_invariants("anti-starvation");
}

/* The streak describes what THIS CPU dispatched.  Held globally it advanced N
 * times too fast on an N-CPU machine and let one CPU decide another's band --
 * besides being a plain byte written from every CPU under different locks. */
static void test_antistarvation_state_is_per_cpu(void) {
    harness_reset();
    for (int i = 0; i < SCHED_ANTISTARVATION_STREAK + 2; ++i) {
        cpu_sched_enqueue(&g_cpus[0].sched, mk_thread(i, SCHED_PRIO_DRIVER, THREAD_STATE_READY));
        pick_on(0);
    }
    thread_t* hi = mk_thread(20, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    thread_t* lo = mk_thread(21, SCHED_PRIO_BACKGROUND, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, hi);
    cpu_sched_enqueue(&g_cpus[1].sched, lo);
    CHECK(pick_on(1) == hi, "CPU1's first dispatch is unaffected by CPU0's streak");
    CHECK(g_cpus[0].sched.high_prio_streak != g_cpus[1].sched.high_prio_streak ||
              g_cpus[1].sched.high_prio_streak == 0,
          "streaks are tracked separately");
}

/* --------------------------------------------- more run-queue regressions */

/* sched_thread_init assigned the new priority BEFORE unlinking, so the unlink
 * was accounted against the new band while the node sat in the old one.  The old
 * band kept its ready bit over an empty list, which cpu_sched_highest_prio then
 * selected forever -- the CPU returned idle on every dispatch with runnable work
 * outstanding.  Same wedge shape as the storm. */
static void test_reinit_at_a_new_priority_drains_the_old_band(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    sched_thread_init(t, SCHED_PRIO_BACKGROUND); /* recycled slot, different band */

    cpu_sched_t* cs = &g_cpus[0].sched;
    CHECK((cs->ready_bitmap & (1u << SCHED_PRIO_DRIVER)) == 0, "old band's ready bit cleared");
    CHECK(cs->thread_count[SCHED_PRIO_DRIVER] == 0, "old band's counter drained");
    check_invariants("reinit at new prio");

    thread_t* other = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, other);
    CHECK(pick_on(0) == other, "picker is not wedged on a phantom band");
}

/* A band holding only stale nodes must not send the CPU to idle while a lower
 * band has runnable work. */
static void test_stale_band_does_not_mask_lower_bands(void) {
    harness_reset();
    thread_t* dead = mk_thread(0, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    thread_t* live = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, dead);
    cpu_sched_enqueue(&g_cpus[0].sched, live);
    dead->state = THREAD_STATE_ZOMBIE; /* reaped while queued */

    CHECK(pick_on(0) == live, "lower band is dispatched, not idle");
    check_invariants("stale band swept");
}

/* ------------------------------------------------- band / bitmap coverage */

/* B1: exhaustive band selection.  ffs_table is a static lookup, so the only way
 * to cover it end-to-end is to drive every reachable bitmap value: all 127
 * non-empty subsets of the seven bands, each asserting the lowest-indexed
 * (highest-priority) occupied band wins.  One pick per subset, so the
 * anti-starvation streak never engages and cannot skew the expectation. */
static void test_exhaustive_band_selection(void) {
    int wrong = 0;
    unsigned first_bad = 0;
    for (unsigned mask = 1; mask < 128u; ++mask) {
        harness_reset();
        thread_t* lowest = NULL;
        for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
            if (!(mask & (1u << p))) {
                continue;
            }
            thread_t* t = mk_thread(p, (sched_prio_t)p, THREAD_STATE_READY);
            cpu_sched_enqueue(&g_cpus[0].sched, t);
            if (!lowest) {
                lowest = t; /* p ascends, so the first inserted is the lowest band */
            }
        }
        if (g_cpus[0].sched.ready_bitmap != (uint8_t)mask) {
            wrong++;
            if (!first_bad) {
                first_bad = mask;
            }
            continue;
        }
        if (pick_on(0) != lowest) {
            wrong++;
            if (!first_bad) {
                first_bad = mask;
            }
        }
    }
    if (wrong) {
        printf("  (first failing subset: 0x%02x, %d of 127 wrong)\n", first_bad, wrong);
    }
    CHECK(wrong == 0, "every one of the 127 band subsets selects its highest priority");
}

/* B2: the bitmap is exactly the occupied set, by value -- not merely non-zero. */
static void test_bitmap_equals_the_occupied_set(void) {
    harness_reset();
    thread_t* b2 = mk_thread(0, (sched_prio_t)2, THREAD_STATE_READY);
    thread_t* b4 = mk_thread(1, (sched_prio_t)4, THREAD_STATE_READY);
    thread_t* b6 = mk_thread(2, (sched_prio_t)6, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, b2);
    cpu_sched_enqueue(&g_cpus[0].sched, b4);
    cpu_sched_enqueue(&g_cpus[0].sched, b6);
    CHECK(g_cpus[0].sched.ready_bitmap == 0x54u, "bands {2,4,6} give 0b1010100");

    cpu_sched_remove_thread(b4); /* drain the middle band only */
    CHECK(g_cpus[0].sched.ready_bitmap == 0x44u, "draining band 4 gives 0b1000100");
    check_invariants("bitmap value");
}

/* B3: bit 7 is not a band.  cpu_sched_highest_prio masks with 0x7F, so a set
 * bit 7 must be ignored rather than indexing ffs_table[128] -- a one-past-the-end
 * read.  This pins the mask so a refactor cannot quietly drop it. */
static void test_bit7_never_indexes_the_table(void) {
    harness_reset();
    g_cpus[0].sched.ready_bitmap = 0x80u; /* all lists empty */
    CHECK(pick_on(0) == g_cpus[0].idle_thread, "bit 7 is ignored, not dispatched");
}

/* B4: a PHANTOM bit -- a band marked occupied whose list is already empty -- is a
 * different entry into the wedge than a stale node: there is nothing to sweep, so
 * a picker that only advances by draining loops back to the same band forever. */
static void test_phantom_bit_does_not_wedge_the_picker(void) {
    harness_reset();
    thread_t* live = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, live);
    g_cpus[0].sched.ready_bitmap |= (uint8_t)(1u << SCHED_PRIO_DRIVER); /* empty band */

    CHECK(pick_on(0) == live, "runnable work below a phantom bit is dispatched");
    CHECK((g_cpus[0].sched.ready_bitmap & (1u << SCHED_PRIO_DRIVER)) == 0,
          "the phantom bit is cleared, not left to re-wedge");
    check_invariants("phantom bit cleared");
}

/* B5: draining everything leaves no residue in either the bitmap or the counters. */
static void test_draining_every_band_leaves_no_residue(void) {
    harness_reset();
    for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
        cpu_sched_enqueue(&g_cpus[0].sched, mk_thread(p, (sched_prio_t)p, THREAD_STATE_READY));
    }
    for (int i = 0; i < SCHED_PRIO_MAX; ++i) {
        CHECK(pick_on(0) != g_cpus[0].idle_thread, "each of the 7 picks dispatches real work");
    }
    CHECK(g_cpus[0].sched.ready_bitmap == 0u, "bitmap fully cleared");
    int counters_clear = 1;
    for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
        if (g_cpus[0].sched.thread_count[p] != 0u) {
            counters_clear = 0;
        }
    }
    CHECK(counters_clear, "every band counter back to zero");
    CHECK(pick_on(0) == g_cpus[0].idle_thread, "and the CPU then idles");
    check_invariants("fully drained");
}

/* --------------------------------------- enqueue: running-elsewhere scan */

/* cpu_sched_enqueue first scans for the thread being some CPU's current_thread.
 * Such a thread must NOT be linked -- it is still executing -- but must be left
 * READY so the owning CPU re-enqueues it when its yield completes. */

static void test_enqueue_running_elsewhere_marks_ready_only(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    t->block_reason = THREAD_BLOCK_IPC;
    g_cpus[2].current_thread = t;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    CHECK(t->state == THREAD_STATE_READY, "promoted to READY");
    CHECK(t->block_reason == THREAD_BLOCK_NONE, "block reason cleared");
    CHECK(list_head_empty(&t->sched_node), "not linked while running elsewhere");
    CHECK(t->on_rq == 0, "not claimed");
    CHECK(saw("enqueue current"), "reported");
    check_invariants("running elsewhere");
}

static void test_enqueue_running_on_the_calling_cpu(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[0].current_thread = t; /* the caller's OWN current thread */
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->state == THREAD_STATE_READY, "same disposition on the local CPU");
    CHECK(list_head_empty(&t->sched_node), "still not linked");
    check_invariants("running locally");
}

static void test_enqueue_running_elsewhere_already_ready(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->block_reason = THREAD_BLOCK_EVENT;
    g_cpus[2].current_thread = t;
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->state == THREAD_STATE_READY, "stays READY");
    CHECK(t->block_reason == THREAD_BLOCK_NONE, "block reason still cleared");
    CHECK(list_head_empty(&t->sched_node), "not linked");
}

/* Regression: 2026-08-16-deferred-enqueue-claim (fixed 8c063c62f3).
 *
 * The deferral above is only half a hand-off, and the missing half wedged a
 * machine.  Marking the thread READY tells the holding CPU nothing it can act
 * on: by the time the mark lands, that CPU may already have run the check it
 * would have acted on, and then nobody enqueues the thread at all.  Observed in
 * CI and reproduced locally -- the ata driver left READY with on_rq 0, every FS
 * request queued behind it, and the machine idle with runnable work outstanding.
 *
 * So a refused enqueue leaves a CLAIM, exactly as a wake does: whoever exchanges
 * it to zero owns the enqueue.  Both orders must produce exactly one enqueue --
 * the holder releasing after the refusal, and the holder releasing first with
 * the refusal landing behind it. */

static void test_refused_enqueue_leaves_a_claim(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[2].current_thread = t;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    CHECK(list_head_empty(&t->sched_node), "not linked while it runs elsewhere");
    CHECK(t->enqueue_owed != 0, "an enqueue is recorded as owed");
    check_invariants("refusal leaves a claim");
}

/* The holder finishes with the thread and settles the debt: the thread must end
 * up queued exactly once, and the claim must be consumed so a later settle does
 * not link it twice. */
static void test_holder_settles_the_deferred_enqueue(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[2].current_thread = t;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    /* What the holder does when its dispatch returns: stop naming the thread,
     * then settle. Order matters and is the kernel's (process.c clears
     * current_thread before it handles the run result). */
    act_as(2);
    g_cpus[2].current_thread = NULL;
    sched_settle_deferred_enqueue(t);

    CHECK(t->state == THREAD_STATE_READY, "runnable");
    CHECK(t->on_rq == 1, "and queued");
    CHECK(t->enqueue_owed == 0, "the claim is consumed");
    check_invariants("holder settled");

    /* A second settle must not link it again. */
    log_reset();
    sched_settle_deferred_enqueue(t);
    CHECK(t->on_rq == 1, "still queued exactly once");
    check_invariants("second settle is a no-op");
}

/* The ordering the holder's settle cannot cover: the claim is published just
 * after that holder looked for one, so nobody is left to act on it. A CPU that
 * finds nothing to run sweeps the debt instead -- which is what makes "the
 * machine never idles with a runnable thread off every queue" true rather than
 * merely usual.
 *
 * The enqueuing side must NOT link the thread itself in this window: the holder
 * may still be deciding the thread's final state, and a thread dispatched out of
 * a queue mid-decision resumes a context nobody finished writing. Written that
 * way first, it panicked with rip inside g_threads. */
static void test_sweep_settles_a_claim_nobody_took(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[2].current_thread = t;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->on_rq == 0, "not linked from the enqueuing CPU");
    CHECK(t->enqueue_owed != 0, "left owed");

    /* Holder releases without settling -- the interleaving in question. */
    g_cpus[2].current_thread = NULL;

    act_as(1);
    sched_sweep_owed_enqueues();
    CHECK(t->on_rq == 1, "the sweep queued it");
    CHECK(t->enqueue_owed == 0, "and consumed the claim");
    check_invariants("swept");
}

/* The sweep must leave a thread that is executing exactly where it is: the
 * enqueue it performs goes through the same guard, which re-owes it. */
static void test_sweep_does_not_link_a_running_thread(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[2].current_thread = t;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    act_as(1);
    log_reset();
    sched_sweep_owed_enqueues(); /* holder still names it */
    sched_sweep_owed_enqueues(); /* and again, as the scheduler loop would */
    CHECK(t->on_rq == 0, "still not linked while it executes");
    CHECK(t->enqueue_owed != 0, "and the debt is carried forward");
    /* The sweep must not hand a running thread to cpu_sched_enqueue at all.
     * Doing so is refused and re-owed, which the sweep then retries on the next
     * scheduler pass -- a loop that produced 43 copies of the guard's report in
     * one CI window, from a thread that merely had a long timeslice. */
    CHECK(!saw("enqueue current"), "and the sweep does not churn against the guard");
    check_invariants("sweep respects the holder");
}

/* A claim for a thread that blocked again is consumed and dropped: the wake it
 * stood for was answered by whatever moved it out of READY. */
static void test_sweep_drops_a_claim_for_a_blocked_thread(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[2].current_thread = t;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    g_cpus[2].current_thread = NULL;
    t->state = THREAD_STATE_BLOCKED;

    act_as(1);
    sched_sweep_owed_enqueues();
    CHECK(t->on_rq == 0, "a blocked thread is not queued");
    CHECK(t->enqueue_owed == 0, "and its claim is consumed, not carried");
    check_invariants("blocked claim dropped");
}

/* Regression: 2026-08-16-tripwire-unlocked-writer (fixed 46bd8b5d26).
 *
 * Every scheduler tripwire must report through the LOCKING writer.
 *
 * The unlocked writers exist for fault handlers, where the lock may be held by
 * the CPU they interrupted.  A tripwire is not that: it fires on a live system,
 * concurrently with whatever other CPUs are printing, and without the lock its
 * line interleaves mid-string with theirs -- corrupting both.  On a real boot
 * that split `[test] preempt ok` into `[test] preempt [scheok` and failed a
 * battery on a machine that was working correctly.
 *
 * Asserted by capturing both writers separately and requiring the unlocked one
 * to stay untouched while a tripwire fires. */
static void test_tripwires_use_the_locking_writer(void) {
    harness_reset();
    g_log_unlocked_count = 0;

    /* Any tripwire will do; this one needs no cross-CPU setup. */
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    CHECK(saw("enqueue non-ready"), "the tripwire reported");
    CHECK(g_log_unlocked_count == 0, "and not through the unlocked writer");
}

/* An ordinary enqueue must leave no claim behind, or the holder's next settle
 * would link an already-queued thread. */
static void test_ordinary_enqueue_owes_nothing(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->on_rq == 1, "queued");
    CHECK(t->enqueue_owed == 0, "owes nothing");
    check_invariants("ordinary enqueue");
}

/* The reap gate: sched_mark_ready_if_live must never resurrect a dead or
 * still-initialising slot, however it is reached. */
static void test_enqueue_never_resurrects_dead_states(void) {
    const thread_state_t dead[] = {THREAD_STATE_ZOMBIE, THREAD_STATE_UNUSED, THREAD_STATE_NEW};
    for (unsigned i = 0; i < sizeof(dead) / sizeof(dead[0]); ++i) {
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, dead[i]);
        t->block_reason = THREAD_BLOCK_IPC;
        g_cpus[2].current_thread = t;
        cpu_sched_enqueue(&g_cpus[0].sched, t);
        CHECK(t->state == dead[i], "dead/new state is never promoted");
        CHECK(t->block_reason == THREAD_BLOCK_IPC, "and its block reason is untouched");
        CHECK(list_head_empty(&t->sched_node), "and it is not linked");
    }
}

/* The non-obvious half of the same rule: BLOCKED *is* a live state, so a
 * running-elsewhere BLOCKED thread is promoted. */
static void test_enqueue_promotes_blocked_running_elsewhere(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    t->block_reason = THREAD_BLOCK_EVENT;
    g_cpus[2].current_thread = t;
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->state == THREAD_STATE_READY, "BLOCKED is live and is promoted");
    CHECK(t->block_reason == THREAD_BLOCK_NONE, "block reason cleared");
    CHECK(list_head_empty(&t->sched_node), "still not linked");
}

static void test_enqueue_refuses_out_of_enum_state(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->state = (thread_state_t)99; /* corrupt / out of enum */
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(list_head_empty(&t->sched_node), "an unknown state is not linked");
    CHECK(t->on_rq == 0, "and not claimed");
    CHECK(sched_debug_count(SCHED_DEBUG_ENQUEUE_NON_READY) == 1, "and is counted");
    check_invariants("out-of-enum state");
}

/* Claim and linkage disagreeing is the shape that lets one node be linked twice.
 * Releasing the claim on the way out is the half that matters: an early return
 * still holding it strands the thread on no queue forever. */
static void test_claimed_but_linked_releases_the_claim(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    uint32_t before = g_cpus[0].sched.thread_count[SCHED_PRIO_WASM];
    log_reset();

    __atomic_store_n(&t->on_rq, 0, __ATOMIC_RELEASE); /* claim lost, node still linked */
    cpu_sched_enqueue(&g_cpus[0].sched, t);

    CHECK(sched_debug_count(SCHED_DEBUG_DOUBLE_LINK) == 1, "the disagreement is counted");
    CHECK(t->on_rq == 0, "the claim is RELEASED, not leaked");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == before, "not linked a second time");
}

/* ------------------------------------------- affinity: redirect targeting */

/* Affinity is a constraint, not a load hint: an allowed CPU is used even when a
 * disallowed one is emptier. */
static void test_allowed_but_loaded_cpu_is_not_redirected(void) {
    harness_reset();
    for (int i = 0; i < 10; ++i) {
        cpu_sched_enqueue(&g_cpus[0].sched, mk_thread(i, SCHED_PRIO_WASM, THREAD_STATE_READY));
    }
    thread_t* t = mk_thread(20, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = ~0u; /* CPU 0 allowed, though CPU 3 is empty */
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->rq == &g_cpus[0].sched, "an allowed CPU is kept despite the load");
    check_invariants("loaded but allowed");
}

static void test_redirect_prefers_last_cpu(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = (1u << 1) | (1u << 3);
    t->last_cpu = 3;
    /* Make CPU 3 the busier of the two allowed CPUs, so only last_cpu explains
     * the choice. */
    cpu_sched_enqueue(&g_cpus[3].sched, mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY));

    act_as(0); /* forbidden, so a redirect must happen */
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->rq == &g_cpus[3].sched, "redirect honours last_cpu among allowed CPUs");
    check_invariants("redirect to last_cpu");
}

/* A queue that is not one of g_cpus[] belongs to the caller -- the in-kernel
 * selftests build cpu_sched_t on the stack.  Affinity must not second-guess such
 * a target, because there is no CPU id to reason about. */
static void test_private_queue_is_not_redirected(void) {
    harness_reset();
    cpu_sched_t private_q;
    cpu_sched_init(&private_q);
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 2; /* would forbid CPU 0, whose index is the fallback */
    act_as(0);
    cpu_sched_enqueue(&private_q, t);
    CHECK(t->rq == &private_q, "a caller-owned queue receives the thread");
    CHECK(private_q.thread_count[SCHED_PRIO_WASM] == 1, "and accounts it");
}

/* ------------------------------------------------------ bounds and idle */

static void test_idle_thread_is_never_enqueued(void) {
    harness_reset();
    thread_t* idle0 = g_cpus[0].idle_thread;
    cpu_sched_enqueue(&g_cpus[0].sched, idle0);
    CHECK(list_head_empty(&idle0->sched_node), "an idle thread is not linked");
    CHECK(g_cpus[0].sched.ready_bitmap == 0u, "and no band is marked occupied");
    check_invariants("idle not enqueued");
}

/* sched_prio indexes ready_list[] and thread_count[], and shifts into the
 * bitmap.  A value >= SCHED_PRIO_MAX is an out-of-bounds write on two arrays and
 * sets the bit cpu_sched_highest_prio masks away. */
static void test_out_of_range_priority_is_refused(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->sched_prio = SCHED_PRIO_MAX; /* one past the last band */
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->on_rq == 0, "an out-of-range priority is not claimed");
    CHECK(list_head_empty(&t->sched_node), "and not linked");
    CHECK(g_cpus[0].sched.ready_bitmap == 0u, "and sets no bit");
    check_invariants("out-of-range prio");
}

/* ------------------------------------------- sched_enqueue_thread_from */

/* This wrapper exists to carry the ORIGINAL call site, which cpu_sched_enqueue
 * cannot report (its immediate caller is always this function).  The cases that
 * expect a report therefore assert on the caller address too: 0xCAFE appears
 * only in messages this path emitted, which is what distinguishes them from the
 * inner cpu_sched_enqueue reports of the same conditions. */

#define FROM_CALLER 0xCAFEu

static void test_enqueue_from_routes_to_the_calling_cpu(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(2);
    sched_enqueue_thread_from(t, FROM_CALLER);
    CHECK(t->rq == &g_cpus[2].sched, "lands on the calling CPU's queue");
    CHECK(t->on_rq == 1, "and is claimed");
    check_invariants("enqueue_from routing");
}

static void test_enqueue_from_non_ready_reports_the_real_caller(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    log_reset();
    sched_enqueue_thread_from(t, FROM_CALLER);

    /* The saw() assertion below is safe despite the power-of-two rate limit:
     * harness_reset() zeroed this counter, so this refusal is the first and
     * always prints.  Past the first few hits a message is not a contract --
     * see the note on the non-READY rejection test. */
    CHECK(sched_debug_count(SCHED_DEBUG_ENQUEUE_FROM_NON_READY) == 1,
          "the wrapper counts the refusal");
    CHECK(saw("cafe"), "and names the ORIGINAL call site, not its own return address");
    CHECK(list_head_empty(&t->sched_node), "the inner enqueue still refuses to link it");
    CHECK(t->on_rq == 0, "and does not claim it");
    check_invariants("enqueue_from non-ready");
}

static void test_enqueue_from_null_is_silent(void) {
    harness_reset();
    log_reset();
    sched_enqueue_thread_from(NULL, FROM_CALLER);
    CHECK(g_log_count == 0, "NULL is a silent no-op, not a reported anomaly");
    check_invariants("enqueue_from NULL");
}

static void test_enqueue_from_running_elsewhere(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    t->block_reason = THREAD_BLOCK_IPC;
    g_cpus[3].current_thread = t;
    log_reset();
    act_as(0);
    sched_enqueue_thread_from(t, FROM_CALLER);

    CHECK(saw("enqueue current"), "the running-elsewhere case is reported");
    CHECK(saw("cafe"), "with the original call site");
    CHECK(t->state == THREAD_STATE_READY, "the thread is promoted");
    CHECK(t->block_reason == THREAD_BLOCK_NONE, "block reason cleared");
    CHECK(list_head_empty(&t->sched_node), "but not linked while running elsewhere");
    check_invariants("enqueue_from running elsewhere");
}

/* -------------------------------------------------- removal / unlink paths */

/* Walk a band and report the tids in order, so FIFO can be asserted directly
 * rather than inferred from which thread pick_next hands back. */
static int band_order(uint32_t cpu, int prio, uint32_t* out, int max) {
    cpu_sched_t* cs = &g_cpus[cpu].sched;
    list_head_t* head = &cs->ready_list[prio];
    int n = 0;
    for (list_head_t* p = head->next; p != head && n < max; p = p->next) {
        out[n++] = list_entry(p, thread_t, sched_node)->tid;
    }
    return n;
}

/* R1: the give-up path.  A claim with no queue behind it (an enqueue that never
 * publishes rq) must terminate rather than spin forever, and must NOT clear the
 * claim -- clearing it is what strands a node linked-but-unclaimed. */
static void test_remove_gives_up_on_a_permanent_in_flight_claim(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    __atomic_store_n(&t->on_rq, 1, __ATOMIC_RELEASE);
    t->rq = 0; /* enqueue "in flight" forever */
    log_reset();

    cpu_sched_remove_thread(t); /* must return, not hang */
    CHECK(sched_debug_count(SCHED_DEBUG_REMOVE_GAVE_UP) == 1, "the give-up path is counted");
    CHECK(t->on_rq == 1, "the claim is NOT cleared out from under the enqueuer");
}

/* R2: a leaked claim over a detached node.  Nothing to unlink, so nothing may be
 * miscounted; the claim must still be released. */
static void test_remove_of_a_detached_but_claimed_thread(void) {
    harness_reset();
    thread_t* keep = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* t = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, keep);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    list_head_del(&t->sched_node); /* detach behind the queue's back */

    cpu_sched_remove_thread(t);
    CHECK(t->on_rq == 0, "the claim is released");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 1,
          "the still-queued sibling is not miscounted away");
    CHECK((g_cpus[0].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) != 0,
          "the band stays occupied while a thread remains");
}

/* R3: priority mutated while queued.  cpu_sched_unlink_locked accounts the
 * unlink against t->rq_prio -- the band recorded when the node was linked -- and
 * never against the thread's current sched_prio, so a priority changed after the
 * link still drains the band the node is actually in.  Accounting against
 * sched_prio instead leaks the old band's counter and hides the damage on the
 * new one behind the underflow floor, with no tripwire firing at all. */
static void test_remove_after_priority_mutation_drains_the_right_band(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    t->sched_prio = SCHED_PRIO_BACKGROUND; /* mutated while linked */

    cpu_sched_remove_thread(t);
    cpu_sched_t* cs = &g_cpus[0].sched;
    CHECK(cs->thread_count[SCHED_PRIO_DRIVER] == 0, "the band it was IN is drained");
    CHECK(cs->thread_count[SCHED_PRIO_BACKGROUND] == 0, "the band it was not in is untouched");
    CHECK((cs->ready_bitmap & (1u << SCHED_PRIO_DRIVER)) == 0, "old band's bit cleared");
    check_invariants("remove after prio mutation");
}

/* R4: the ghost-head tripwire itself.  A band spliced through a node with two
 * owners is the most expensive bug in this file's history, and the detector for
 * it had no test. */
static void test_ghost_head_is_reported(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    /* Link the same node a second time, bypassing the claim, so one unlink
     * cannot detach it from the head. */
    list_head_add_tail(&g_cpus[0].sched.ready_list[SCHED_PRIO_WASM], &t->sched_node);
    log_reset();

    cpu_sched_remove_thread(t);
    CHECK(sched_debug_count(SCHED_DEBUG_GHOST_HEAD) == 1,
          "a head still reaching the node is counted");
}

/* R5: the counter floor.  A drifted counter must clamp at zero rather than wrap
 * to UINT32_MAX, which is what historically kept a band's bit set forever. */
static void test_counter_floors_at_zero(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    cpu_sched_t* cs = &g_cpus[0].sched;
    cs->thread_count[SCHED_PRIO_WASM] = 0; /* forced drift */

    ksync_spinlock_lock(&cs->lock);
    cpu_sched_dequeue(cs, t);
    ksync_spinlock_unlock(&cs->lock);
    CHECK(cs->thread_count[SCHED_PRIO_WASM] == 0, "counter clamps, never wraps");
    CHECK((cs->ready_bitmap & (1u << SCHED_PRIO_WASM)) == 0, "and the band still goes idle");
}

/* R6: cpu_sched_dequeue against a queue that does not hold the thread is a
 * caller-contract violation.  Pinned so a future assert has a baseline. */
static void test_dequeue_against_the_wrong_queue(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, t);
    uint32_t owner_before = g_cpus[1].sched.thread_count[SCHED_PRIO_WASM];

    cpu_sched_t* wrong = &g_cpus[0].sched;
    ksync_spinlock_lock(&wrong->lock);
    cpu_sched_dequeue(wrong, t);
    ksync_spinlock_unlock(&wrong->lock);

    /* The node does leave CPU 1's list -- list_head_del does not consult the
     * head -- so the owning counter is what goes stale. */
    CHECK(g_cpus[1].sched.thread_count[SCHED_PRIO_WASM] == owner_before,
          "the owning queue's counter is NOT adjusted by the wrong caller");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 0,
          "and the wrong queue's counter floors rather than wrapping");
}

/* R7: the release order, asserted together rather than a field per test. */
static void test_remove_releases_queue_pointer_and_claim(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    cpu_sched_remove_thread(t);
    CHECK(t->rq == 0, "queue pointer cleared");
    CHECK(t->on_rq == 0, "claim released");
    CHECK(list_head_empty(&t->sched_node), "node canonically detached");
}

/* R8: removing from any position leaves the rest intact and in FIFO order. */
static void test_remove_from_head_middle_tail_preserves_fifo(void) {
    const int victim_idx[3] = {0, 1, 2};
    const uint32_t expect[3][2] = {{2u, 3u}, {1u, 3u}, {1u, 2u}};
    for (int c = 0; c < 3; ++c) {
        harness_reset();
        thread_t* th[3];
        for (int i = 0; i < 3; ++i) {
            th[i] = mk_thread(i, SCHED_PRIO_WASM, THREAD_STATE_READY);
            cpu_sched_enqueue(&g_cpus[0].sched, th[i]);
        }
        cpu_sched_remove_thread(th[victim_idx[c]]);

        uint32_t order[4];
        int n = band_order(0, SCHED_PRIO_WASM, order, 4);
        CHECK(n == 2, "two threads remain");
        CHECK(n == 2 && order[0] == expect[c][0] && order[1] == expect[c][1],
              "the remaining two keep FIFO order");
        check_invariants("remove from a 3-deep band");
    }
}

/* ------------------------------------------------ anti-starvation policy */

/* Observed donation cycle, expressed against the constant rather than hardcoded.
 *
 * It is STREAK+2, not STREAK+1, and the extra dispatch is deliberate to pin: the
 * dispatch that RETURNS to the high band after a donation takes the
 * last_dispatched_prio > prio arm, which RESETS the streak to 0 instead of
 * counting itself as the first of the new run.  So the high band receives
 * STREAK+1 consecutive slots per donated slot, while the constant reads as
 * though it should receive STREAK.  That arm cannot distinguish "returning from
 * a donation" from "a higher band just arrived", where resetting is correct, so
 * tightening the cycle changes fairness -- see SCHED_ANTISTARVATION_STREAK in
 * sched.h. */
#define DONATION_CYCLE (SCHED_ANTISTARVATION_STREAK + 2)

/* Dispatch `n` times on CPU 0 with both bands kept occupied (each picked thread
 * is re-enqueued), recording the band each dispatch came from. */
static void run_dispatches(int n, int hi, int lo, int* out_band) {
    for (int i = 0; i < n; ++i) {
        thread_t* t = pick_and_requeue(0);
        out_band[i] = (t == g_cpus[0].idle_thread) ? -1 : (int)t->sched_prio;
    }
    (void)hi;
    (void)lo;
}

/* Enqueue `count` fresh READY threads at `prio` onto CPU 0, taking pool slots
 * base_idx .. base_idx + count - 1. mk_thread recycles a slot by index, so two seed_band
 * calls in one case must use disjoint ranges or the second silently reuses the first's
 * threads. */
static void seed_band(int prio, int count, int base_idx) {
    for (int i = 0; i < count; ++i) {
        cpu_sched_enqueue(&g_cpus[0].sched,
                          mk_thread(base_idx + i, (sched_prio_t)prio, THREAD_STATE_READY));
    }
}

/* A1: the streak boundary, derived from the constant.  Every dispatch before the
 * boundary comes from the high band; the first low-band slot lands exactly on it. */
static void test_streak_boundary_is_exact(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    seed_band(SCHED_PRIO_BACKGROUND, 2, 10);
    int band[16];
    run_dispatches(DONATION_CYCLE, SCHED_PRIO_DRIVER, SCHED_PRIO_BACKGROUND, band);

    int first_low = -1;
    for (int i = 0; i < DONATION_CYCLE; ++i) {
        if (band[i] == SCHED_PRIO_BACKGROUND && first_low < 0) {
            first_low = i;
        }
    }
    CHECK(first_low == DONATION_CYCLE - 1, "the first donated slot lands on the boundary");
    for (int i = 0; i < DONATION_CYCLE - 1; ++i) {
        CHECK(band[i] == SCHED_PRIO_DRIVER, "every dispatch before the boundary is high band");
    }
}

/* A2: the donation goes to the NEXT occupied lower band, not the lowest. */
static void test_demotion_picks_the_next_lower_band(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    seed_band(SCHED_PRIO_WASM, 2, 10);
    seed_band(SCHED_PRIO_BACKGROUND, 2, 20);
    int band[16];
    run_dispatches(DONATION_CYCLE, 0, 0, band);
    CHECK(band[DONATION_CYCLE - 1] == SCHED_PRIO_WASM,
          "the yielded slot goes to the next occupied band, not the lowest");
}

/* A3: one slot, then straight back to the top, and the cycle repeats. */
static void test_donation_is_one_slot_then_back_to_the_top(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    seed_band(SCHED_PRIO_BACKGROUND, 2, 10);
    int band[3 * (SCHED_ANTISTARVATION_STREAK + 2)];
    int n = 3 * DONATION_CYCLE;
    run_dispatches(n, 0, 0, band);

    int lows = 0;
    int last_low = -1;
    int spacing_ok = 1;
    for (int i = 0; i < n; ++i) {
        if (band[i] != SCHED_PRIO_BACKGROUND) {
            continue;
        }
        lows++;
        if (last_low >= 0 && (i - last_low) != DONATION_CYCLE) {
            spacing_ok = 0;
        }
        last_low = i;
        if (i + 1 < n) {
            CHECK(band[i + 1] == SCHED_PRIO_DRIVER, "the very next dispatch returns to the top");
        }
    }
    CHECK(lows == 3, "exactly one donation per cycle");
    CHECK(spacing_ok, "donations are evenly spaced by the cycle length");
}

/* A4: with no lower band to donate to, the streak keeps climbing and nothing is
 * demoted -- the counter must not reset just because the search failed. */
static void test_no_lower_band_means_no_demotion_and_no_reset(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    int band[SCHED_ANTISTARVATION_STREAK + 5];
    int n = SCHED_ANTISTARVATION_STREAK + 5;
    run_dispatches(n, 0, 0, band);
    int all_high = 1;
    for (int i = 0; i < n; ++i) {
        if (band[i] != SCHED_PRIO_DRIVER) {
            all_high = 0;
        }
    }
    CHECK(all_high, "every dispatch stays in the only occupied band");
    CHECK(g_cpus[0].sched.high_prio_streak > SCHED_ANTISTARVATION_STREAK,
          "and the streak keeps climbing rather than resetting");
}

/* A5: a higher band arriving resets the streak, so the newcomer is not
 * immediately demoted past. */
static void test_higher_band_arrival_resets_the_streak(void) {
    harness_reset();
    seed_band(SCHED_PRIO_WASM, 2, 0);
    int band[16];
    run_dispatches(SCHED_ANTISTARVATION_STREAK, 0, 0, band);

    cpu_sched_enqueue(&g_cpus[0].sched, mk_thread(10, SCHED_PRIO_DRIVER, THREAD_STATE_READY));
    thread_t* got = pick_on(0);
    CHECK(got->sched_prio == SCHED_PRIO_DRIVER, "the higher band is dispatched");
    CHECK(g_cpus[0].sched.high_prio_streak == 0, "and the streak resets");
}

/* A6: going idle clears both pieces of state. */
static void test_going_idle_resets_the_policy_state(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    (void)pick_on(0);
    (void)pick_on(0);
    CHECK(pick_on(0) == g_cpus[0].idle_thread, "queue drained");
    CHECK(g_cpus[0].sched.high_prio_streak == 0, "streak cleared on idle");
    CHECK(g_cpus[0].sched.last_dispatched_prio == SCHED_PRIO_IDLE, "last band reset to idle");
}

/* A7: high_prio_streak is a uint8_t and wraps.  A wrap must only DELAY fairness,
 * never disable it -- after the counter rolls through zero, a newly occupied
 * lower band must still receive a slot within one cycle. */
static void test_streak_wrap_only_delays_fairness(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    int band[300];
    run_dispatches(300, 0, 0, band); /* drives the uint8_t past 255 */

    seed_band(SCHED_PRIO_BACKGROUND, 2, 10);
    int follow[32];
    int n = DONATION_CYCLE + 2;
    run_dispatches(n, 0, 0, follow);
    int saw_low = 0;
    for (int i = 0; i < n; ++i) {
        if (follow[i] == SCHED_PRIO_BACKGROUND) {
            saw_low = 1;
        }
    }
    CHECK(saw_low, "fairness resumes within a cycle after the counter wraps");
}

/* A8: the policy as a property rather than its mechanics -- under sustained load
 * on both bands, the lower one receives roughly its share. */
static void test_sustained_fairness_ratio(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    seed_band(SCHED_PRIO_WASM, 2, 10);
    int band[200];
    run_dispatches(200, 0, 0, band);
    int lows = 0;
    for (int i = 0; i < 200; ++i) {
        if (band[i] == SCHED_PRIO_WASM) {
            lows++;
        }
    }
    int expect = 200 / DONATION_CYCLE;
    CHECK(lows >= expect - 1 && lows <= expect + 1, "the lower band gets its share of slots");
}

/* A9: a donation must not land in a band that only holds stale nodes and leave
 * the CPU idle -- the sweep drops them and the picker converges back up. */
static void test_demotion_into_a_stale_band_converges(void) {
    harness_reset();
    seed_band(SCHED_PRIO_DRIVER, 2, 0);
    thread_t* dead1 = mk_thread(10, SCHED_PRIO_BACKGROUND, THREAD_STATE_READY);
    thread_t* dead2 = mk_thread(11, SCHED_PRIO_BACKGROUND, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, dead1);
    cpu_sched_enqueue(&g_cpus[0].sched, dead2);
    dead1->state = THREAD_STATE_ZOMBIE;
    dead2->state = THREAD_STATE_ZOMBIE;

    int band[16];
    run_dispatches(DONATION_CYCLE, 0, 0, band);
    for (int i = 0; i < DONATION_CYCLE; ++i) {
        CHECK(band[i] != -1, "no dispatch falls through to idle");
    }
    check_invariants("demotion into a stale band");
}

/* ------------------------------------- pick_next: sweep, idle, staleness */

/* P1: the sweep is LAZY, not a band scrub.  It stops at the first live node, so
 * stale nodes behind that one stay queued.  Pinning this matters because the
 * opposite reading -- "pick_next cleans the band" -- would make several of the
 * wedge fixes look redundant. */
static void test_sweep_stops_at_the_first_live_node(void) {
    harness_reset();
    thread_t* lead_stale = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* live = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* trail_stale = mk_thread(2, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, lead_stale);
    cpu_sched_enqueue(&g_cpus[0].sched, live);
    cpu_sched_enqueue(&g_cpus[0].sched, trail_stale);
    lead_stale->state = THREAD_STATE_ZOMBIE;
    trail_stale->state = THREAD_STATE_ZOMBIE;

    CHECK(pick_on(0) == live, "the first live node is returned");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 1,
          "the trailing stale node is still queued -- the sweep is lazy");
    CHECK(list_head_empty(&lead_stale->sched_node), "the leading stale node was dropped");
}

/* P2: tid == 0 is treated as stale even when the state says READY -- a reset slot
 * whose state has not caught up yet. */
static void test_tid_zero_is_stale_even_when_ready(void) {
    harness_reset();
    thread_t* zero = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* live = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, zero);
    cpu_sched_enqueue(&g_cpus[0].sched, live);
    zero->tid = 0;

    CHECK(pick_on(0) == live, "a tid==0 node is dropped, not dispatched");
    CHECK(list_head_empty(&zero->sched_node), "and is unlinked");
}

/* P3: the sweep drops only UNUSED and ZOMBIE.  BLOCKED, RUNNING and NEW nodes
 * are handed BACK, which is precisely what produces SCHED_R_NOTREADY upstream --
 * so this pins why that return code has to exist at all. */
static void test_sweep_returns_blocked_running_and_new(void) {
    const thread_state_t passed[] = {THREAD_STATE_BLOCKED, THREAD_STATE_RUNNING, THREAD_STATE_NEW};
    for (unsigned i = 0; i < sizeof(passed) / sizeof(passed[0]); ++i) {
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
        cpu_sched_enqueue(&g_cpus[0].sched, t);
        t->state = passed[i]; /* transitioned while queued */
        CHECK(pick_on(0) == t, "a non-READY but non-dead node is returned to the dispatcher");
    }
}

/* P4: with no idle thread installed, an empty queue yields NULL.  That is the
 * SCHED_R_PICK case the scheduler loop panics on -- "not even idle was
 * dispatchable" -- and it is a real invariant violation, not a fallback. */
static void test_missing_idle_thread_yields_null(void) {
    harness_reset();
    g_cpus[0].idle_thread = NULL;
    g_cpus[0].sched.idle = NULL;
    CHECK(pick_on(0) == NULL, "no idle installed means no thread at all");
}

/* P5: pick_next answers the idle fallback from cpu_local()->idle_thread, and
 * process_schedule_once_impl's work-steal trigger compares against that SAME
 * field.  Testing cs->idle instead leaves the trigger permanently false in the
 * window where the two disagree -- an AP after process_ap_init but before
 * g_cpus[id].sched.idle is set -- so the CPU idles while runnable work piles up
 * elsewhere.  This pins that pick_next reads cpu_local(), which is the half the
 * trigger depends on; cs->idle stays load-bearing for cpu_sched_load_on and the
 * steal scan (T4).
 *
 * FIXME: the second assertion's message still describes the superseded trigger
 * (`thread == cs->idle`); the assertion itself only observes that the two fields
 * can disagree, and a NULL cs->idle no longer disables work stealing. */
static void test_cs_idle_and_cpu_local_idle_must_agree(void) {
    harness_reset();
    g_cpus[0].sched.idle = NULL; /* the AP-bringup window */
    thread_t* got = pick_on(0);
    CHECK(got == g_cpus[0].idle_thread, "pick_next answers from cpu_local(), not cs->idle");
    CHECK(got != g_cpus[0].sched.idle,
          "so a NULL cs->idle silently disables the work-steal trigger");
}

/* P6: the idle fallback is the CALLER's, not the queue's.  Picking on a remote
 * queue therefore returns the local idle thread -- non-obvious, and the
 * in-kernel selftest depends on it. */
static void test_pick_on_a_remote_queue_returns_local_idle(void) {
    harness_reset();
    act_as(0);
    cpu_sched_t* remote = &g_cpus[3].sched;
    ksync_spinlock_lock(&remote->lock);
    thread_t* got = cpu_sched_pick_next(remote);
    ksync_spinlock_unlock(&remote->lock);
    CHECK(got == g_cpus[0].idle_thread, "the caller's idle is returned, not the queue owner's");
}

/* P7: every band stale at once.  The convergence loop must drain them all, clear
 * every bit and counter, and terminate. */
static void test_all_bands_stale_converges_to_idle(void) {
    harness_reset();
    for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
        thread_t* t = mk_thread(p, (sched_prio_t)p, THREAD_STATE_READY);
        cpu_sched_enqueue(&g_cpus[0].sched, t);
        t->state = THREAD_STATE_ZOMBIE;
    }
    CHECK(pick_on(0) == g_cpus[0].idle_thread, "converges to idle rather than wedging");
    CHECK(g_cpus[0].sched.ready_bitmap == 0u, "every band bit cleared");
    int counters_clear = 1;
    for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
        if (g_cpus[0].sched.thread_count[p] != 0u) {
            counters_clear = 0;
        }
    }
    CHECK(counters_clear, "every counter drained");
    check_invariants("all bands stale");
}

/* -------------------------------------- sched_wake_thread + Dekker handshake */

/* The wake/block handshake (sched_wake_claim_enqueue / sched_block_complete_claim
 * in thread.h).  wake_pending is a claim TOKEN: whoever exchanges it to 0 owns
 * the enqueue, so "exactly one side owns it" is the property to pin -- losing it
 * strands a thread READY on no queue, which is how every CPU ends up idling with
 * runnable work outstanding. */

static void test_wake_ordinary(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    t->block_reason = THREAD_BLOCK_EVENT;
    act_as(2);
    sched_wake_thread(t);
    CHECK(t->state == THREAD_STATE_READY, "woken to READY");
    CHECK(t->block_reason == THREAD_BLOCK_NONE, "block reason cleared");
    CHECK(t->rq == &g_cpus[2].sched, "enqueued on the waking CPU");
    check_invariants("ordinary wake");
}

/* The deferral half: with a block in flight the completion path owns the
 * enqueue, so the waker must promote the thread and leave the token behind
 * rather than link it itself. */
static void test_wake_during_blocking_transition_defers_with_token(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_SEQ_CST);

    sched_wake_thread(t);
    CHECK(list_head_empty(&t->sched_node), "not enqueued by the waker");
    CHECK(t->on_rq == 0, "and not claimed");
    CHECK(t->wake_pending == 1, "the token is LEFT for the completion path");
    CHECK(t->state == THREAD_STATE_READY, "but the thread is promoted");
}

static void test_wake_of_a_resumed_thread_is_ignored(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    sched_wake_thread(t);
    CHECK(list_head_empty(&t->sched_node), "a stale wake does not enqueue a running thread");
    CHECK(t->on_rq == 0, "and does not claim it");
}

static void test_wake_of_dead_states_is_ignored(void) {
    const thread_state_t dead[] = {THREAD_STATE_ZOMBIE, THREAD_STATE_UNUSED};
    for (unsigned i = 0; i < sizeof(dead) / sizeof(dead[0]); ++i) {
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, dead[i]);
        sched_wake_thread(t);
        CHECK(t->state == dead[i], "a dead thread is not resurrected by a wake");
        CHECK(list_head_empty(&t->sched_node), "and is not enqueued");
    }
}

static void test_double_wake_links_once(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    sched_wake_thread(t);
    sched_wake_thread(t); /* the second loses thread_wake_if_blocked */
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 1, "linked exactly once");
    check_invariants("double wake");
}

static void test_wake_honours_affinity_end_to_end(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    t->cpu_affinity = 1u << 3;
    act_as(0);
    sched_wake_thread(t);
    CHECK(t->rq == &g_cpus[3].sched, "a wake from a forbidden CPU still lands on an allowed one");
    check_invariants("wake honours affinity");
}

/* Priority preemption: requested when this CPU is running nothing, or when the
 * woken thread outranks what it is running.  Lower sched_prio == higher band. */
static void test_resched_requested_only_on_a_priority_win(void) {
    harness_reset();
    thread_t* running = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);

    g_cpus[0].current_thread = NULL;
    g_resched_requests = 0;
    sched_wake_thread(mk_thread(1, SCHED_PRIO_BACKGROUND, THREAD_STATE_BLOCKED));
    CHECK(g_resched_requests == 1, "nothing running: a wake always requests resched");

    g_cpus[0].current_thread = running;
    g_resched_requests = 0;
    sched_wake_thread(mk_thread(2, SCHED_PRIO_DRIVER, THREAD_STATE_BLOCKED));
    CHECK(g_resched_requests == 1, "a higher band preempts");

    g_resched_requests = 0;
    sched_wake_thread(mk_thread(3, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED));
    CHECK(g_resched_requests == 0, "an EQUAL band does not preempt");

    g_resched_requests = 0;
    sched_wake_thread(mk_thread(4, SCHED_PRIO_BACKGROUND, THREAD_STATE_BLOCKED));
    CHECK(g_resched_requests == 0, "a lower band does not preempt");
}

/* The handshake itself, driven directly through the two inlines.  Exactly one
 * side must come away owning the enqueue in every ordering. */
static void test_dekker_exactly_one_side_owns_the_enqueue(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);

    /* (a) wake entirely before the block begins: the waker owns it. */
    __atomic_store_n(&t->blocking_transition, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->wake_pending, 0, __ATOMIC_SEQ_CST);
    CHECK(sched_wake_claim_enqueue(t) == 1, "waker claims when no block is in flight");
    CHECK(t->wake_pending == 0, "and consumes the token");

    /* (b) wake after blocking_transition is published: the completion owns it. */
    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->wake_pending, 0, __ATOMIC_SEQ_CST);
    CHECK(sched_wake_claim_enqueue(t) == 0, "waker defers to the in-flight block");
    CHECK(t->wake_pending == 1, "leaving the token behind");
    CHECK(sched_block_complete_claim(t) == 1, "the completion path claims it");
    CHECK(t->wake_pending == 0, "and consumes it");
    CHECK(t->blocking_transition == 0, "and clears the transition flag");

    /* (c) several wakes, one completion: still claimed exactly once. */
    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->wake_pending, 0, __ATOMIC_SEQ_CST);
    CHECK(sched_wake_claim_enqueue(t) == 0, "wake 1 defers");
    CHECK(sched_wake_claim_enqueue(t) == 0, "wake 2 defers");
    CHECK(sched_wake_claim_enqueue(t) == 0, "wake 3 defers");
    CHECK(sched_block_complete_claim(t) == 1, "the completion claims once");
    CHECK(sched_block_complete_claim(t) == 0, "and a second completion claims nothing");
}

/* A token left over from an earlier cycle must be consumed once and not
 * resurface as a spurious wake on the next block. */
static void test_stale_token_cannot_force_a_spurious_wake(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    __atomic_store_n(&t->wake_pending, 1, __ATOMIC_SEQ_CST); /* stale, no block in progress */

    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_SEQ_CST);
    CHECK(sched_block_complete_claim(t) == 1, "the stale token is consumed once");

    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_SEQ_CST);
    CHECK(sched_block_complete_claim(t) == 0, "and the next cycle sees no token");
}

/* The caller-CPU-bias switch decides whether a wake retargets last_cpu.  The
 * harness is compiled with the same arm as the kernel, so this observes what
 * actually ships rather than the other branch. */
static void test_caller_cpu_bias_arm(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    t->last_cpu = 1;
    act_as(2);
    sched_wake_thread(t);
#if WASMOS_SCHED_CALLER_CPU_BIAS
    CHECK(t->last_cpu == 2, "bias ON: the waker's CPU becomes last_cpu");
#else
    CHECK(t->last_cpu == 1, "bias OFF: last_cpu is left alone");
#endif
}

/* ------------------------------------------------------- sched_thread_init */

/* I1: the field contract.  sched_thread_init is the sole place a thread's
 * scheduler state is established from scratch. */
static void test_init_establishes_every_field(void) {
    harness_reset();
    thread_t* t = &g_pool[0];
    memset(t, 0, sizeof(*t));
    t->tid = 1;
    list_head_init(&t->sched_node);
    sched_thread_init(t, SCHED_PRIO_SERVICE);

    CHECK(t->ctx_canary_pre == PROCESS_CTX_CANARY_VALUE, "leading canary set");
    CHECK(t->ctx_canary_post == PROCESS_CTX_CANARY_VALUE, "trailing canary set");
    CHECK(t->sched_prio == SCHED_PRIO_SERVICE, "priority adopted");
    CHECK(t->cpu_affinity == ~0u, "affinity unrestricted by default");
    CHECK(t->last_cpu == 0, "last_cpu reset");
    CHECK(t->on_rq == 0, "no run-queue claim");
    CHECK(t->rq == 0, "no owning queue");
    CHECK(list_head_empty(&t->sched_node), "sched_node canonically detached");
    CHECK(list_head_empty(&t->event_node), "event_node canonically detached");
    CHECK(t->wait_event == 0, "not waiting on an event");
    CHECK(t->pend_state == SCHED_PEND_NONE, "no pending wake state");
    CHECK(t->pend_data == 0, "no pending wake payload");
    CHECK(t->join_event.type == SCHED_EVENT_TYPE_JOIN, "join event typed");
    CHECK(list_head_empty(&t->join_event.wait_list), "join event has no waiters");
}

/* I2: a leaked claim over a detached node.  The guard fires,
 * cpu_sched_remove_thread gives up (there is no rq to follow), and init then
 * clears the claim unconditionally -- so the thread must come out ENQUEUEABLE
 * rather than stranded holding a claim no queue backs. */
static void test_init_recovers_a_leaked_claim(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    __atomic_store_n(&t->on_rq, 1, __ATOMIC_RELEASE);
    t->rq = 0; /* claim with nothing behind it */

    sched_thread_init(t, SCHED_PRIO_WASM);
    CHECK(t->on_rq == 0, "the leaked claim is cleared");
    t->state = THREAD_STATE_READY;
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->on_rq == 1 && t->rq == &g_cpus[0].sched, "and the thread is enqueueable again");
    check_invariants("init after leaked claim");
}

/* I3: init RESETS cpu_affinity to unrestricted.  Re-initialising a thread that
 * was pinned therefore discards the pinning silently.  Pinned as current
 * behaviour: init means "establish from scratch", and every caller today is a
 * fresh or recycled slot where ~0u is correct -- but a caller that pins first
 * and initialises second loses the mask with no diagnostic. */
static void test_init_discards_an_existing_pinning(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 2;
    sched_thread_init(t, SCHED_PRIO_WASM);
    CHECK(t->cpu_affinity == ~0u, "a pinning does not survive re-initialisation");
}

/* I4: re-init across every (old, new) band pair, including the diagonal --
 * same-band re-init is a distinct path from the cross-band case. */
static void test_reinit_across_every_band_pair(void) {
    int bad = 0;
    for (int old_p = 0; old_p < SCHED_PRIO_MAX; ++old_p) {
        for (int new_p = 0; new_p < SCHED_PRIO_MAX; ++new_p) {
            harness_reset();
            thread_t* t = mk_thread(0, (sched_prio_t)old_p, THREAD_STATE_READY);
            cpu_sched_enqueue(&g_cpus[0].sched, t);
            sched_thread_init(t, (sched_prio_t)new_p);

            cpu_sched_t* cs = &g_cpus[0].sched;
            if (cs->thread_count[old_p] != 0u || (cs->ready_bitmap & (1u << old_p)) != 0) {
                bad++;
            }
            if (old_p != new_p &&
                (cs->thread_count[new_p] != 0u || (cs->ready_bitmap & (1u << new_p)) != 0)) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "every band pair drains the old band and leaves the new one untouched");
}

/* I5: a thread queued on a REMOTE CPU.  The unlink must follow t->rq rather than
 * assume the initialising CPU's queue. */
static void test_reinit_of_a_remotely_queued_thread(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    act_as(3);
    cpu_sched_enqueue(&g_cpus[3].sched, t);

    act_as(0); /* initialise from a different CPU */
    sched_thread_init(t, SCHED_PRIO_WASM);
    CHECK(g_cpus[3].sched.thread_count[SCHED_PRIO_DRIVER] == 0, "the owning queue is drained");
    CHECK((g_cpus[3].sched.ready_bitmap & (1u << SCHED_PRIO_DRIVER)) == 0,
          "and its band bit cleared");
    CHECK(g_cpus[0].sched.ready_bitmap == 0u, "the initialising CPU's queue is untouched");
    CHECK(t->on_rq == 0 && t->rq == 0, "and the thread is left unclaimed");
    check_invariants("remote re-init");
}

/* ------------------------------------------------------------- placement */

/* Load N threads onto `cpu` so placement decisions can be set up by weight. */
static void load_cpu(uint32_t cpu, int n, int base_idx) {
    for (int i = 0; i < n; ++i) {
        thread_t* t = mk_thread(base_idx + i, SCHED_PRIO_WASM, THREAD_STATE_READY);
        t->cpu_affinity = ~0u;
        act_as(cpu);
        cpu_sched_enqueue(&g_cpus[cpu].sched, t);
    }
    act_as(0);
}

static void test_load_sums_every_band(void) {
    harness_reset();
    for (int i = 0; i < 2; ++i) {
        act_as(1);
        cpu_sched_enqueue(&g_cpus[1].sched, mk_thread(i, SCHED_PRIO_DRIVER, THREAD_STATE_READY));
    }
    for (int i = 0; i < 3; ++i) {
        act_as(1);
        cpu_sched_enqueue(&g_cpus[1].sched, mk_thread(10 + i, SCHED_PRIO_WASM, THREAD_STATE_READY));
    }
    act_as(0);
    CHECK(cpu_sched_pick_target_cpu() != 1, "a CPU loaded across two bands is not the lightest");
}

static void test_running_non_idle_thread_counts_as_load(void) {
    harness_reset();
    thread_t* busy = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_RUNNING);
    g_cpus[1].current_thread = busy;
    load_cpu(0, 1, 10);
    load_cpu(3, 1, 20);
    CHECK(cpu_sched_pick_target_cpu() == 2,
          "the CPU running real work is skipped for the idle one");
}

static void test_running_idle_thread_does_not_count(void) {
    harness_reset();
    g_cpus[1].current_thread = g_cpus[1].idle_thread;
    load_cpu(0, 1, 10);
    load_cpu(2, 1, 20);
    load_cpu(3, 1, 30);
    CHECK(cpu_sched_pick_target_cpu() == 1, "a CPU running only idle reads as unloaded");
}

/* The AP-bringup window: sched.idle unset while a thread runs.  The idle test is
 * "current_thread != cs->idle", so a NULL cs->idle makes even the idle thread
 * count, and placement over-counts that CPU. */
static void test_null_cs_idle_makes_idle_count_as_load(void) {
    harness_reset();
    g_cpus[1].current_thread = g_cpus[1].idle_thread;
    g_cpus[1].sched.idle = NULL; /* bringup window */
    load_cpu(0, 1, 10);
    load_cpu(2, 1, 20);
    load_cpu(3, 1, 30);
    CHECK(cpu_sched_pick_target_cpu() != 1,
          "with cs->idle unset the idle thread counts and the CPU looks loaded");
}

static void test_pick_target_selects_the_minimum(void) {
    harness_reset();
    load_cpu(0, 3, 0);
    load_cpu(1, 1, 10);
    load_cpu(2, 4, 15);
    load_cpu(3, 2, 22);
    CHECK(cpu_sched_pick_target_cpu() == 1, "the lightest CPU wins");
}

/* Ties rotate through the placement cursor.  harness_reset() re-seeds it via
 * sched_debug_reset, so this asserts the exact SEQUENCE rather than settling for
 * a distribution.  A cursor held in a function static would carry over from
 * whatever ran before, leaving only the shape of the spread checkable. */
static void test_ties_rotate_evenly(void) {
    harness_reset();
    int wrong = 0;
    for (uint32_t i = 0; i < 8u; ++i) {
        if (cpu_sched_pick_target_cpu() != (i % 4u)) {
            wrong++;
        }
    }
    CHECK(wrong == 0, "tied placements walk the CPUs in order from a known cursor");
}

/* pick_target_cpu is deliberately NOT affinity-aware -- that is what
 * _for_thread is for.  Pinned so the two contracts stay distinguishable. */
static void test_pick_target_ignores_affinity_by_design(void) {
    harness_reset();
    thread_t* pinned = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    pinned->cpu_affinity = 1u << 3;
    load_cpu(3, 4, 10);
    CHECK(cpu_sched_pick_target_cpu() != 3, "the non-thread-aware picker answers by load alone");
    CHECK(cpu_sched_pick_target_cpu_for_thread(pinned, 0) == 3,
          "while the thread-aware one honours the mask");
}

static void test_for_thread_null_is_safe(void) {
    harness_reset();
    load_cpu(0, 2, 0);
    load_cpu(1, 2, 5);
    load_cpu(3, 2, 10);
    CHECK(cpu_sched_pick_target_cpu_for_thread(NULL, 1) == 2,
          "NULL yields the lightest online CPU");
}

static void test_prefer_last_cpu_forbidden_by_affinity(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = (1u << 2) | (1u << 3);
    t->last_cpu = 0; /* not in the mask */
    load_cpu(3, 3, 10);
    CHECK(cpu_sched_pick_target_cpu_for_thread(t, 1) == 2,
          "a forbidden last_cpu falls through to the lightest allowed CPU");
}

static void test_prefer_last_cpu_out_of_range(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->last_cpu = 7; /* >= g_cpu_count */
    load_cpu(0, 2, 10);
    load_cpu(1, 2, 15);
    load_cpu(3, 2, 20);
    CHECK(cpu_sched_pick_target_cpu_for_thread(t, 1) == 2, "an out-of-range last_cpu is ignored");
}

static void test_prefer_last_cpu_zero_selects_by_load(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->last_cpu = 0;
    load_cpu(0, 4, 10);
    load_cpu(1, 2, 16);
    load_cpu(3, 2, 20);
    CHECK(cpu_sched_pick_target_cpu_for_thread(t, 0) == 2,
          "prefer_last_cpu=0 ignores a perfectly valid last_cpu");
}

static void test_affinity_and_load_interact(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = (1u << 1) | (1u << 3);
    load_cpu(1, 5, 10);
    load_cpu(3, 2, 20);
    CHECK(cpu_sched_pick_target_cpu_for_thread(t, 0) == 3,
          "the lightest ALLOWED CPU wins, not the globally lightest");
}

static void test_spawn_sets_last_cpu_and_enqueues_there(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->last_cpu = 0;
    load_cpu(0, 3, 10);
    load_cpu(1, 3, 15);
    load_cpu(3, 3, 20);
    sched_spawn_thread(t);
    CHECK(t->last_cpu == 2, "last_cpu is rewritten to the chosen target");
    CHECK(t->rq == &g_cpus[2].sched, "and the thread is enqueued there");
    check_invariants("spawn placement");
}

/* Partial effect: the target is chosen and last_cpu rewritten BEFORE the enqueue
 * can refuse, so a non-READY thread ends up on no queue with a mutated
 * last_cpu.  Pinned rather than fixed -- callers spawn READY threads. */
static void test_spawn_of_a_non_ready_thread_still_rewrites_last_cpu(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    t->last_cpu = 3;
    sched_spawn_thread(t);
    CHECK(list_head_empty(&t->sched_node), "the enqueue is refused");
    CHECK(t->on_rq == 0, "and no claim is taken");
    CHECK(t->last_cpu != 3, "but last_cpu was already rewritten -- a partial effect");
}

static void test_spawn_with_affinity_needs_no_redirect(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 3;
    sched_spawn_thread(t);
    CHECK(t->rq == &g_cpus[3].sched, "placement already chose an allowed CPU");
    CHECK(t->last_cpu == 3, "and last_cpu agrees with the queue it landed on");
    check_invariants("spawn with affinity");
}

/* T7/T8: degenerate CPU counts.  g_cpu_count comes from the MADT scan and is not
 * validated at its source, so both placement entry points take their loop bound
 * and modulus from cpu_sched_usable_cpus(), which clamps it to WASMOS_MAX_CPUS
 * and floors it at 1 (the BSP always exists).  Without the floor a count of 0
 * divides by zero; without the clamp a larger count walks off g_cpus[]. */
static void test_zero_cpu_count_is_survivable(void) {
    harness_reset();
    g_cpu_count = 0;
    CHECK(cpu_sched_pick_target_cpu() < WASMOS_MAX_CPUS, "no division by zero on an empty CPU set");
    CHECK(cpu_sched_pick_target_cpu_for_thread(NULL, 0) < WASMOS_MAX_CPUS,
          "and the same for _for_thread");
    g_cpu_count = 4;
}

static void test_cpu_count_beyond_the_table_is_clamped(void) {
    harness_reset();
    g_cpu_count = WASMOS_MAX_CPUS + 8u; /* more CPUs claimed than g_cpus[] holds */
    CHECK(cpu_sched_pick_target_cpu() < WASMOS_MAX_CPUS, "placement stays inside the table");
    CHECK(cpu_sched_pick_target_cpu_for_thread(NULL, 0) < WASMOS_MAX_CPUS,
          "and so does the thread-aware picker");
    g_cpu_count = 4;
}

/* -------------------------------------------------------------- affinity */

/* cpu_sched_affinity_allows is static, so these drive it through the public
 * entry points that consult it: the enqueue redirect, steal, and placement. */

/* C1: a mask naming one ONLINE and one offline CPU must restrict to the online
 * subset.  Distinct from the unsatisfiable-mask case, which leaves through the
 * "no constraint" fallback without ever running the restrictive path. */
static void test_partially_online_mask_restricts(void) {
    harness_reset();
    for (uint32_t forbidden = 0; forbidden < 4; ++forbidden) {
        if (forbidden == 1u) {
            continue;
        }
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
        t->cpu_affinity = (1u << 1) | (1u << 9); /* CPU 1 online, CPU 9 not */
        act_as(forbidden);
        cpu_sched_enqueue(&g_cpus[forbidden].sched, t);
        CHECK(t->rq == &g_cpus[1].sched, "redirected to the only online allowed CPU");
    }
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = (1u << 1) | (1u << 9);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, t);
    CHECK(t->rq == &g_cpus[1].sched, "and left alone on the allowed CPU");
}

/* C2: an empty mask means unconstrained, not "runnable nowhere". */
static void test_empty_mask_is_unconstrained(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 0u;
    act_as(2);
    cpu_sched_enqueue(&g_cpus[2].sched, t);
    CHECK(t->on_rq == 1, "an empty mask does not strand the thread");
    CHECK(t->rq == &g_cpus[2].sched, "and does not force a redirect");
}

/* C3: a CPU id outside the table reads as allowed, and cpu_sched_cpu_index
 * answers WASMOS_MAX_CPUS for a queue that is not one of g_cpus[] -- which is
 * what keeps an unknown cpu_sched_t (a caller-owned queue) out of the redirect. */
static void test_out_of_table_cpu_id_is_allowed(void) {
    harness_reset();
    cpu_sched_t private_q;
    cpu_sched_init(&private_q);
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 2; /* would forbid CPU 0 if the queue folded onto index 0 */
    act_as(0);
    cpu_sched_enqueue(&private_q, t);
    CHECK(t->rq == &private_q, "an unknown queue is never second-guessed by affinity");
}

/* C4: the BSP is unconditionally online, even with started clear. */
static void test_bsp_is_always_online(void) {
    harness_reset();
    g_cpus[0].started = 0;
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 0;
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->rq == &g_cpus[0].sched, "a BSP-pinned thread is allowed on the BSP");
}

/* C5: a CPU going offline AFTER placement.  The mask then names no online CPU,
 * so the thread becomes stealable by anyone -- which is currently the only thing
 * that stops it being stranded on a dead queue. */
static void test_cpu_going_offline_frees_a_pinned_thread(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    t->cpu_affinity = 1u << 3;
    act_as(3);
    cpu_sched_enqueue(&g_cpus[3].sched, t);
    CHECK(t->rq == &g_cpus[3].sched, "placed on its only allowed CPU");

    g_cpus[3].started = 0; /* CPU 3 goes away */
    act_as(0);
    CHECK(cpu_sched_try_steal(0) == t, "the thread becomes stealable rather than stranded");
    check_invariants("offline recovery");
}

/* C6: affinity is not consumed by a wake -- it survives repeated block/wake
 * round trips from different CPUs. */
static void test_affinity_survives_wake_block_round_trips(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_BLOCKED);
    t->cpu_affinity = 1u << 2;

    act_as(0);
    sched_wake_thread(t);
    CHECK(t->rq == &g_cpus[2].sched, "woken from CPU 0, lands on CPU 2");

    CHECK(pick_on(2) == t, "dispatched by its own CPU");
    t->state = THREAD_STATE_BLOCKED;

    act_as(1);
    sched_wake_thread(t);
    CHECK(t->rq == &g_cpus[2].sched, "woken from CPU 1, still lands on CPU 2");
    CHECK(t->cpu_affinity == (1u << 2), "and the mask itself is unchanged");
    check_invariants("affinity across round trips");
}

/* --------------------------------------------------------- work stealing */

/* Link a thread into a band WITHOUT going through cpu_sched_enqueue, for cases
 * that need a state the enqueue guards now refuse to produce. */
static void force_link(uint32_t cpu, thread_t* t, int prio) {
    cpu_sched_t* cs = &g_cpus[cpu].sched;
    t->rq = cs;
    t->rq_prio = (uint8_t)prio;
    __atomic_store_n(&t->on_rq, 1, __ATOMIC_RELEASE);
    list_head_add_tail(&cs->ready_list[prio], &t->sched_node);
    cs->thread_count[prio]++;
    cs->ready_bitmap |= (uint8_t)(1u << prio);
}

static void test_steal_takes_the_highest_priority_stealable(void) {
    harness_reset();
    thread_t* low = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* high = mk_thread(1, SCHED_PRIO_DRIVER, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, low);
    cpu_sched_enqueue(&g_cpus[1].sched, high);
    act_as(0);
    CHECK(cpu_sched_try_steal(0) == high, "the highest band is taken, not the first enqueued");
}

/* A sticky thread at the HEAD must not shadow the rest of its band.  A band
 * holding only sticky threads cannot distinguish "skips sticky" from "gives up
 * on the band", so a normal thread sits behind the sticky head here. */
static void test_sticky_at_the_head_does_not_block_the_band(void) {
    harness_reset();
    thread_t* sticky = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* normal = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, sticky);
    cpu_sched_enqueue(&g_cpus[1].sched, normal);
    sticky->sched_sticky = 1;

    act_as(0);
    CHECK(cpu_sched_try_steal(0) == normal, "the steal scan looks past a sticky head");
    CHECK(sticky->on_rq == 1, "and the sticky thread stays where it is");
}

/* The steal scan sweeps stale nodes even on a scan that steals nothing. */
static void test_steal_scan_sweeps_stale_nodes(void) {
    harness_reset();
    thread_t* dead = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* sticky = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, dead);
    cpu_sched_enqueue(&g_cpus[1].sched, sticky);
    dead->state = THREAD_STATE_ZOMBIE;
    sticky->sched_sticky = 1;

    act_as(0);
    CHECK(cpu_sched_try_steal(0) == NULL, "nothing stealable");
    CHECK(list_head_empty(&dead->sched_node), "but the stale node was still unlinked");
    CHECK(g_cpus[1].sched.thread_count[SCHED_PRIO_WASM] == 1, "and the counter follows");
    /* The band keeps its bit: the sticky thread is still queued there. */
    CHECK((g_cpus[1].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) != 0,
          "the bit stays while a thread remains");
    check_invariants("steal scan sweep");
}

/* Even force-linked past the enqueue guard, an idle thread is never stolen. */
static void test_idle_is_never_stolen(void) {
    harness_reset();
    thread_t* idle1 = g_cpus[1].idle_thread;
    force_link(1, idle1, SCHED_PRIO_IDLE);
    act_as(0);
    CHECK(cpu_sched_try_steal(0) == NULL, "a linked idle thread is not stealable");
    CHECK(idle1->on_rq == 1, "and is left alone");
}

/* Victim scan starts at my_cpu_id + 1, so CPUs do not all raid CPU 0. */
static void test_victim_scan_order(void) {
    harness_reset();
    thread_t* on1 = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* on2 = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, on1);
    act_as(2);
    cpu_sched_enqueue(&g_cpus[2].sched, on2);
    act_as(0);
    CHECK(cpu_sched_try_steal(0) == on1, "the next CPU up is raided first");
}

/* A contended victim is SKIPPED, not waited on -- try_lock, not lock. */
static void test_contended_victim_is_skipped(void) {
    harness_reset();
    thread_t* on1 = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    thread_t* on2 = mk_thread(1, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, on1);
    act_as(2);
    cpu_sched_enqueue(&g_cpus[2].sched, on2);

    act_as(0);
    ksync_spinlock_lock(&g_cpus[1].sched.lock); /* CPU 1 busy */
    thread_t* got = cpu_sched_try_steal(0);
    ksync_spinlock_unlock(&g_cpus[1].sched.lock);
    CHECK(got == on2, "the locked victim is skipped and the next one raided");
    CHECK(on1->on_rq == 1, "the contended queue is untouched");
}

static void test_all_victims_locked_returns_null(void) {
    harness_reset();
    for (uint32_t c = 1; c < 4; ++c) {
        act_as(c);
        cpu_sched_enqueue(&g_cpus[c].sched, mk_thread((int)c, SCHED_PRIO_WASM, THREAD_STATE_READY));
    }
    act_as(0);
    for (uint32_t c = 1; c < 4; ++c) {
        ksync_spinlock_lock(&g_cpus[c].sched.lock);
    }
    thread_t* got = cpu_sched_try_steal(0);
    for (uint32_t c = 1; c < 4; ++c) {
        ksync_spinlock_unlock(&g_cpus[c].sched.lock);
    }
    CHECK(got == NULL, "all victims contended yields NULL rather than blocking");
}

static void test_single_cpu_never_steals(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    g_cpu_count = 1;
    act_as(0);
    CHECK(cpu_sched_try_steal(0) == NULL, "a single-CPU system has no victims");
    CHECK(t->on_rq == 1, "and nothing is disturbed");
    g_cpu_count = 4;
}

static void test_never_steals_from_itself(void) {
    harness_reset();
    thread_t* mine = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(0);
    cpu_sched_enqueue(&g_cpus[0].sched, mine);
    CHECK(cpu_sched_try_steal(0) == NULL, "a CPU does not raid its own queue");
    CHECK(mine->on_rq == 1, "and its own thread stays queued");
}

/* Post-steal bookkeeping: last_cpu retargeting and the STEALER's steal_count
 * (cpu_local()'s, not the victim's). */
static void test_stolen_thread_bookkeeping(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, t);
    act_as(0);
    uint32_t before = g_cpus[0].steal_count;

    CHECK(cpu_sched_try_steal(0) == t, "stolen");
    CHECK(t->last_cpu == 0, "last_cpu retargeted to the stealer");
    CHECK(g_cpus[0].steal_count == before + 1, "the stealer's steal_count is bumped");
    CHECK(g_cpus[1].sched.thread_count[SCHED_PRIO_WASM] == 0, "the victim's counter drops");
    CHECK((g_cpus[1].sched.ready_bitmap & (1u << SCHED_PRIO_WASM)) == 0, "and its bit clears");
}

/* The hand-off is CALLER-OWNED: a stolen thread is on no queue and unclaimed, so
 * a caller that drops it (the SCHED_R_STALE arm) strands it. */
static void test_stolen_thread_is_on_no_queue(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, t);
    act_as(0);
    (void)cpu_sched_try_steal(0);
    CHECK(t->on_rq == 0, "unclaimed after the steal");
    CHECK(t->rq == 0, "and pointing at no queue");
    CHECK(list_head_empty(&t->sched_node), "and detached -- the caller now owns it");
    check_invariants("post-steal ownership");
}

/* Mirrors P3: only UNUSED/ZOMBIE are swept, so BLOCKED and RUNNING are stolen. */
static void test_steal_returns_blocked_and_running(void) {
    const thread_state_t passed[] = {THREAD_STATE_BLOCKED, THREAD_STATE_RUNNING};
    for (unsigned i = 0; i < sizeof(passed) / sizeof(passed[0]); ++i) {
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
        act_as(1);
        cpu_sched_enqueue(&g_cpus[1].sched, t);
        t->state = passed[i];
        act_as(0);
        CHECK(cpu_sched_try_steal(0) == t, "a non-READY but non-dead node is stealable");
    }
}

static void test_steal_then_enqueue_round_trip(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    act_as(1);
    cpu_sched_enqueue(&g_cpus[1].sched, t);
    act_as(0);
    thread_t* stolen = cpu_sched_try_steal(0);
    CHECK(stolen == t, "stolen");
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    CHECK(t->rq == &g_cpus[0].sched, "re-homed on the stealer");
    CHECK(g_cpus[1].sched.thread_count[SCHED_PRIO_WASM] == 0, "and gone from the victim");
    check_invariants("steal then enqueue");
}

/* ------------------------------------------------------- sched_default_prio */

static void test_default_prio_mapping(void) {
    CHECK(sched_default_prio(1, 0, 0, 0) == SCHED_PRIO_IDLE, "idle");
    CHECK(sched_default_prio(0, 1, 0, 0) == SCHED_PRIO_SYSTEM, "kernel worker");
    CHECK(sched_default_prio(0, 0, 1, 0) == SCHED_PRIO_DRIVER, "driver");
    CHECK(sched_default_prio(0, 0, 0, 1) == SCHED_PRIO_SERVICE, "native service");
    CHECK(sched_default_prio(0, 0, 0, 0) == SCHED_PRIO_WASM, "none set: plain WASM");
}

/* The function is a chain of early returns, so overlapping flags resolve
 * first-match-wins.  One-hot inputs cannot observe that ordering at all -- any
 * permutation of the branches passes them -- so reordering the chain would have
 * been silent. */
static void test_default_prio_precedence_when_flags_overlap(void) {
    CHECK(sched_default_prio(1, 0, 1, 0) == SCHED_PRIO_IDLE, "idle outranks driver");
    CHECK(sched_default_prio(0, 1, 1, 0) == SCHED_PRIO_SYSTEM, "kernel worker outranks driver");
    CHECK(sched_default_prio(0, 0, 1, 1) == SCHED_PRIO_DRIVER, "driver outranks native service");
    CHECK(sched_default_prio(1, 1, 1, 1) == SCHED_PRIO_IDLE, "idle wins over everything");
    CHECK(sched_default_prio(0, 1, 0, 1) == SCHED_PRIO_SYSTEM,
          "kernel worker outranks native service");
}

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"enqueue/pick roundtrip", test_enqueue_pick_roundtrip},
        {"priority ordering and FIFO", test_priority_and_fifo},
        {"empty queue yields per-CPU idle", test_empty_queue_yields_per_cpu_idle},
        {"remove_thread crosses CPUs", test_remove_thread_crosses_cpus},
        {"remove_thread on unqueued is a no-op", test_remove_thread_not_queued_is_noop},
        {"regression: init on a queued node", test_init_on_queued_is_reported},
        {"regression: recycled slot unlinked first", test_recycled_slot_is_unlinked_first},
        {"regression: claim and linkage agree", test_claim_and_linkage_never_disagree},
        {"regression: drifted counter cannot wedge", test_drifted_counter_cannot_wedge_a_band},
        {"regression: zero-filled node reads linked", test_zero_filled_node_reads_as_linked},
        {"reject: non-READY enqueue", test_enqueue_refuses_non_ready},
        {"reject: double enqueue", test_double_enqueue_links_once},
        {"reject: stale nodes at pick", test_pick_drops_stale_nodes_and_falls_back},
        {"reject: NULL inputs", test_null_inputs_are_safe},
        {"steal takes work", test_steal_takes_work_and_respects_exclusions},
        {"steal skips sticky", test_steal_skips_sticky_and_idle},
        {"affinity governs placement", test_affinity_governs_placement},
        {"affinity redirects a forbidden enqueue", test_affinity_redirects_a_forbidden_enqueue},
        {"affinity blocks a steal", test_affinity_blocks_a_steal},
        {"unsatisfiable affinity is not a constraint",
         test_affinity_naming_no_online_cpu_is_not_a_constraint},
        {"anti-starvation demotes after streak", test_antistarvation_demotes_after_streak},
        {"anti-starvation state is per-CPU", test_antistarvation_state_is_per_cpu},
        {"regression: reinit at a new priority", test_reinit_at_a_new_priority_drains_the_old_band},
        {"regression: stale band masks lower", test_stale_band_does_not_mask_lower_bands},
        {"B1 exhaustive band selection", test_exhaustive_band_selection},
        {"B2 bitmap equals occupied set", test_bitmap_equals_the_occupied_set},
        {"B3 bit 7 never indexes the table", test_bit7_never_indexes_the_table},
        {"B4 phantom bit does not wedge", test_phantom_bit_does_not_wedge_the_picker},
        {"B5 draining leaves no residue", test_draining_every_band_leaves_no_residue},
        {"E1 running elsewhere: ready only", test_enqueue_running_elsewhere_marks_ready_only},
        {"E2 running on the calling CPU", test_enqueue_running_on_the_calling_cpu},
        {"E3 running elsewhere, already READY", test_enqueue_running_elsewhere_already_ready},
        {"E4 never resurrects dead states", test_enqueue_never_resurrects_dead_states},
        {"E5 BLOCKED elsewhere is promoted", test_enqueue_promotes_blocked_running_elsewhere},
        {"E6 out-of-enum state refused", test_enqueue_refuses_out_of_enum_state},
        {"E7 claimed-but-linked releases claim", test_claimed_but_linked_releases_the_claim},
        {"E8 allowed-but-loaded not redirected", test_allowed_but_loaded_cpu_is_not_redirected},
        {"E9 redirect prefers last_cpu", test_redirect_prefers_last_cpu},
        {"E10 private queue not redirected", test_private_queue_is_not_redirected},
        {"E11 idle thread never enqueued", test_idle_thread_is_never_enqueued},
        {"E12 out-of-range priority refused", test_out_of_range_priority_is_refused},
        {"F1 enqueue_from routes to calling CPU", test_enqueue_from_routes_to_the_calling_cpu},
        {"F2 enqueue_from non-READY names caller",
         test_enqueue_from_non_ready_reports_the_real_caller},
        {"F3 enqueue_from NULL is silent", test_enqueue_from_null_is_silent},
        {"F4 enqueue_from running elsewhere", test_enqueue_from_running_elsewhere},
        {"R1 remove gives up on in-flight claim",
         test_remove_gives_up_on_a_permanent_in_flight_claim},
        {"R2 remove of detached but claimed", test_remove_of_a_detached_but_claimed_thread},
        {"R3 remove after priority mutation",
         test_remove_after_priority_mutation_drains_the_right_band},
        {"R4 ghost head is reported", test_ghost_head_is_reported},
        {"R5 counter floors at zero", test_counter_floors_at_zero},
        {"R6 dequeue against the wrong queue", test_dequeue_against_the_wrong_queue},
        {"R7 remove releases rq and claim", test_remove_releases_queue_pointer_and_claim},
        {"R8 remove preserves FIFO", test_remove_from_head_middle_tail_preserves_fifo},
        {"A1 streak boundary is exact", test_streak_boundary_is_exact},
        {"A2 demotion picks next lower band", test_demotion_picks_the_next_lower_band},
        {"A3 one slot then back to the top", test_donation_is_one_slot_then_back_to_the_top},
        {"A4 no lower band: no demote, no reset",
         test_no_lower_band_means_no_demotion_and_no_reset},
        {"A5 higher band arrival resets streak", test_higher_band_arrival_resets_the_streak},
        {"A6 idle resets policy state", test_going_idle_resets_the_policy_state},
        {"A7 streak wrap only delays fairness", test_streak_wrap_only_delays_fairness},
        {"A8 sustained fairness ratio", test_sustained_fairness_ratio},
        {"A9 demotion into a stale band", test_demotion_into_a_stale_band_converges},
        {"P1 sweep stops at first live node", test_sweep_stops_at_the_first_live_node},
        {"P2 tid==0 is stale even when READY", test_tid_zero_is_stale_even_when_ready},
        {"P3 BLOCKED/RUNNING/NEW are returned", test_sweep_returns_blocked_running_and_new},
        {"P4 missing idle yields NULL", test_missing_idle_thread_yields_null},
        {"P5 cs->idle vs cpu_local idle", test_cs_idle_and_cpu_local_idle_must_agree},
        {"P6 remote queue returns local idle", test_pick_on_a_remote_queue_returns_local_idle},
        {"P7 all bands stale converges", test_all_bands_stale_converges_to_idle},
        {"D1 refused enqueue leaves a claim", test_refused_enqueue_leaves_a_claim},
        {"D2 holder settles the deferred enqueue", test_holder_settles_the_deferred_enqueue},
        {"D3 sweep settles an untaken claim", test_sweep_settles_a_claim_nobody_took},
        {"D5 sweep never links a running thread", test_sweep_does_not_link_a_running_thread},
        {"D6 sweep drops a blocked thread's claim", test_sweep_drops_a_claim_for_a_blocked_thread},
        {"D4 ordinary enqueue owes nothing", test_ordinary_enqueue_owes_nothing},
        {"D7 tripwires use the locking writer", test_tripwires_use_the_locking_writer},
        {"W1 ordinary wake", test_wake_ordinary},
        {"W2 wake during blocking transition",
         test_wake_during_blocking_transition_defers_with_token},
        {"W3 stale wake of a resumed thread", test_wake_of_a_resumed_thread_is_ignored},
        {"W4 wake of dead states", test_wake_of_dead_states_is_ignored},
        {"W5 double wake links once", test_double_wake_links_once},
        {"W6 wake honours affinity", test_wake_honours_affinity_end_to_end},
        {"W7 resched only on a priority win", test_resched_requested_only_on_a_priority_win},
        {"W8 Dekker: exactly one owner", test_dekker_exactly_one_side_owns_the_enqueue},
        {"W9 stale token no spurious wake", test_stale_token_cannot_force_a_spurious_wake},
        {"W10 caller-CPU-bias arm", test_caller_cpu_bias_arm},
        {"I1 init establishes every field", test_init_establishes_every_field},
        {"I2 init recovers a leaked claim", test_init_recovers_a_leaked_claim},
        {"I3 init discards an existing pinning", test_init_discards_an_existing_pinning},
        {"I4 re-init across every band pair", test_reinit_across_every_band_pair},
        {"I5 re-init of a remotely queued thread", test_reinit_of_a_remotely_queued_thread},
        {"T1 load sums every band", test_load_sums_every_band},
        {"T2 running non-idle counts as load", test_running_non_idle_thread_counts_as_load},
        {"T3 running idle does not count", test_running_idle_thread_does_not_count},
        {"T4 NULL cs->idle makes idle count", test_null_cs_idle_makes_idle_count_as_load},
        {"T5 pick_target selects the minimum", test_pick_target_selects_the_minimum},
        {"T6 ties rotate evenly", test_ties_rotate_evenly},
        {"T9 pick_target ignores affinity", test_pick_target_ignores_affinity_by_design},
        {"T10 _for_thread(NULL) is safe", test_for_thread_null_is_safe},
        {"T11 last_cpu forbidden by affinity", test_prefer_last_cpu_forbidden_by_affinity},
        {"T12 last_cpu out of range", test_prefer_last_cpu_out_of_range},
        {"T13 prefer_last_cpu=0 by load", test_prefer_last_cpu_zero_selects_by_load},
        {"T14 affinity and load interact", test_affinity_and_load_interact},
        {"T15 spawn sets last_cpu", test_spawn_sets_last_cpu_and_enqueues_there},
        {"T16 spawn non-READY partial effect",
         test_spawn_of_a_non_ready_thread_still_rewrites_last_cpu},
        {"T17 spawn with affinity, no redirect", test_spawn_with_affinity_needs_no_redirect},
        {"T7 zero CPU count survivable", test_zero_cpu_count_is_survivable},
        {"T8 CPU count beyond the table", test_cpu_count_beyond_the_table_is_clamped},
        {"C1 partially-online mask restricts", test_partially_online_mask_restricts},
        {"C2 empty mask is unconstrained", test_empty_mask_is_unconstrained},
        {"C3 out-of-table cpu id is allowed", test_out_of_table_cpu_id_is_allowed},
        {"C4 BSP is always online", test_bsp_is_always_online},
        {"C5 offline CPU frees a pinned thread", test_cpu_going_offline_frees_a_pinned_thread},
        {"C6 affinity survives round trips", test_affinity_survives_wake_block_round_trips},
        {"S1 steals the highest band", test_steal_takes_the_highest_priority_stealable},
        {"S2 sticky head does not block band", test_sticky_at_the_head_does_not_block_the_band},
        {"S3 steal scan sweeps stale nodes", test_steal_scan_sweeps_stale_nodes},
        {"S4 idle is never stolen", test_idle_is_never_stolen},
        {"S5 victim scan order", test_victim_scan_order},
        {"S6 contended victim is skipped", test_contended_victim_is_skipped},
        {"S7 all victims locked", test_all_victims_locked_returns_null},
        {"S8 single CPU never steals", test_single_cpu_never_steals},
        {"S9 never steals from itself", test_never_steals_from_itself},
        {"S10 stolen thread bookkeeping", test_stolen_thread_bookkeeping},
        {"S11 stolen thread is on no queue", test_stolen_thread_is_on_no_queue},
        {"S12 steals BLOCKED and RUNNING", test_steal_returns_blocked_and_running},
        {"S13 steal then enqueue round trip", test_steal_then_enqueue_round_trip},
        {"D1 default prio mapping", test_default_prio_mapping},
        {"D2 default prio precedence", test_default_prio_precedence_when_flags_overlap},
    };

    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    const int test_count = (int)(sizeof(tests) / sizeof(tests[0]));
    int order[WASMOS_TEST_MAX_CASES];
    const uint64_t seed = wasmos_test_shuffle(order, test_count);

    for (int i = 0; i < test_count; ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[order[i]].name);
        fflush(stdout);
        tests[order[i]].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[order[i]].name);
        }
    }

    printf("test_sched_runqueue: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
