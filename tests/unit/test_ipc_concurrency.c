/* test_ipc_concurrency.c — the multi-threaded arm for the kernel IPC layer.
 *
 * The SMP claims ipc.c makes -- the lock order between the endpoint table and
 * the per-endpoint lock, the lost-wakeup window ipc_select_wait closes by taking
 * event.lock before releasing the table lock, the rule that a non-blocking poll
 * must not register a waiter -- are only meaningful under real concurrency, and
 * this is where they get it.
 *
 * A pthread here is a CPU: it has its own current thread (a _Thread_local tid)
 * and contends for the same spinlocks and the same tables the kernel does.
 *
 * What a passing run means: no invariant violation was OBSERVED. A race test
 * cannot prove absence, so the value is in the assertions being total — they
 * must hold after ANY interleaving — and in the sanitizer arms. Only which
 * interleavings get explored varies, so this cannot flake in the "sometimes a
 * different answer" sense.
 *
 * MODELLING NOTE. process_yield cannot suspend a host thread, so every blocking
 * wait degrades to "park, get resumed at once, observe a spurious wake". That
 * is a legal outcome of the real API (IPC_EMPTY, caller retries), so the code
 * under test is exercised honestly; what it cannot reproduce is a thread that
 * stays off-CPU for a long time. The stub therefore detaches itself from the
 * wait list the way a real waker would, revalidating under the event lock.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_shuffle.h"

#include "ipc.h"
#include "poll.h"
#include "sched.h"
#include "sched_event.h"
#include "thread.h"

/* How many host threads -- CPUs, in this file's model -- take part, and how many
 * messages each sender pushes. Both are compile-time overridable so a soak run
 * can raise the pressure; NTHREADS must stay at or below POOL_MAX, which is
 * derived from it, and a case that needs a distinct sender and receiver assumes
 * at least two. Raising the round count lengthens every case linearly and is the
 * usual knob for making a rare interleaving show up. */
#ifndef WASMOS_TEST_NTHREADS
#define WASMOS_TEST_NTHREADS 4
#endif
#define NTHREADS WASMOS_TEST_NTHREADS

#ifndef WASMOS_TEST_ROUNDS
#define WASMOS_TEST_ROUNDS 1000
#endif

static int g_failures;
static int g_checks;

/* Record one assertion, countable from any worker: both counters are bumped
 * atomically. A failed condition is counted and printed with its source line and
 * the caller CONTINUES, so one case can report several failures. Workers do not
 * normally call this -- they report through worker_arg_t::result, which the
 * parent turns into a CHECK after the join. */
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        __atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);                                        \
        if (!(cond)) {                                                                             \
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);                                  \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ stubs */

/* One pool entry per host thread, plus slack. g_current_tid is thread-local
 * because a pthread stands in for a CPU: each worker sets it once on entry and
 * every kernel path it then calls sees that as the running thread. A worker that
 * forgets to set it acts as tid 0, which resolves to no thread at all. */
#define POOL_MAX (NTHREADS + 4)
static thread_t g_pool[POOL_MAX];
static _Thread_local uint32_t g_current_tid;

/* The clock stands still: no case here arms a timeout, so nothing needs to
 * advance and a frozen clock keeps runs reproducible. The kernel's timer_ticks
 * counts timer interrupts, and its timer_ms_to_ticks scales by the configured
 * tick rate (rounding up) rather than treating a millisecond as a tick. */
uint64_t timer_ticks(void) {
    return 0;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

/* Index the fake table, bounded by POOL_MAX rather than THREAD_MAX_COUNT, and
 * hand back the slot whatever state it is in -- as the kernel's does. */
thread_t* thread_table_at(uint32_t index) {
    return (index < POOL_MAX) ? &g_pool[index] : 0;
}

/* Map a tid straight onto its pool slot. The kernel takes the thread-table lock
 * and skips THREAD_STATE_UNUSED slots; the direct index here is unlocked, which
 * is safe only because pool_init runs before any worker starts and no slot is
 * ever freed. Out-of-range tids and tid 0 resolve to no thread, as in the
 * kernel. */
thread_t* thread_get(uint32_t tid) {
    if (tid == 0 || tid > POOL_MAX) {
        return 0;
    }
    return &g_pool[tid - 1u];
}

/* The kernel reads the executing CPU's current thread; here the executing host
 * thread carries its own tid, which is the same statement one level down. */
uint32_t thread_current_tid(void) {
    return g_current_tid;
}

/* Publish the new state with a release store so a concurrent reader sees it
 * ordered after whatever the writer did first. The kernel additionally validates
 * the edge against the thread state machine under the table lock and drops an
 * illegal one -- notably any attempt to leave ZOMBIE -- so a transition the real
 * kernel would refuse still lands here. block_reason is written plainly, so it
 * is only meaningful to the thread itself. */
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        __atomic_store_n((uint32_t*)&t->state, (uint32_t)state, __ATOMIC_RELEASE);
        t->block_reason = reason;
    }
}

/* Atomic so the sanitizer arms report races in ipc.c rather than in the stub:
 * a waker on one host thread stores here while the owning thread loads.
 *
 * The kernel's version does considerably more: it claims the wake against the
 * thread's completion path, drops it unless the thread is genuinely BLOCKED,
 * enqueues the thread on a run queue and may request a preemption. Here a wake
 * is only a state store, so a stale or duplicated wake that the real scheduler
 * would discard is applied instead -- which is why the cases assert on message
 * delivery rather than on who was made runnable. */
void sched_wake_thread(thread_t* t) {
    if (t) {
        __atomic_store_n((uint32_t*)&t->state, (uint32_t)THREAD_STATE_READY, __ATOMIC_RELEASE);
    }
}

/* Returns immediately -- see the MODELLING NOTE at the top -- and ends with a
 * host sched_yield, offering the CPU to another worker at exactly the point the
 * kernel would have descheduled the caller. */
void process_yield(process_run_result_t result) {
    (void)result;
    thread_t* self = thread_get(g_current_tid);
    /* Model an immediate resume. A real resume always follows an unlink, so
     * unlink here too — revalidating under the event lock exactly the way
     * sched_timeout_fire does, since a genuine waker may be detaching this
     * thread concurrently. */
    if (self) {
        sched_event_t* ev = (sched_event_t*)__atomic_load_n(&self->wait_event, __ATOMIC_ACQUIRE);
        if (ev) {
            ksync_spinlock_lock(&ev->lock);
            if (self->wait_event == ev && !list_head_empty(&self->event_node)) {
                list_head_del(&self->event_node);
                self->wait_event = 0;
            }
            ksync_spinlock_unlock(&ev->lock);
        }
        __atomic_store_n((uint32_t*)&self->state, (uint32_t)THREAD_STATE_RUNNING, __ATOMIC_RELEASE);
    }
    sched_yield();
}

/* --------------------------------------------------------------- harness */

/* Lay out the fake thread table once, before any worker exists: every slot
 * RUNNING, tid == index + 1 so thread_get is a direct index, and both list nodes
 * self-linked so an unlink on a thread that never blocked is harmless. Called
 * from main only -- there is no between-case reset, because a worker still
 * holding a pool pointer would race one. */
static void pool_init(void) {
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        g_pool[i].tid = i + 1u;
        g_pool[i].state = THREAD_STATE_RUNNING;
        list_head_init(&g_pool[i].event_node);
        list_head_init(&g_pool[i].sched_node);
    }
}

/* A start barrier so the workers actually overlap instead of finishing in
 * launch order. Spin rather than pthread_barrier_t: macOS does not ship one,
 * and spinning keeps the threads hot, which is what produces interleaving. */
typedef struct {
    int count;
    int target;
} spin_barrier_t;
static spin_barrier_t g_spin_barrier;

/* BARRIER_INIT(n) arms the barrier for exactly n arrivals and must run before
 * the workers are created; BARRIER_WAIT() spins until all n have arrived. The
 * barrier is single-use per BARRIER_INIT and there is only one, so cases run one
 * at a time and every worker of a case must reach BARRIER_WAIT or the rest spin
 * forever. */
#define BARRIER_INIT(n)                                                                            \
    do {                                                                                           \
        __atomic_store_n(&g_spin_barrier.target, (int)(n), __ATOMIC_RELEASE);                      \
        __atomic_store_n(&g_spin_barrier.count, 0, __ATOMIC_RELEASE);                              \
    } while (0)
#define BARRIER_WAIT()                                                                             \
    do {                                                                                           \
        __atomic_fetch_add(&g_spin_barrier.count, 1, __ATOMIC_ACQ_REL);                            \
        while (__atomic_load_n(&g_spin_barrier.count, __ATOMIC_ACQUIRE) <                          \
               __atomic_load_n(&g_spin_barrier.target, __ATOMIC_ACQUIRE)) {                        \
            sched_yield();                                                                         \
        }                                                                                          \
    } while (0)

/* Non-zero when the endpoint resolves AND its queue is empty. A failed query
 * reads the same as a non-empty queue, so this answers "drained" only in the
 * affirmative and is used after the workers have joined. */
static int count_of_is_zero_impl(uint32_t ep) {
    uint32_t n = 0xFFFFFFFFu;
    return ipc_endpoint_count(ep, &n) == IPC_OK && n == 0;
}

/* A previously unused owner context id. The counter is bumped atomically, so
 * concurrent callers still get distinct ids; what they do not get is a
 * CONTIGUOUS run of them, which is why a worker needing a block of ids has the
 * parent reserve it. Ids are never recycled within a run, so no case can meet an
 * endpoint an earlier one left under the same owner. */
static uint32_t g_next_ctx = 1000u;
static uint32_t fresh_ctx(void) {
    return __atomic_fetch_add(&g_next_ctx, 1u, __ATOMIC_RELAXED);
}

/* What every worker body receives. run_workers fills tid (index + 1, naming a
 * pool slot) and index (the worker's position in the args array); the case fills
 * ctx, endpoint and rounds before the workers start, and aux/aux2 carry whatever
 * else a particular body needs. result is the worker's only channel back: 0
 * means it finished clean, and anything else is a code that body defines -- an
 * ipc_result_t it did not expect, or a negative sentinel for a violated
 * invariant. The parent reads it after joining, never before. */
typedef struct {
    uint32_t tid;
    uint32_t index;
    uint32_t ctx;
    uint32_t endpoint;
    uint32_t rounds;
    int result;
    void* aux;
    void* aux2;
} worker_arg_t;

/* Start n workers on `fn`, arm the barrier for exactly them, and join them all
 * before returning. args must hold at least n entries and n must not exceed
 * POOL_MAX, since each worker's tid indexes the thread pool. A case that needs
 * two different worker bodies drives pthread_create itself instead. */
static void run_workers(void* (*fn)(void*), worker_arg_t* args, int n) {
    pthread_t th[POOL_MAX];
    BARRIER_INIT(n);
    for (int i = 0; i < n; ++i) {
        args[i].index = (uint32_t)i;
        args[i].tid = (uint32_t)i + 1u;
        pthread_create(&th[i], 0, fn, &args[i]);
    }
    for (int i = 0; i < n; ++i) {
        pthread_join(th[i], 0);
    }
}

/* ------------------------------------------ exactly-once, single receiver */

#define SENDERS (NTHREADS - 1)
#define PER_SENDER WASMOS_TEST_ROUNDS

static uint8_t* g_seen; /* [sender][seq] */

/* Push a->rounds messages into a->endpoint, each tagged with a->index in arg0
 * and its sequence number in arg1 so a receiver can check per-sender order.
 * Sends carry no source, i.e. as the kernel, so no ownership check applies. A
 * full queue is the one transient it retries; any other non-OK code is recorded
 * in a->result and ends the worker with its remaining messages unsent. */
static void* sender_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (uint32_t seq = 0; seq < a->rounds; ++seq) {
        ipc_message_t m;
        memset(&m, 0, sizeof(m));
        m.type = 1u;
        m.source = IPC_ENDPOINT_NONE;
        m.arg0 = a->index;
        m.arg1 = seq;
        for (;;) {
            int rc = ipc_send(a->endpoint, &m);
            if (rc == IPC_OK) {
                break;
            }
            if (rc != IPC_ERR_FULL) {
                a->result = rc; /* only FULL is an acceptable transient */
                return 0;
            }
            sched_yield();
        }
    }
    return 0;
}

/* The only receiver on a->endpoint: drain SENDERS * PER_SENDER messages,
 * marking each in g_seen and checking per-sender order as they arrive. Reports
 * through a->result -- an unexpected ipc code, or -100 for a message nobody
 * sent, -101 for a duplicate, -102 for an out-of-order arrival within one
 * sender, -103 for a destination the send failed to stamp. It runs until the
 * whole expected total arrives, so a message that is lost rather than misrouted
 * hangs the case instead of failing it. */
static void* single_receiver_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    uint32_t next_expected[POOL_MAX];
    uint32_t total = 0;
    memset(next_expected, 0, sizeof(next_expected));
    g_current_tid = a->tid;
    BARRIER_WAIT();
    while (total < (uint32_t)SENDERS * PER_SENDER) {
        ipc_message_t m;
        int rc = ipc_recv_for(a->ctx, a->endpoint, &m);
        if (rc == IPC_EMPTY) {
            sched_yield();
            continue;
        }
        if (rc != IPC_OK) {
            a->result = rc;
            return 0;
        }
        if (m.arg0 >= (uint32_t)SENDERS || m.arg1 >= PER_SENDER) {
            a->result = -100; /* a message that was never sent */
            return 0;
        }
        if (g_seen[m.arg0 * PER_SENDER + m.arg1]) {
            a->result = -101; /* duplicate delivery */
            return 0;
        }
        g_seen[m.arg0 * PER_SENDER + m.arg1] = 1;
        /* One receiver observes the true global order, so per-sender FIFO is
         * checkable: message n+1 from a sender can never arrive before n. */
        if (m.arg1 != next_expected[m.arg0]) {
            a->result = -102;
            return 0;
        }
        next_expected[m.arg0]++;
        if (m.destination != a->endpoint) {
            a->result = -103;
            return 0;
        }
        total++;
    }
    return 0;
}

static void test_exactly_once_and_per_sender_fifo(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    worker_arg_t args[POOL_MAX];
    (void)ipc_endpoint_create(ctx, &ep);
    g_seen = (uint8_t*)calloc((size_t)SENDERS * PER_SENDER, 1);
    memset(args, 0, sizeof(args));
    for (int i = 0; i < NTHREADS; ++i) {
        args[i].ctx = ctx;
        args[i].endpoint = ep;
        args[i].rounds = PER_SENDER;
    }

    /* Thread 0 receives, the rest send. The receiver's index is irrelevant to
     * the senders, which index by args[i].index - so shift the senders down. */
    pthread_t th[POOL_MAX];
    BARRIER_INIT(NTHREADS);
    args[0].tid = 1u;
    pthread_create(&th[0], 0, single_receiver_thread, &args[0]);
    for (int i = 1; i < NTHREADS; ++i) {
        args[i].tid = (uint32_t)i + 1u;
        args[i].index = (uint32_t)i - 1u;
        pthread_create(&th[i], 0, sender_thread, &args[i]);
    }
    for (int i = 0; i < NTHREADS; ++i) {
        pthread_join(th[i], 0);
    }

    int bad = 0;
    for (int i = 0; i < NTHREADS; ++i) {
        if (args[i].result != 0) {
            bad = args[i].result;
        }
    }
    CHECK(bad == 0, "no duplicate, no fabricated message, no out-of-order per-sender delivery");

    uint32_t delivered = 0;
    for (uint32_t i = 0; i < (uint32_t)SENDERS * PER_SENDER; ++i) {
        delivered += g_seen[i] ? 1u : 0u;
    }
    CHECK(delivered == (uint32_t)SENDERS * PER_SENDER, "every message sent was delivered once");
    CHECK(count_of_is_zero_impl(ep), "the queue is drained");

    free(g_seen);
    g_seen = 0;
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------- exactly-once, many receivers */

static uint32_t g_drained;
/* The receiver-side completion target. C1 and C2 use different sender counts,
 * so it cannot be derived from SENDERS. */
static uint32_t g_expect_total;

/* One of several receivers draining the same endpoint, until g_drained reaches
 * g_expect_total. Same sentinels as single_receiver_thread minus the ordering
 * one: with several receivers no single one of them observes the true order.
 * Every receiver exits on the shared count, so a lost message leaves all of them
 * spinning rather than failing. */
static void* multi_receiver_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (;;) {
        if (__atomic_load_n(&g_drained, __ATOMIC_ACQUIRE) >= g_expect_total) {
            return 0;
        }
        ipc_message_t m;
        int rc = ipc_recv_for(a->ctx, a->endpoint, &m);
        if (rc == IPC_EMPTY) {
            sched_yield();
            continue;
        }
        if (rc != IPC_OK) {
            a->result = rc;
            return 0;
        }
        if (m.arg0 * PER_SENDER + m.arg1 >= g_expect_total) {
            a->result = -100; /* a message that was never sent */
            return 0;
        }
        /* Exactly-once across receivers: the claim is the exchange itself. */
        if (__atomic_exchange_n(
                &g_seen[m.arg0 * PER_SENDER + m.arg1], (uint8_t)1, __ATOMIC_ACQ_REL) != 0) {
            a->result = -101;
            return 0;
        }
        __atomic_fetch_add(&g_drained, 1u, __ATOMIC_ACQ_REL);
    }
}

static void test_no_message_is_delivered_twice_across_receivers(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    worker_arg_t args[POOL_MAX];
    (void)ipc_endpoint_create(ctx, &ep);
    /* Half receive, half send — with at least one of each. */
    int receivers = NTHREADS / 2 > 0 ? NTHREADS / 2 : 1;
    int senders = NTHREADS - receivers > 0 ? NTHREADS - receivers : 1;
    g_seen = (uint8_t*)calloc((size_t)senders * PER_SENDER, 1);
    g_drained = 0;
    g_expect_total = (uint32_t)senders * PER_SENDER;
    memset(args, 0, sizeof(args));
    pthread_t th[POOL_MAX];
    BARRIER_INIT(receivers + senders);
    for (int i = 0; i < receivers + senders; ++i) {
        args[i].ctx = ctx;
        args[i].endpoint = ep;
        args[i].tid = (uint32_t)i + 1u;
        args[i].rounds = PER_SENDER;
    }
    for (int i = 0; i < receivers; ++i) {
        pthread_create(&th[i], 0, multi_receiver_thread, &args[i]);
    }
    for (int i = 0; i < senders; ++i) {
        args[receivers + i].index = (uint32_t)i;
        pthread_create(&th[receivers + i], 0, sender_thread, &args[receivers + i]);
    }
    for (int i = 0; i < receivers + senders; ++i) {
        pthread_join(th[i], 0);
    }

    int bad = 0;
    for (int i = 0; i < receivers + senders; ++i) {
        if (args[i].result != 0) {
            bad = args[i].result;
        }
    }
    CHECK(bad == 0, "no message reaches two receivers and none is fabricated");
    CHECK(g_drained == g_expect_total, "every message was accounted for exactly once");

    free(g_seen);
    g_seen = 0;
    ipc_endpoints_release_owner(ctx);
}

/* --------------------------------------------------- concurrent creation */

#define CREATE_PER_THREAD 200
#define CTX_PER_THREAD (CREATE_PER_THREAD / IPC_ENDPOINT_PER_CONTEXT_MAX + 1u)

/* Create CREATE_PER_THREAD endpoints, alternating message and notification, and
 * record their ids in the shared array at a->aux starting at
 * a->index * CREATE_PER_THREAD. The ids are the subject: the case sorts the
 * union afterwards and looks for collisions. The first failing create stops the
 * worker with its code in a->result. */
static void* creator_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    uint32_t* ids = (uint32_t*)a->aux;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (int i = 0; i < CREATE_PER_THREAD; ++i) {
        uint32_t id = 0;
        /* Walk a reserved block of contexts, one per quota's worth: 200 creates
         * is far past any single context's endpoint allowance, and what this
         * asks about is id allocation under contention rather than the quota.
         * The block is reserved by the caller because fresh_ctx() is a plain
         * counter and is not safe to call from these threads. */
        uint32_t ctx = a->ctx + (uint32_t)i / IPC_ENDPOINT_PER_CONTEXT_MAX;
        int rc = (i & 1) ? ipc_notification_create(ctx, &id) : ipc_endpoint_create(ctx, &id);
        if (rc != IPC_OK) {
            a->result = rc;
            return 0;
        }
        ids[a->index * CREATE_PER_THREAD + i] = id;
    }
    return 0;
}

static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a;
    uint32_t y = *(const uint32_t*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void test_concurrent_creation_hands_out_unique_ids(void) {
    worker_arg_t args[POOL_MAX];
    uint32_t* ids = (uint32_t*)calloc((size_t)NTHREADS * CREATE_PER_THREAD, sizeof(uint32_t));
    memset(args, 0, sizeof(args));
    for (int i = 0; i < NTHREADS; ++i) {
        /* Reserve a contiguous block of contexts for this thread, enough that
         * CREATE_PER_THREAD creates fit inside the per-context quota. fresh_ctx
         * hands out consecutive ids, so the thread can walk the block itself. */
        args[i].ctx = fresh_ctx();
        for (uint32_t k = 1; k < CTX_PER_THREAD; ++k) {
            (void)fresh_ctx();
        }
        args[i].aux = ids;
    }
    run_workers(creator_thread, args, NTHREADS);

    int bad = 0;
    for (int i = 0; i < NTHREADS; ++i) {
        if (args[i].result != 0) {
            bad = args[i].result;
        }
    }
    CHECK(bad == 0, "every concurrent create succeeds");

    size_t n = (size_t)NTHREADS * CREATE_PER_THREAD;
    qsort(ids, n, sizeof(uint32_t), cmp_u32);
    int duplicate = 0, reserved = 0;
    for (size_t i = 0; i < n; ++i) {
        if (ids[i] == 0 || ids[i] == IPC_ENDPOINT_NONE) {
            reserved = 1;
        }
        if (i > 0 && ids[i] == ids[i - 1]) {
            duplicate = 1;
        }
    }
    CHECK(!duplicate, "no two concurrently created endpoints share an id");
    CHECK(!reserved, "no reserved id is ever handed out");

    free(ids);
    /* Release every context each thread walked, not just the first. */
    for (int i = 0; i < NTHREADS; ++i) {
        for (uint32_t k = 0; k < CTX_PER_THREAD; ++k) {
            ipc_endpoints_release_owner(args[i].ctx + k);
        }
    }
}

/* ------------------------------------- teardown racing senders and select */

/* The memory-corrupting shape this pins against: a send that read ep->poll_struct
 * under ep->lock but ran poll_notify after releasing it, while
 * ipc_endpoints_release_owner took the same lock and freed that poll_struct.
 * ipc_send_from closes the window by holding ep->lock across poll_notify.
 *
 * The window is only reachable when the endpoint actually has a watcher, so the
 * test keeps a select set registered throughout. Detection depends on the ASan
 * arm; on a plain build a use-after-free here is usually silent. */
typedef struct {
    uint32_t endpoint;
    uint32_t ctx;
    int stop;
} teardown_shared_t;

/* Hammer sends at whichever endpoint the teardown worker currently publishes,
 * until it raises stop. The endpoint id is re-read every round because it is
 * deliberately being destroyed and replaced underneath. */
static void* teardown_sender_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    teardown_shared_t* sh = (teardown_shared_t*)a->aux;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    while (!__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE)) {
        ipc_message_t m;
        memset(&m, 0, sizeof(m));
        m.source = IPC_ENDPOINT_NONE;
        m.arg0 = a->index;
        int rc = ipc_send(__atomic_load_n(&sh->endpoint, __ATOMIC_ACQUIRE), &m);
        /* OK, a transiently full queue, or the endpoint having been torn down
         * concurrently -- and nothing else. NOENT rather than a generic invalid
         * code is the point: the sender can tell a gone endpoint from a
         * malformed argument. */
        if (rc != IPC_OK && rc != IPC_ERR_FULL && rc != IPC_ERR_NOENT) {
            a->result = rc;
            return 0;
        }
    }
    return 0;
}

/* Create an endpoint, put a watching select set on it, publish the endpoint to
 * the senders and tear both down -- a->rounds times, alternating the order of
 * destruction. Reports -1 when the endpoint cannot be created and -2 when the
 * set cannot be built, and either way stops early. Raises stop on the way out,
 * which is what ends the sender workers, so this must be the only worker driving
 * teardown. */
static void* teardown_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    teardown_shared_t* sh = (teardown_shared_t*)a->aux;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (uint32_t round = 0; round < a->rounds; ++round) {
        uint32_t ctx = sh->ctx;
        uint32_t ep = 0, sel = 0;
        if (ipc_endpoint_create(ctx, &ep) != IPC_OK) {
            a->result = -1;
            break;
        }
        if (ipc_select_listen(ctx, &ep, 1, &sel) != IPC_OK) {
            a->result = -2;
            break;
        }
        __atomic_store_n(&sh->endpoint, ep, __ATOMIC_RELEASE);
        sched_yield();
        /* Destroy in both orders across rounds: set-then-endpoint, and
         * endpoint-then-set. The second is the one that leaves a watcher
         * pointing at an endpoint whose poll_struct is being freed. */
        if (round & 1) {
            ipc_select_destroy(sel, ctx);
            ipc_endpoints_release_owner(ctx);
        } else {
            ipc_endpoints_release_owner(ctx);
            ipc_select_destroy(sel, ctx);
        }
    }
    __atomic_store_n(&sh->stop, 1, __ATOMIC_RELEASE);
    return 0;
}

static void test_teardown_racing_senders_is_memory_safe(void) {
    teardown_shared_t sh;
    worker_arg_t args[POOL_MAX];
    uint32_t ctx = fresh_ctx();
    uint32_t placeholder = 0;
    memset(&sh, 0, sizeof(sh));
    memset(args, 0, sizeof(args));
    sh.ctx = ctx;
    (void)ipc_endpoint_create(ctx, &placeholder);
    sh.endpoint = placeholder;

    int senders = NTHREADS - 1 > 0 ? NTHREADS - 1 : 1;
    pthread_t th[POOL_MAX];
    BARRIER_INIT(senders + 1);
    for (int i = 0; i < senders + 1; ++i) {
        args[i].tid = (uint32_t)i + 1u;
        args[i].index = (uint32_t)i;
        args[i].aux = &sh;
        args[i].ctx = ctx;
        args[i].rounds = 200;
    }
    pthread_create(&th[0], 0, teardown_thread, &args[0]);
    for (int i = 0; i < senders; ++i) {
        pthread_create(&th[i + 1], 0, teardown_sender_thread, &args[i + 1]);
    }
    for (int i = 0; i < senders + 1; ++i) {
        pthread_join(th[i], 0);
    }

    int bad = 0;
    for (int i = 0; i < senders + 1; ++i) {
        if (args[i].result != 0) {
            bad = args[i].result;
        }
    }
    CHECK(bad == 0, "a send racing endpoint teardown returns OK, FULL or NOENT and nothing else");
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------- select under contention */

/* The service-side receive loop. select_recv alone is enough -- readiness is
 * level-triggered, so a message stays reported until it is taken -- but the
 * loop still drains every watched endpoint on EMPTY, which is what a real
 * reactor sharing its endpoints with other consumers has to do to be sure it
 * lost a race rather than been starved. */
static void* select_waiter_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    uint32_t sel = (uint32_t)(uintptr_t)a->aux;
    const uint32_t* eps = (const uint32_t*)a->aux2;
    uint32_t got = 0;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    while (got < a->rounds) {
        uint32_t ready = IPC_ENDPOINT_NONE;
        ipc_message_t m;
        int rc = ipc_select_recv(sel, a->ctx, &ready, &m, 0);
        if (rc == IPC_OK) {
            if (ready == IPC_ENDPOINT_NONE) {
                a->result = -110; /* OK must always name an endpoint */
                return 0;
            }
            got++;
            continue;
        }
        if (rc != IPC_EMPTY) {
            a->result = rc;
            return 0;
        }
        for (int i = 0; i < 4; ++i) {
            while (ipc_recv_for(a->ctx, eps[i], &m) == IPC_OK) {
                got++;
            }
        }
        sched_yield();
    }
    return 0;
}

/* A select set is signalled from every sender's CPU while its owner waits. If
 * ipc_select_wait's ready_ep handling or its lost-wakeup ordering were wrong,
 * the waiter would either miss messages or be handed IPC_ENDPOINT_NONE. */
static void test_select_delivers_under_concurrent_signals(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t eps[4] = {0, 0, 0, 0};
    uint32_t sel = 0;
    worker_arg_t args[POOL_MAX];
    int senders = NTHREADS - 1 > 0 ? NTHREADS - 1 : 1;
    uint32_t per = 500;

    for (int i = 0; i < 4; ++i) {
        (void)ipc_endpoint_create(ctx, &eps[i]);
    }
    (void)ipc_select_listen(ctx, eps, 4, &sel);
    memset(args, 0, sizeof(args));

    pthread_t th[POOL_MAX];
    BARRIER_INIT(senders + 1);
    for (int i = 0; i < senders + 1; ++i) {
        args[i].ctx = ctx;
        args[i].tid = (uint32_t)i + 1u;
    }
    args[0].aux = (void*)(uintptr_t)sel;
    args[0].aux2 = eps;
    args[0].rounds = (uint32_t)senders * per;
    pthread_create(&th[0], 0, select_waiter_thread, &args[0]);
    for (int i = 0; i < senders; ++i) {
        args[i + 1].index = (uint32_t)i;
        args[i + 1].endpoint = eps[i % 4];
        args[i + 1].rounds = per;
        pthread_create(&th[i + 1], 0, sender_thread, &args[i + 1]);
    }
    for (int i = 0; i < senders + 1; ++i) {
        pthread_join(th[i], 0);
    }

    int bad = 0;
    for (int i = 0; i < senders + 1; ++i) {
        if (args[i].result != 0) {
            bad = args[i].result;
        }
    }
    CHECK(bad == 0, "the select waiter receives every message and is never handed a NONE endpoint");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* --------------------------------------------- select table slot accounting */

/* Create a select set, point it at a->endpoint and destroy it again, a->rounds
 * times, against the context every churn worker shares. A FULL create is
 * expected -- they contend for one per-context allowance -- and consumes its
 * round rather than being retried; any other non-OK code, or a zero id from a
 * create that claimed to succeed (-120), ends the worker through a->result. */
static void* select_churn_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (uint32_t i = 0; i < a->rounds; ++i) {
        uint32_t sel = 0;
        int rc = ipc_select_create(a->ctx, &sel);
        if (rc == IPC_ERR_FULL) {
            sched_yield();
            /* Legitimate: every churn thread creates against the SAME context,
             * so they contend for one IPC_SELECT_PER_CONTEXT_MAX allowance. */
            continue;
        }
        if (rc != IPC_OK) {
            a->result = rc;
            return 0;
        }
        if (sel == 0) {
            a->result = -120;
            return 0;
        }
        (void)ipc_select_add(sel, a->endpoint, a->ctx);
        ipc_select_destroy(sel, a->ctx);
    }
    return 0;
}

/* Every slot handed out must come back. A create/destroy race that lost slots
 * would silently cap how many services can ever run; one that handed the same
 * slot to two threads would cross-wire two services' readiness. */
static void test_select_slots_are_conserved_under_churn(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    worker_arg_t args[POOL_MAX];
    (void)ipc_endpoint_create(ctx, &ep);
    memset(args, 0, sizeof(args));
    for (int i = 0; i < NTHREADS; ++i) {
        args[i].ctx = ctx;
        args[i].endpoint = ep;
        args[i].rounds = 500;
    }
    run_workers(select_churn_thread, args, NTHREADS);

    int bad = 0;
    for (int i = 0; i < NTHREADS; ++i) {
        if (args[i].result != 0) {
            bad = args[i].result;
        }
    }
    CHECK(bad == 0, "no churn thread saw an invalid select id");

    /* Nothing the churn allocated may still be held. The table grows on demand,
     * so "every slot came back" is not a count of slots -- it is the churning
     * context being able to claim its whole quota again, which it cannot do if a
     * single set leaked. */
    uint32_t ids[IPC_SELECT_PER_CONTEXT_MAX];
    uint32_t n = 0;
    while (n < IPC_SELECT_PER_CONTEXT_MAX) {
        uint32_t sel = 0;
        if (ipc_select_create(ctx, &sel) != IPC_OK) {
            break;
        }
        ids[n++] = sel;
    }
    CHECK(n == IPC_SELECT_PER_CONTEXT_MAX, "every set the churn allocated was returned");
    for (uint32_t i = 0; i < n; ++i) {
        ipc_select_destroy(ids[i], ctx);
    }
    ipc_endpoints_release_owner(ctx);
}

/* -------------------------------------------------------------------- main */

/* Lay out the thread pool and initialise the IPC layer once, then run every case
 * in a shuffled order. Returns 0 only when every CHECK passed and 1 otherwise;
 * on failure the shuffle seed is printed so the order can be replayed through
 * WASMOS_TEST_SEED. Neither the pool nor the endpoint table is reset between
 * cases, so each works under its own fresh_ctx() and releases it. */
int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"C1 exactly-once and per-sender FIFO", test_exactly_once_and_per_sender_fifo},
        {"C2 no double delivery across receivers",
         test_no_message_is_delivered_twice_across_receivers},
        {"C3 concurrent creation gives unique ids", test_concurrent_creation_hands_out_unique_ids},
        {"C4 teardown racing senders is memory safe", test_teardown_racing_senders_is_memory_safe},
        {"C5 select delivers under concurrent signals",
         test_select_delivers_under_concurrent_signals},
        {"C6 select slots are conserved under churn", test_select_slots_are_conserved_under_churn},
    };

    pool_init();
    ipc_init();

    printf("test_ipc_concurrency: %d threads, %d rounds\n", NTHREADS, WASMOS_TEST_ROUNDS);
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
    printf("test_ipc_concurrency: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}