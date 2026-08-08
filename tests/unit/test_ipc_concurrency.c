/* test_ipc_concurrency.c — the multi-threaded arm for the kernel IPC layer.
 *
 * ipc.c is full of prose about SMP: the lock order between the endpoint table
 * and the per-endpoint lock, the lost-wakeup window ipc_select_wait closes by
 * taking event.lock before releasing the table lock, the comment on why a
 * non-blocking poll must not register a waiter. None of it was executed by more
 * than one thread until now.
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

#include "ipc.h"
#include "poll.h"
#include "sched.h"
#include "sched_event.h"
#include "thread.h"

#ifndef WASMOS_TEST_NTHREADS
#define WASMOS_TEST_NTHREADS 4
#endif
#define NTHREADS WASMOS_TEST_NTHREADS

#ifndef WASMOS_TEST_ROUNDS
#define WASMOS_TEST_ROUNDS 1000
#endif

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

/* ------------------------------------------------------------------ stubs */

#define POOL_MAX (NTHREADS + 4)
static thread_t g_pool[POOL_MAX];
static _Thread_local uint32_t g_current_tid;

uint64_t timer_ticks(void) {
    return 0;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

thread_t* thread_table_at(uint32_t index) {
    return (index < POOL_MAX) ? &g_pool[index] : 0;
}

thread_t* thread_get(uint32_t tid) {
    if (tid == 0 || tid > POOL_MAX) {
        return 0;
    }
    return &g_pool[tid - 1u];
}

uint32_t thread_current_tid(void) {
    return g_current_tid;
}

void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        __atomic_store_n((uint32_t*)&t->state, (uint32_t)state, __ATOMIC_RELEASE);
        t->block_reason = reason;
    }
}

/* Atomic so the sanitizer arms report races in ipc.c rather than in the stub:
 * a waker on one host thread stores here while the owning thread loads. */
void sched_wake_thread(thread_t* t) {
    if (t) {
        __atomic_store_n((uint32_t*)&t->state, (uint32_t)THREAD_STATE_READY, __ATOMIC_RELEASE);
    }
}

void process_yield(process_run_result_t result) {
    (void)result;
    thread_t* self = thread_get(g_current_tid);
    /* Model an immediate resume. A real resume always follows an unlink, so
     * unlink here too — revalidating under the event lock exactly the way
     * sched_timeout_fire does, since a genuine waker may be detaching us
     * concurrently. */
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
 * and spinning keeps the threads hot, which is what we want for interleaving. */
typedef struct {
    int count;
    int target;
} spin_barrier_t;
static spin_barrier_t g_spin_barrier;

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

static int count_of_is_zero_impl(uint32_t ep) {
    uint32_t n = 0xFFFFFFFFu;
    return ipc_endpoint_count(ep, &n) == IPC_OK && n == 0;
}

static uint32_t g_next_ctx = 1000u;
static uint32_t fresh_ctx(void) {
    return __atomic_fetch_add(&g_next_ctx, 1u, __ATOMIC_RELAXED);
}

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
        if (__atomic_exchange_n(&g_seen[m.arg0 * PER_SENDER + m.arg1], (uint8_t)1,
                                __ATOMIC_ACQ_REL) != 0) {
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

static void* creator_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    uint32_t* ids = (uint32_t*)a->aux;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (int i = 0; i < CREATE_PER_THREAD; ++i) {
        uint32_t id = 0;
        int rc = (i & 1) ? ipc_notification_create(a->ctx, &id) : ipc_endpoint_create(a->ctx, &id);
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
    uint32_t ctx = fresh_ctx();
    worker_arg_t args[POOL_MAX];
    uint32_t* ids = (uint32_t*)calloc((size_t)NTHREADS * CREATE_PER_THREAD, sizeof(uint32_t));
    memset(args, 0, sizeof(args));
    for (int i = 0; i < NTHREADS; ++i) {
        args[i].ctx = ctx;
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
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------- teardown racing senders and select */

/* This is the shape that used to corrupt memory: ipc_send_from read
 * ep->poll_struct under ep->lock and then notified after releasing it, while
 * ipc_endpoints_release_owner took the same lock and freed that poll_struct.
 * The window is only reachable when the endpoint actually has a watcher, so
 * the test keeps a select set registered throughout. Runs clean only under
 * ASan; on a plain build a use-after-free here is usually silent. */
typedef struct {
    uint32_t endpoint;
    uint32_t ctx;
    int stop;
} teardown_shared_t;

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
        if (rc != IPC_OK && rc != IPC_ERR_FULL && rc != IPC_ERR_INVALID) {
            a->result = rc; /* the only legal outcomes while a teardown races */
            return 0;
        }
    }
    return 0;
}

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
    CHECK(bad == 0, "a send racing endpoint teardown returns OK, FULL or INVALID and nothing else");
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------- select under contention */

/* The service-side receive loop, written the way the contract requires: the
 * readiness latch is a single slot, so concurrent signals collapse and a
 * consumer that trusted select_recv alone would strand messages. On EMPTY it
 * re-polls every watched endpoint — which is also what makes the completion
 * condition here reachable rather than a hang. */
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

static void* select_churn_thread(void* p) {
    worker_arg_t* a = (worker_arg_t*)p;
    g_current_tid = a->tid;
    BARRIER_WAIT();
    for (uint32_t i = 0; i < a->rounds; ++i) {
        uint32_t sel = 0;
        int rc = ipc_select_create(a->ctx, &sel);
        if (rc == IPC_ERR_FULL) {
            sched_yield();
            continue; /* legitimate: the table is shared and finite */
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

    /* The table must be completely free again. */
    uint32_t ids[64];
    uint32_t n = 0;
    while (n < 64) {
        uint32_t sel = 0;
        if (ipc_select_create(ctx, &sel) != IPC_OK) {
            break;
        }
        ids[n++] = sel;
    }
    CHECK(n == 32u, "every slot the churn allocated was returned");
    for (uint32_t i = 0; i < n; ++i) {
        ipc_select_destroy(ids[i], ctx);
    }
    ipc_endpoints_release_owner(ctx);
}

/* -------------------------------------------------------------------- main */

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
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[i].name);
        }
    }
    printf("test_ipc_concurrency: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}