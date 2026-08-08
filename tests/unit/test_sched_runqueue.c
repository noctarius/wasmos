/* test_sched_runqueue.c — host-side invariant tests for the real sched_thread.c.
 *
 * This drives the ACTUAL scheduler source, not a model of it, with the host
 * standing in for the CPUs: cpu_local() resolves through g_host_cpu_local (see
 * the WASMOS_HOST_TEST_SMP arm in arch/x86_64/smp.h), so act_as(n) makes the
 * following calls run "on CPU n".  That gives deterministic multi-CPU coverage
 * with no threads, which is what a unit gate wants -- every ordering here is
 * one the kernel can actually produce, just chosen rather than raced for.
 *
 * The centrepiece is check_invariants(): the run-queue corruption that cost
 * ~180 QEMU boots to localise is a structural violation that a walk of the
 * queues catches immediately.  Each regression test below reproduces a bug we
 * actually shipped; each rejection test pins an input the API must refuse.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sched.h"
#include "thread.h"
#include "arch/x86_64/smp.h"

/* ------------------------------------------------------------------ harness */

cpu_local_t g_cpus[WASMOS_MAX_CPUS];
uint32_t g_cpu_count = 4;
_Thread_local cpu_local_t* g_host_cpu_local;

#define POOL_MAX 32
static thread_t g_pool[POOL_MAX];

static int g_failures;
static int g_checks;

/* Captured tripwire output.  The scheduler reports invariant violations through
 * serial_printf_unlocked rather than halting, so tests assert on what it said. */
#define LOG_MAX 64
#define LOG_LINE 256
static char g_log[LOG_MAX][LOG_LINE];
static int g_log_count;

void serial_printf_unlocked(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log_count < LOG_MAX) {
        vsnprintf(g_log[g_log_count], LOG_LINE, fmt, ap);
        g_log_count++;
    }
    va_end(ap);
}

void process_set_need_resched(void) {}

/* sched_thread_init initialises the thread's join_event.  Mirrored rather than
 * linked: the real sched_event.c drags in the timer and thread table, and no
 * test here exercises events. */
void sched_event_init(sched_event_t* ev, sched_event_type_t type) {
    ksync_spinlock_init(&ev->lock);
    list_head_init(&ev->wait_list);
    ev->cnt = 0;
    ev->type = type;
}

/* Real CAS semantics: the scheduler relies on losing this race, not on being
 * the only writer, so a stub that always succeeds would hide exactly the bugs
 * these tests exist for. */
int thread_transit(thread_t* t, thread_state_t from, thread_state_t to) {
    uint32_t expected = (uint32_t)from;
    if (!t) {
        return 0;
    }
    return __atomic_compare_exchange_n((uint32_t*)&t->state, &expected, (uint32_t)to, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int thread_wake_if_blocked(uint32_t tid) {
    for (int i = 0; i < POOL_MAX; ++i) {
        if (g_pool[i].tid == tid) {
            return thread_transit(&g_pool[i], THREAD_STATE_BLOCKED, THREAD_STATE_READY);
        }
    }
    return 0;
}

static void act_as(uint32_t cpu) { g_host_cpu_local = &g_cpus[cpu]; }

static int saw(const char* needle) {
    for (int i = 0; i < g_log_count; ++i) {
        if (strstr(g_log[i], needle)) {
            return 1;
        }
    }
    return 0;
}

static void log_reset(void) { g_log_count = 0; }

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
 * LINKED.  That misreading produced 6-7 false tripwire reports per boot. */
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

static void harness_reset(void) {
    memset(g_cpus, 0, sizeof(g_cpus));
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        cpu_sched_init(&g_cpus[i].sched);
        g_cpus[i].cpu_id = i;
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
    act_as(0);
}

/* -------------------------------------------------------- invariant checker */

/* Walks every band of every CPU and asserts the structural facts the run queue
 * must always satisfy.  Bounded so a cycle -- the exact shape of the storm --
 * terminates and reports instead of hanging the test. */
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
                    printf("  [FAIL] %s: queued non-READY tid=%u state=%u\n", where, t->tid,
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
                printf("  [FAIL] %s: cpu%u band%d count=%u walked=%u\n", where, c, p,
                       cs->thread_count[p], walked);
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

static thread_t* pick_on(uint32_t cpu) {
    act_as(cpu);
    cpu_sched_t* cs = &g_cpus[cpu].sched;
    ksync_spinlock_lock(&cs->lock);
    thread_t* t = cpu_sched_pick_next(cs);
    ksync_spinlock_unlock(&cs->lock);
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

/* ------------------------------------- Kind 1: bugs we actually shipped */

/* Round 2 (aa3db5c160).  process_thread_spawn_worker_internal published a
 * thread READY before its sched_node existed; a wake in that window enqueued
 * it, and sched_thread_init's list_head_init then self-linked the node while
 * the band still pointed at it -- the ghost that gets re-picked forever. */
static void test_init_on_queued_is_reported(void) {
    harness_reset();
    thread_t* t = mk_thread(0, SCHED_PRIO_WASM, THREAD_STATE_READY);
    cpu_sched_enqueue(&g_cpus[0].sched, t);
    log_reset();

    sched_thread_init(t, SCHED_PRIO_WASM); /* the corrupting write */
    CHECK(saw("init on queued"), "re-init of a queued node is reported");
}

/* Round 1 (10f5f3eaa1).  thread_reset_slot freed a slot whose sched_node was
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
    CHECK(!saw("init on queued"), "a properly unlinked slot re-inits silently");
    cpu_sched_enqueue(&g_cpus[0].sched, reborn);
    check_invariants("recycled slot requeued");
    CHECK(g_cpus[0].sched.thread_count[SCHED_PRIO_WASM] == 0, "old band left empty");
}

/* The round-1 REGRESSION.  The claim was taken before t->rq was published, and
 * remove_thread read rq == 0 as "not queued" and cleared the claim under the
 * in-flight enqueue -- leaving the node linked but unclaimed, hence linkable
 * twice.  on_rq must never be observable as 0 while the node is linked. */
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
 * reads as LINKED.  thread_reset_slot must leave it canonically detached; this
 * documents the trap that produced 6-7 false reports per boot. */
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
    const thread_state_t bad[] = {THREAD_STATE_BLOCKED, THREAD_STATE_ZOMBIE, THREAD_STATE_RUNNING,
                                  THREAD_STATE_NEW};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        harness_reset();
        thread_t* t = mk_thread(0, SCHED_PRIO_WASM, bad[i]);
        cpu_sched_enqueue(&g_cpus[0].sched, t);
        CHECK(list_head_empty(&t->sched_node), "non-READY thread is not linked");
        CHECK(t->on_rq == 0, "non-READY thread is not claimed");
        /* Only the first is asserted to be reported.  The tripwire is
         * rate-limited to powers of two and its counter is a function-static
         * that persists across tests, so refusals 4, 6, 7 ... are silent BY
         * DESIGN -- the storm this guards against would otherwise flood serial
         * at scheduler speed.  Behaviour above is checked every iteration; the
         * message is not a contract. */
        if (i == 0) {
            CHECK(saw("enqueue non-ready"), "the first refusal is reported");
        }
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
    };

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[i].name);
        }
    }

    printf("test_sched_runqueue: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
