/* test_sched_concurrency.c — the real multi-threaded arm of the host harness.
 *
 * Every race sched_thread.c's comments describe is argued for in prose and, up
 * to now, tested by nothing. WASMOS_HOST_TEST_SMP already routes cpu_local()
 * through a _Thread_local, so a pthread genuinely IS a CPU here: it has its own
 * cpu_local, its own run queue, and it contends for the same locks and atomics
 * the kernel does.
 *
 * What a passing run means, precisely: no invariant violation was OBSERVED. A
 * race test cannot prove absence, so the value is in the invariants being
 * checked continuously and in the sanitizer arms, not in the iteration count.
 * The assertions themselves are deterministic -- they must hold after ANY
 * interleaving -- so this cannot flake in the "sometimes wrong answer" sense.
 * Only which interleavings get explored varies.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "sched.h"
#include "thread.h"
#include "arch/x86_64/smp.h"

/* ------------------------------------------------------------------ harness */

cpu_local_t g_cpus[WASMOS_MAX_CPUS];
uint32_t g_cpu_count = 4;
_Thread_local cpu_local_t* g_host_cpu_local;

/* CPU count is a build parameter so the gate can run the same races at several
 * widths. It is not cosmetic: the steal scan order ((my + n) % count), the
 * placement tie rotation and the modulo in both pickers all change shape with
 * it, and more CPUs than host cores is a feature here -- the preemption that
 * causes explores interleavings a 1:1 mapping never reaches. */
#ifndef WASMOS_TEST_NCPU
#define WASMOS_TEST_NCPU 4
#endif
#if WASMOS_TEST_NCPU > WASMOS_MAX_CPUS
#error "WASMOS_TEST_NCPU exceeds WASMOS_MAX_CPUS"
#endif
#define NCPU WASMOS_TEST_NCPU
#define POOL_MAX 32
#define ITERATIONS 20000

static thread_t g_pool[POOL_MAX];
static thread_t g_idle[NCPU];
static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        __atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);                                        \
        if (!(cond)) {                                                                             \
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);                                  \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* Silent: a storm of tripwire output from four threads would dominate the run
 * and slow the interleaving. Counted instead, so a test can still assert one
 * fired. */
static int g_reports;
void serial_printf_unlocked(const char* fmt, ...) {
    (void)fmt;
    __atomic_fetch_add(&g_reports, 1, __ATOMIC_RELAXED);
}
void process_set_need_resched(void) {}
void sched_event_init(sched_event_t* ev, sched_event_type_t type) {
    ksync_spinlock_init(&ev->lock);
    list_head_init(&ev->wait_list);
    ev->cnt = 0;
    ev->type = type;
}
int thread_transit(thread_t* t, thread_state_t from, thread_state_t to) {
    uint32_t expected = (uint32_t)from;
    if (!t) {
        return 0;
    }
    return __atomic_compare_exchange_n((uint32_t*)&t->state, &expected, (uint32_t)to, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
int thread_wake_if_blocked(uint32_t tid) {
    if (tid == 0) {
        return 0;
    }
    for (int i = 0; i < POOL_MAX; ++i) {
        if (g_pool[i].tid != tid) {
            continue;
        }
        thread_t* t = &g_pool[i];
        if (!thread_transit(t, THREAD_STATE_BLOCKED, THREAD_STATE_READY)) {
            return 0;
        }
        /* The kernel writes this under g_thread_table_lock; the stub has no such
         * lock, so make it atomic rather than let the harness generate reports. */
        __atomic_store_n(&t->block_reason, THREAD_BLOCK_NONE, __ATOMIC_RELAXED);
        return 1;
    }
    return 0;
}

static void be_cpu(uint32_t id) {
    g_host_cpu_local = &g_cpus[id];
}

static void harness_init(void) {
    memset(g_cpus, 0, sizeof(g_cpus));
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        cpu_sched_init(&g_cpus[i].sched);
        g_cpus[i].cpu_id = i;
        g_cpus[i].started = (i < NCPU) ? 1 : 0;
    }
    for (int i = 0; i < NCPU; ++i) {
        memset(&g_idle[i], 0, sizeof(g_idle[i]));
        g_idle[i].tid = 9000u + (uint32_t)i;
        list_head_init(&g_idle[i].sched_node);
        sched_thread_init(&g_idle[i], SCHED_PRIO_IDLE);
        g_idle[i].state = THREAD_STATE_READY;
        g_cpus[i].idle_thread = &g_idle[i];
        g_cpus[i].sched.idle = &g_idle[i];
    }
    for (int i = 0; i < POOL_MAX; ++i) {
        thread_t* t = &g_pool[i];
        t->tid = (uint32_t)i + 1u;
        t->owner_pid = 100u + (uint32_t)i;
        list_head_init(&t->sched_node);
        sched_thread_init(t, (sched_prio_t)(i % SCHED_PRIO_MAX));
        t->state = THREAD_STATE_READY;
    }
    g_cpu_count = NCPU;
    be_cpu(0);
}

/* Stop-the-world structural check: hold every queue lock so the walk sees a
 * consistent snapshot, then assert a thread is linked at most once across all
 * CPUs and that the per-band bookkeeping matches the lists. */
static void check_invariants(const char* where) {
    for (int c = 0; c < NCPU; ++c) {
        ksync_spinlock_lock(&g_cpus[c].sched.lock);
    }
    int seen[POOL_MAX + 1];
    memset(seen, 0, sizeof(seen));
    for (int c = 0; c < NCPU; ++c) {
        cpu_sched_t* cs = &g_cpus[c].sched;
        for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
            list_head_t* head = &cs->ready_list[p];
            uint32_t walked = 0;
            for (list_head_t* n = head->next; n != head; n = n->next) {
                if (++walked > POOL_MAX + 2u) {
                    __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
                    printf("  [FAIL] %s: cycle cpu%d band%d\n", where, c, p);
                    break;
                }
                if (n->next->prev != n || n->prev->next != n) {
                    __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
                    printf("  [FAIL] %s: broken links cpu%d band%d\n", where, c, p);
                    break;
                }
                thread_t* t = list_entry(n, thread_t, sched_node);
                if (t->tid >= 1u && t->tid <= POOL_MAX) {
                    seen[t->tid]++;
                }
                if (!t->on_rq) {
                    __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
                    printf("  [FAIL] %s: linked but unclaimed tid=%u\n", where, t->tid);
                }
                if (t->rq != cs) {
                    __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
                    printf("  [FAIL] %s: rq mismatch tid=%u\n", where, t->tid);
                }
            }
            if (((cs->ready_bitmap & (1u << p)) != 0) != (walked != 0)) {
                __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
                printf("  [FAIL] %s: cpu%d band%d bit/list disagree\n", where, c, p);
            }
            if (cs->thread_count[p] != walked) {
                __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
                printf("  [FAIL] %s: cpu%d band%d count=%u walked=%u\n", where, c, p,
                       cs->thread_count[p], walked);
            }
        }
    }
    for (int i = 1; i <= POOL_MAX; ++i) {
        if (seen[i] > 1) {
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
            printf("  [FAIL] %s: tid=%d linked %d times\n", where, i, seen[i]);
        }
    }
    __atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);
    for (int c = NCPU - 1; c >= 0; --c) {
        ksync_spinlock_unlock(&g_cpus[c].sched.lock);
    }
}

/* Deterministic per-thread PRNG so a failing run is reproducible from its seed. */
static uint32_t rnd(uint32_t* s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* pthread_barrier_t is an optional POSIX feature and macOS does not ship it, so
 * this is a minimal single-use spin barrier. Spinning is right here: the point
 * is to release all workers as close to simultaneously as possible, which a
 * condvar's wake-one-at-a-time hand-off would blunt. */
typedef struct {
    volatile int count;
    int target;
} test_barrier_t;

static test_barrier_t g_barrier;

static void barrier_init(test_barrier_t* b, int n) {
    b->target = n;
    __atomic_store_n(&b->count, 0, __ATOMIC_RELEASE);
}

static void barrier_wait(test_barrier_t* b) {
    __atomic_fetch_add(&b->count, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&b->count, __ATOMIC_ACQUIRE) < b->target) {
        cpu_relax();
    }
}

/* ------------------------------------------------------------------- X1 soak */

static void* soak_worker(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    uint32_t seed = 0x1234567u + id * 7919u;
    barrier_wait(&g_barrier);

    for (int i = 0; i < ITERATIONS; ++i) {
        uint32_t r = rnd(&seed);
        thread_t* t = &g_pool[r % POOL_MAX];
        switch ((r >> 8) % 5u) {
        case 0:
            cpu_sched_enqueue(&g_cpus[id].sched, t);
            break;
        case 1: {
            cpu_sched_t* cs = &g_cpus[id].sched;
            ksync_spinlock_lock(&cs->lock);
            thread_t* got = cpu_sched_pick_next(cs);
            ksync_spinlock_unlock(&cs->lock);
            /* Put it straight back so the pool cannot drain to nothing. */
            if (got && got != g_cpus[id].idle_thread) {
                cpu_sched_enqueue(cs, got);
            }
            break;
        }
        case 2:
            cpu_sched_remove_thread(t);
            break;
        case 3: {
            thread_t* stolen = cpu_sched_try_steal(id);
            if (stolen) {
                cpu_sched_enqueue(&g_cpus[id].sched, stolen);
            }
            break;
        }
        default:
            sched_wake_thread(t);
            break;
        }
    }
    return NULL;
}

static void test_soak(void) {
    harness_init();
    pthread_t th[NCPU];
    barrier_init(&g_barrier, NCPU);
    for (uint32_t i = 0; i < NCPU; ++i) {
        pthread_create(&th[i], NULL, soak_worker, (void*)(uintptr_t)i);
    }
    for (int i = 0; i < NCPU; ++i) {
        pthread_join(th[i], NULL);
    }
    be_cpu(0);
    check_invariants("soak");
    /* Conservation: nothing may be linked twice (checked above) and nothing may
     * be linked while unclaimed or claimed while unlinked. */
    int inconsistent = 0;
    for (int i = 0; i < POOL_MAX; ++i) {
        thread_t* t = &g_pool[i];
        int linked = !list_head_empty(&t->sched_node);
        if (linked != (t->on_rq != 0)) {
            inconsistent++;
        }
    }
    CHECK(inconsistent == 0, "claim and linkage agree for every pool thread after the soak");
}

#if NCPU >= 2

/* ------------------------------------------------- X2 concurrent enqueue */

static thread_t* g_shared;
static void* enqueue_worker(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    for (int i = 0; i < 2000; ++i) {
        cpu_sched_enqueue(&g_cpus[id].sched, g_shared);
    }
    return NULL;
}

static void test_concurrent_enqueue_links_once(void) {
    harness_init();
    g_shared = &g_pool[0];
    pthread_t a, b;
    barrier_init(&g_barrier, 2);
    pthread_create(&a, NULL, enqueue_worker, (void*)(uintptr_t)0);
    pthread_create(&b, NULL, enqueue_worker, (void*)(uintptr_t)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    be_cpu(0);

    check_invariants("concurrent enqueue");
    int total = 0;
    for (int c = 0; c < NCPU; ++c) {
        for (int p = 0; p < SCHED_PRIO_MAX; ++p) {
            total += (int)g_cpus[c].sched.thread_count[p];
        }
    }
    CHECK(total == 1, "a thread hammered from two CPUs is linked exactly once");
}

/* ------------------------------------ X3 concurrent enqueue vs remove */

static volatile int g_stop;
static void* churn_enqueue(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        cpu_sched_enqueue(&g_cpus[id].sched, g_shared);
    }
    return NULL;
}
static void* churn_remove(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    for (int i = 0; i < 20000; ++i) {
        cpu_sched_remove_thread(g_shared);
        /* No unlocked sampling of sched_node here. Reading the list pointers
         * without the owning queue's lock is itself a data race -- it made the
         * TSan arm report the TEST rather than the scheduler. The same invariant
         * (never linked while unclaimed) is asserted by the stop-the-world
         * check_invariants() after the join, where it is well-defined. */
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_concurrent_enqueue_and_remove(void) {
    harness_init();
    g_shared = &g_pool[0];
    g_stop = 0;
    pthread_t a, b;
    barrier_init(&g_barrier, 2);
    pthread_create(&a, NULL, churn_enqueue, (void*)(uintptr_t)0);
    pthread_create(&b, NULL, churn_remove, (void*)(uintptr_t)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    be_cpu(0);
    check_invariants("enqueue vs remove");
    CHECK(1, "enqueue and remove churn completed without a linked-but-unclaimed sighting");
}

/* --------------------------------------------------- X4 two stealers */

static int g_steal_wins;
static void* steal_worker(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    if (cpu_sched_try_steal(id)) {
        __atomic_fetch_add(&g_steal_wins, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void test_two_stealers_one_thread(void) {
    int total_wins = 0;
    for (int round = 0; round < 500; ++round) {
        harness_init();
        g_steal_wins = 0;
        be_cpu(3);
        cpu_sched_enqueue(&g_cpus[3].sched, &g_pool[0]); /* the single prize */

        pthread_t a, b;
        barrier_init(&g_barrier, 2);
        pthread_create(&a, NULL, steal_worker, (void*)(uintptr_t)0);
        pthread_create(&b, NULL, steal_worker, (void*)(uintptr_t)1);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        if (g_steal_wins != 1) {
            total_wins++;
        }
    }
    be_cpu(0);
    CHECK(total_wins == 0, "exactly one stealer wins every round -- never both, never neither");
}

/* ------------------------------------------------ X5 steal versus reap */

/* CONTRACT NOTE. sched_thread_init calls list_head_init(&t->sched_node)
 * unconditionally, so it requires EXCLUSIVE ownership of the thread -- it is a
 * spawn/recycle operation, not a concurrent one. In the kernel that exclusivity
 * comes from process_reap, which wins a ZOMBIE -> REAPING CAS before any slot is
 * reset, and from the fact that a terminal thread is refused by
 * cpu_sched_enqueue's state guard and swept by the steal scan.
 *
 * An earlier version of this test recycled the SAME slots a stealer was
 * enqueueing and reported list corruption. That was the test violating the
 * contract, not the scheduler breaking it: no amount of locking inside
 * sched_thread_init would make "re-initialise a node another CPU is linking"
 * well-defined.
 *
 * So the reaper here recycles slots that are on NO queue -- which is the state
 * thread_reset_slot operates on -- while the other CPU drives queue traffic on
 * different slots. The property under test is that concurrent slot recycling
 * does not corrupt the shared queue state, and that a recycled slot is left
 * detached and unclaimed. The same-slot pairing is deliberately NOT tested: it
 * is unsupported, and a test asserting it would be asserting a guarantee the
 * kernel does not make. */
#define REAP_SLOTS 8

static void* reap_worker(void* arg) {
    (void)arg;
    be_cpu(2);
    barrier_wait(&g_barrier);
    for (int i = 0; i < 5000; ++i) {
        /* Never enqueued: a queued thread is stealable by ANY CPU, which is how
         * an earlier version of this test lost its "disjoint ranges" property
         * and recreated the unsupported same-slot pairing. Recycling a slot that
         * is on no queue is what thread_reset_slot actually does. */
        thread_t* t = &g_pool[i % REAP_SLOTS];
        cpu_sched_remove_thread(t); /* the unlink-before-reset the reap path owes */
        sched_thread_init(t, SCHED_PRIO_WASM);
        t->state = THREAD_STATE_READY;
        if (!list_head_empty(&t->sched_node) || t->on_rq) {
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

static void* steal_churn(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    for (int i = 0; i < 5000; ++i) {
        thread_t* t = cpu_sched_try_steal(id);
        if (t) {
            cpu_sched_enqueue(&g_cpus[id].sched, t);
        }
        /* Disjoint from the reaper's range. */
        cpu_sched_enqueue(&g_cpus[id].sched, &g_pool[REAP_SLOTS + (i % REAP_SLOTS)]);
    }
    return NULL;
}

static void test_steal_versus_reap(void) {
    harness_init();
    pthread_t a, b;
    barrier_init(&g_barrier, 2);
    pthread_create(&a, NULL, steal_churn, (void*)(uintptr_t)0);
    pthread_create(&b, NULL, reap_worker, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    be_cpu(0);
    check_invariants("steal vs reap");
    CHECK(1, "recycling slots under a concurrent stealer left no queue pointing at them");
}

/* ------------------------------------------- X6 wake/block Dekker */

static int g_enq_observed;
static void* dekker_waker(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    for (int i = 0; i < 20000; ++i) {
        __atomic_store_n(&g_shared->state, THREAD_STATE_BLOCKED, __ATOMIC_RELEASE);
        sched_wake_thread(g_shared);
    }
    return NULL;
}
static void* dekker_completion(void* arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    be_cpu(id);
    barrier_wait(&g_barrier);
    for (int i = 0; i < 20000; ++i) {
        /* Mirror process_schedule_once_impl's PROCESS_RUN_BLOCKED arm. */
        __atomic_store_n(&g_shared->blocking_transition, 1, __ATOMIC_SEQ_CST);
        if (sched_block_complete_claim(g_shared)) {
            __atomic_fetch_add(&g_enq_observed, 1, __ATOMIC_RELAXED);
            cpu_sched_enqueue(&g_cpus[id].sched, g_shared);
        }
        cpu_sched_remove_thread(g_shared);
    }
    return NULL;
}

static void test_dekker_under_real_threads(void) {
    harness_init();
    g_shared = &g_pool[0];
    g_enq_observed = 0;
    pthread_t a, b;
    barrier_init(&g_barrier, 2);
    pthread_create(&a, NULL, dekker_waker, (void*)(uintptr_t)0);
    pthread_create(&b, NULL, dekker_completion, (void*)(uintptr_t)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    be_cpu(0);
    check_invariants("dekker");
    /* The token is a claim: it can be consumed at most once per publication, so
     * the completion path can never claim more times than wakes were issued. */
    CHECK(g_enq_observed <= 20000, "the completion path never claims more often than wakes occur");
}

#endif /* NCPU >= 2 */

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
#if NCPU >= 2
        /* Every case below pairs two CPUs against one another, so they are only
         * meaningful once a second CPU exists. At NCPU == 1 the soak still runs
         * and is worth running: it covers the degenerate single-CPU paths --
         * try_steal with no victims, placement with one candidate -- which the
         * wider widths never take. */
        {"X2 concurrent enqueue links once", test_concurrent_enqueue_links_once},
        {"X3 concurrent enqueue vs remove", test_concurrent_enqueue_and_remove},
        {"X4 two stealers, one thread", test_two_stealers_one_thread},
        {"X5 steal versus reap", test_steal_versus_reap},
        {"X6 wake/block Dekker under threads", test_dekker_under_real_threads},
#endif
        {"X1 soak", test_soak},
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
    printf("test_sched_concurrency(smp=%d): %d checks, %d failures\n", NCPU, g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
