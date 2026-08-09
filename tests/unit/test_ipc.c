/* test_ipc.c — host tests for the REAL kernel IPC layer (src/kernel/ipc.c).
 *
 * ipc.c had no host coverage: everything it does was only ever exercised
 * end-to-end through QEMU, where a transport bug shows up as a boot hang three
 * subsystems away. This links the real ipc.c together with the real poll.c and
 * the real sched_event.c, and stubs only what those reach outside themselves:
 * the timer, the thread table, and the two scheduler entry points.
 *
 * MODELLING NOTE (blocking). In the kernel, process_yield(PROCESS_RUN_BLOCKED)
 * does not return until a waker resumes the thread, so everything after it in
 * sched_event_wait runs post-wake. A host stub cannot suspend, so process_yield
 * returns immediately and the caller sees a "spurious wake" — which is exactly
 * the contract ipc_recv_blocking_for / ipc_select_wait document (return
 * IPC_EMPTY, caller retries). To test the delivered-while-blocked path, a test
 * installs a yield hook: it runs at the point where the thread is parked in the
 * wait list with no IPC lock held, which is precisely where a real waker on
 * another CPU would run. So the wake really goes through sched_event_wake_one
 * and the real wait-list handoff, not a fabricated one.
 *
 * Timeout behaviour is not re-tested here: sched_event.c owns the deadline
 * machinery and test_sched_event.c covers it. This file asserts only that IPC
 * hands the timeout through and that the untimed paths behave.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"
#include "poll.h"
#include "sched.h"
#include "sched_event.h"
#include "thread.h"

/* poll.c allocates its watcher nodes, and kmem/list_alloc grow the endpoint
 * table, through malloc. Defining it here routes both through a switch so the
 * allocation-failure paths -- otherwise unreachable -- can be driven. free is
 * deliberately NOT defined: aligned_alloc memory is freeable by the host free,
 * so the substitution stays one-sided and minimal. */
static int g_malloc_fail;

void* malloc(size_t n) {
    if (g_malloc_fail) {
        return 0;
    }
    return aligned_alloc(16u, (n + 15u) & ~(size_t)15u);
}

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ stubs */

#define POOL_MAX 8
static thread_t g_pool[POOL_MAX];
static uint32_t g_current_tid;
static uint64_t g_now;
static int g_yield_calls;
static int g_wake_calls;

static thread_t* g_last_woken;

/* Runs inside process_yield, i.e. with the caller parked in the wait list and
 * no IPC lock held — where a waker on another CPU would run. Fires once. */
static void (*g_yield_hook)(void);

/* When set, process_yield leaves the caller parked instead of modelling the
 * spurious wake. That is the state a real blocked thread is in between the
 * yield and its waker, and it is the only way a single host thread can build up
 * several simultaneous waiters on one endpoint. */
static int g_park;

uint64_t timer_ticks(void) {
    return g_now;
}

uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

thread_t* thread_table_at(uint32_t index) {
    return (index < POOL_MAX) ? &g_pool[index] : 0;
}

thread_t* thread_get(uint32_t tid) {
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        if (g_pool[i].tid == tid) {
            return &g_pool[i];
        }
    }
    return 0;
}

uint32_t thread_current_tid(void) {
    return g_current_tid;
}

void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        t->state = state;
        t->block_reason = reason;
    }
}

void sched_wake_thread(thread_t* t) {
    g_wake_calls++;
    g_last_woken = t;
    if (t) {
        t->state = THREAD_STATE_READY;
    }
}

void process_yield(process_run_result_t result) {
    (void)result;
    g_yield_calls++;
    thread_t* self = thread_get(g_current_tid);
    if (g_yield_hook) {
        void (*hook)(void) = g_yield_hook;
        g_yield_hook = 0; /* once */
        hook();
    }
    if (g_park) {
        return; /* stay in the wait list; see g_park */
    }
    /* If nothing woke us, model the spurious wake: every real resume path
     * (waker, timeout, abort) unlinks the thread before it runs again, so
     * leaving the node linked here would corrupt the next block. */
    if (self) {
        sched_event_t* ev = self->wait_event;
        if (ev) {
            ksync_spinlock_lock(&ev->lock);
            if (self->wait_event == ev) {
                list_head_del(&self->event_node);
                self->wait_event = 0;
            }
            ksync_spinlock_unlock(&ev->lock);
        }
        self->state = THREAD_STATE_RUNNING;
    }
}

/* --------------------------------------------------------------- fixtures */

/* Context ids. IPC_CONTEXT_KERNEL (0) is privileged, so every ordinary actor
 * needs a non-zero one. */
#define CTX_A 11u
#define CTX_B 22u

static uint32_t g_next_ctx = 100u;

/* A fresh owner context per test: ipc_init cannot be called twice (it re-inits
 * the endpoint list and leaks the old backing store), so tests isolate
 * themselves by owner and release what they created. */
static uint32_t fresh_ctx(void) {
    return g_next_ctx++;
}

static void reset_threads(void) {
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        g_pool[i].tid = i + 1u;
        g_pool[i].state = THREAD_STATE_RUNNING;
        list_head_init(&g_pool[i].event_node);
        list_head_init(&g_pool[i].sched_node);
    }
    g_current_tid = 1;
    g_now = 100;
    g_yield_calls = 0;
    g_wake_calls = 0;
    g_yield_hook = 0;
    g_last_woken = 0;
    g_park = 0;
    g_malloc_fail = 0;
}

/* Park `t` in `ep`'s wait list through the real blocking path and leave it
 * there. Returns with the thread genuinely linked into ep->event.wait_list by
 * sched_event_wait, so a later send/abort exercises the real handoff. */
static void park_recv(uint32_t ctx, uint32_t ep, thread_t* t) {
    ipc_message_t scratch;
    uint32_t saved = g_current_tid;
    g_current_tid = t->tid;
    g_park = 1;
    (void)ipc_recv_blocking_for(ctx, ep, &scratch);
    g_park = 0;
    t->state = THREAD_STATE_BLOCKED; /* the scheduler would leave it blocked */
    g_current_tid = saved;
}

static int waiters_on(uint32_t ep_unused, thread_t** ts, int n) {
    (void)ep_unused;
    int parked = 0;
    for (int i = 0; i < n; ++i) {
        if (ts[i]->wait_event && !list_head_empty(&ts[i]->event_node)) {
            parked++;
        }
    }
    return parked;
}

static thread_t* self_thread(void) {
    return thread_get(g_current_tid);
}

static ipc_message_t msg_of(uint32_t source, uint32_t type, uint32_t a0) {
    ipc_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.source = source;
    m.destination = 0xDEADBEEFu; /* must be overwritten by send */
    m.request_id = a0 ^ 0x5A5Au;
    m.arg0 = a0;
    m.arg1 = a0 + 1u;
    m.arg2 = a0 + 2u;
    m.arg3 = a0 + 3u;
    return m;
}

static uint32_t count_of(uint32_t ep) {
    uint32_t n = 0xFFFFFFFFu;
    if (ipc_endpoint_count(ep, &n) != IPC_OK) {
        return 0xFFFFFFFFu;
    }
    return n;
}

/* Send as the kernel, which skips the source-ownership check. */
static int ksend(uint32_t ep, uint32_t a0) {
    ipc_message_t m = msg_of(IPC_ENDPOINT_NONE, 7u, a0);
    return ipc_send(ep, &m);
}

/* --------------------------------------------------- endpoint lifecycle */

static void test_create_assigns_distinct_ids(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t a = 0, b = 0, c = 0;
    CHECK(ipc_endpoint_create(ctx, &a) == IPC_OK, "create succeeds");
    CHECK(ipc_endpoint_create(ctx, &b) == IPC_OK, "create succeeds");
    CHECK(ipc_notification_create(ctx, &c) == IPC_OK, "notification create succeeds");
    CHECK(a != 0 && b != 0 && c != 0, "no endpoint is id 0");
    CHECK(a != IPC_ENDPOINT_NONE && b != IPC_ENDPOINT_NONE && c != IPC_ENDPOINT_NONE,
          "no endpoint is the reserved NONE id");
    CHECK(a != b && b != c && a != c, "ids are distinct");
    CHECK(a < b && b < c, "ids are handed out monotonically");
    CHECK(count_of(a) == 0, "a fresh endpoint is empty");

    uint32_t owner = 0;
    CHECK(ipc_endpoint_owner(a, &owner) == IPC_OK && owner == ctx, "owner is the creating context");
    CHECK(ipc_endpoint_owner(c, &owner) == IPC_OK && owner == ctx,
          "notification owner is the creating context");

    ipc_endpoints_release_owner(ctx);
}

static void test_create_rejects_a_null_out_param(void) {
    CHECK(ipc_endpoint_create(CTX_A, 0) == IPC_ERR_INVALID, "create(NULL) is INVALID");
    CHECK(ipc_notification_create(CTX_A, 0) == IPC_ERR_INVALID,
          "notification_create(NULL) is INVALID");
}

static void test_reserved_and_unknown_ids_are_invalid(void) {
    uint32_t out = 0;
    ipc_message_t m;
    memset(&m, 0, sizeof(m));

    CHECK(ipc_endpoint_owner(0, &out) == IPC_ERR_INVALID, "endpoint id 0 is not a handle");
    CHECK(ipc_endpoint_owner(IPC_ENDPOINT_NONE, &out) == IPC_ERR_INVALID,
          "the NONE sentinel is not a handle");
    CHECK(ipc_endpoint_owner(0x7FFFFFFFu, &out) == IPC_ERR_INVALID, "an unknown id is INVALID");
    CHECK(ipc_endpoint_count(0x7FFFFFFFu, &out) == IPC_ERR_INVALID, "count of an unknown id");
    CHECK(ipc_send(0x7FFFFFFFu, &m) == IPC_ERR_INVALID, "send to an unknown id");
    CHECK(ipc_recv(0x7FFFFFFFu, &m) == IPC_ERR_INVALID, "recv from an unknown id");
    CHECK(ipc_notify(0x7FFFFFFFu) == IPC_ERR_INVALID, "notify to an unknown id");
    CHECK(ipc_wait(0x7FFFFFFFu) == IPC_ERR_INVALID, "wait on an unknown id");
}

static void test_query_rejects_null_out_params(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    /* Both take ep->lock before the NULL check, so this also pins that the
     * lock is released on the rejection path — a leaked lock would deadlock
     * the very next lookup below. */
    CHECK(ipc_endpoint_owner(ep, 0) == IPC_ERR_INVALID, "owner(NULL) is INVALID");
    CHECK(ipc_endpoint_count(ep, 0) == IPC_ERR_INVALID, "count(NULL) is INVALID");
    CHECK(count_of(ep) == 0, "the endpoint is still usable after a rejected query");
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------------- queue semantics */

static void test_queue_is_fifo_across_ring_wrap(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);

    /* Deliberately not a multiple of the depth: the head/tail modulo must be
     * exercised at an offset, not just from index 0. */
    uint32_t next_send = 0;
    uint32_t next_recv = 0;
    int order_ok = 1;
    int payload_ok = 1;
    for (int round = 0; round < 7; ++round) {
        for (int i = 0; i < 5; ++i) {
            if (ksend(ep, next_send) != IPC_OK) {
                order_ok = 0;
            }
            next_send++;
        }
        for (int i = 0; i < 3; ++i) {
            ipc_message_t got;
            memset(&got, 0, sizeof(got));
            if (ipc_recv(ep, &got) != IPC_OK) {
                order_ok = 0;
                continue;
            }
            if (got.arg0 != next_recv) {
                order_ok = 0;
            }
            if (got.arg1 != next_recv + 1u || got.arg2 != next_recv + 2u ||
                got.arg3 != next_recv + 3u || got.request_id != (next_recv ^ 0x5A5Au) ||
                got.type != 7u) {
                payload_ok = 0;
            }
            next_recv++;
        }
    }
    CHECK(order_ok, "every send lands and comes back in FIFO order across the wrap");
    CHECK(payload_ok, "the whole message body survives the queue");
    CHECK(count_of(ep) == next_send - next_recv, "count tracks the outstanding messages");

    /* Drain and confirm the tail of the sequence is still in order. */
    int drain_ok = 1;
    while (next_recv < next_send) {
        ipc_message_t got;
        memset(&got, 0, sizeof(got));
        if (ipc_recv(ep, &got) != IPC_OK || got.arg0 != next_recv) {
            drain_ok = 0;
            break;
        }
        next_recv++;
    }
    CHECK(drain_ok, "the drain stays in FIFO order");
    CHECK(count_of(ep) == 0, "a fully drained endpoint reports zero");
    CHECK(ipc_recv(ep, &(ipc_message_t){0}) == IPC_EMPTY, "recv on an empty endpoint is EMPTY");

    ipc_endpoints_release_owner(ctx);
}

static void test_queue_full_is_reported_and_recovers(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);

    int fill_ok = 1;
    for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; ++i) {
        if (ksend(ep, i) != IPC_OK) {
            fill_ok = 0;
        }
    }
    CHECK(fill_ok, "the queue accepts exactly IPC_QUEUE_DEPTH messages");
    CHECK(count_of(ep) == IPC_QUEUE_DEPTH, "a full queue reports the full depth");
    CHECK(ksend(ep, 999u) == IPC_ERR_FULL, "the overflowing send is refused with FULL");
    CHECK(count_of(ep) == IPC_QUEUE_DEPTH, "a refused send does not change the count");

    ipc_message_t got;
    CHECK(ipc_recv(ep, &got) == IPC_OK && got.arg0 == 0, "the head is still the first message");
    CHECK(ksend(ep, 999u) == IPC_OK, "freeing a slot re-opens the queue");

    /* The refused message must not have displaced anything: the remaining
     * sequence is 1..DEPTH-1 followed by the one accepted after the drain. */
    int seq_ok = 1;
    for (uint32_t i = 1; i < IPC_QUEUE_DEPTH; ++i) {
        ipc_message_t m;
        if (ipc_recv(ep, &m) != IPC_OK || m.arg0 != i) {
            seq_ok = 0;
        }
    }
    ipc_message_t last;
    if (ipc_recv(ep, &last) != IPC_OK || last.arg0 != 999u) {
        seq_ok = 0;
    }
    CHECK(seq_ok, "a FULL rejection leaves the queued sequence untouched");

    ipc_endpoints_release_owner(ctx);
}

static void test_send_stamps_the_destination(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    ipc_message_t m = msg_of(IPC_ENDPOINT_NONE, 1u, 5u);
    CHECK(m.destination != ep, "the caller's destination field starts wrong");
    CHECK(ipc_send(ep, &m) == IPC_OK, "send succeeds");
    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    CHECK(ipc_recv(ep, &got) == IPC_OK, "recv succeeds");
    CHECK(got.destination == ep, "send overwrites destination with the target endpoint");
    CHECK(m.destination != ep, "send does not write through the caller's const message");
    ipc_endpoints_release_owner(ctx);
}

static void test_send_and_recv_reject_null_payloads(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    CHECK(ipc_send(ep, 0) == IPC_ERR_INVALID, "send(NULL) is INVALID");
    CHECK(ksend(ep, 42u) == IPC_OK, "a message is queued");
    CHECK(ipc_recv(ep, 0) == IPC_ERR_INVALID, "recv(NULL) is INVALID");
    CHECK(count_of(ep) == 1, "a rejected recv does not consume the message");
    ipc_message_t got;
    CHECK(ipc_recv(ep, &got) == IPC_OK && got.arg0 == 42u, "the message is still deliverable");
    ipc_endpoints_release_owner(ctx);
}

/* ----------------------------------------------------------- permissions */

static void test_send_requires_a_source_the_sender_owns(void) {
    uint32_t ep_a = 0, ep_b = 0, target = 0;
    (void)ipc_endpoint_create(CTX_A, &ep_a);
    (void)ipc_endpoint_create(CTX_B, &ep_b);
    (void)ipc_endpoint_create(CTX_B, &target);

    ipc_message_t m = msg_of(ep_a, 1u, 1u);
    CHECK(ipc_send_from(CTX_A, target, &m) == IPC_OK, "a sender may send with its own source");

    m.source = ep_b;
    CHECK(ipc_send_from(CTX_A, target, &m) == IPC_ERR_PERM,
          "a sender may not forge another context's source");

    m.source = IPC_ENDPOINT_NONE;
    CHECK(ipc_send_from(CTX_A, target, &m) == IPC_ERR_PERM,
          "a non-kernel sender may not omit its source");

    m.source = 0x7FFFFFFFu;
    CHECK(ipc_send_from(CTX_A, target, &m) == IPC_ERR_PERM, "an unknown source endpoint is PERM");

    m.source = IPC_ENDPOINT_NONE;
    CHECK(ipc_send_from(IPC_CONTEXT_KERNEL, target, &m) == IPC_OK,
          "the kernel context bypasses the source check");

    /* The permission check must run before the destination lookup, so a forged
     * source is refused even when the destination does not exist. */
    m.source = ep_b;
    CHECK(ipc_send_from(CTX_A, 0x7FFFFFFFu, &m) == IPC_ERR_PERM,
          "the source check precedes the destination lookup");

    CHECK(count_of(target) == 2, "exactly the two permitted sends landed");

    ipc_endpoints_release_owner(CTX_A);
    ipc_endpoints_release_owner(CTX_B);
}

static void test_recv_requires_ownership(void) {
    uint32_t ep = 0;
    (void)ipc_endpoint_create(CTX_A, &ep);
    CHECK(ksend(ep, 1u) == IPC_OK, "a message is queued");

    ipc_message_t got;
    CHECK(ipc_recv_for(CTX_B, ep, &got) == IPC_ERR_PERM, "a non-owner may not receive");
    CHECK(count_of(ep) == 1, "a refused receive does not consume the message");
    CHECK(ipc_recv_for(IPC_CONTEXT_KERNEL, ep, &got) == IPC_OK, "the kernel may receive anywhere");

    CHECK(ksend(ep, 2u) == IPC_OK, "another message is queued");
    CHECK(ipc_recv_for(CTX_A, ep, &got) == IPC_OK && got.arg0 == 2u, "the owner receives");

    ipc_endpoints_release_owner(CTX_A);
}

/* -------------------------------------------------- endpoint type safety */

static void test_message_and_notification_endpoints_do_not_mix(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t msg_ep = 0, note_ep = 0;
    (void)ipc_endpoint_create(ctx, &msg_ep);
    (void)ipc_notification_create(ctx, &note_ep);

    ipc_message_t m = msg_of(IPC_ENDPOINT_NONE, 1u, 1u);
    ipc_message_t got;

    CHECK(ipc_send(note_ep, &m) == IPC_ERR_INVALID, "send to a notification endpoint is INVALID");
    CHECK(ipc_recv(note_ep, &got) == IPC_ERR_INVALID,
          "recv from a notification endpoint is INVALID");
    CHECK(ipc_recv_blocking_for(ctx, note_ep, &got) == IPC_ERR_INVALID,
          "blocking recv on a notification endpoint is INVALID");
    CHECK(ipc_endpoint_wait_for(ctx, note_ep, 0) == IPC_ERR_INVALID,
          "endpoint_wait on a notification endpoint is INVALID");

    CHECK(ipc_notify(msg_ep) == IPC_ERR_INVALID, "notify on a message endpoint is INVALID");
    CHECK(ipc_wait(msg_ep) == IPC_ERR_INVALID, "wait on a message endpoint is INVALID");

    /* Neither endpoint was disturbed by the rejected cross-type calls. */
    CHECK(count_of(msg_ep) == 0, "the message endpoint is untouched");
    CHECK(ipc_wait(note_ep) == IPC_EMPTY, "the notification endpoint is untouched");

    ipc_endpoints_release_owner(ctx);
}

/* ---------------------------------------------------------- notifications */

static void test_notifications_count_up_and_down(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_notification_create(ctx, &ep);

    CHECK(ipc_wait_for(ctx, ep) == IPC_EMPTY, "an unsignalled notification waits empty");
    CHECK(ipc_notify_from(ctx, ep) == IPC_OK, "the owner may notify");
    CHECK(ipc_notify_from(IPC_CONTEXT_KERNEL, ep) == IPC_OK, "the kernel may notify");
    CHECK(ipc_notify_from(CTX_B, ep) == IPC_ERR_PERM, "a foreign context may not notify");

    CHECK(ipc_wait_for(CTX_B, ep) == IPC_ERR_PERM, "a foreign context may not consume");
    CHECK(ipc_wait_for(ctx, ep) == IPC_OK, "the first signal is consumed");
    CHECK(ipc_wait_for(ctx, ep) == IPC_OK, "the second signal is consumed");
    CHECK(ipc_wait_for(ctx, ep) == IPC_EMPTY, "the counter does not go negative");
    CHECK(ipc_wait_for(IPC_CONTEXT_KERNEL, ep) == IPC_EMPTY, "and stays empty for the kernel too");

    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------ non-blocking polls stay quiet */

/* Regression tripwire. A non-blocking poll that registers a waiter lets a later
 * sender "wake" a thread that never blocked, pushing a RUNNING thread back to
 * READY on another CPU. Both poll paths carry a comment saying they must not do
 * this; nothing checked it. */
static void test_non_blocking_polls_do_not_arm_a_waiter(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t msg_ep = 0, note_ep = 0;
    (void)ipc_endpoint_create(ctx, &msg_ep);
    (void)ipc_notification_create(ctx, &note_ep);
    reset_threads();
    thread_t* self = self_thread();

    ipc_message_t got;
    CHECK(ipc_recv_for(ctx, msg_ep, &got) == IPC_EMPTY, "the poll finds nothing");
    CHECK(self->wait_event == 0, "recv_for did not park the caller on an event");
    CHECK(list_head_empty(&self->event_node), "recv_for did not link the caller into a wait list");
    CHECK(self->state == THREAD_STATE_RUNNING, "recv_for left the caller runnable");
    CHECK(g_yield_calls == 0, "recv_for did not yield");

    CHECK(ipc_wait_for(ctx, note_ep) == IPC_EMPTY, "the notification poll finds nothing");
    CHECK(self->wait_event == 0, "wait_for did not park the caller on an event");
    CHECK(list_head_empty(&self->event_node), "wait_for did not link the caller into a wait list");
    CHECK(g_yield_calls == 0, "wait_for did not yield");

    /* And a later send must therefore wake nobody. */
    CHECK(ksend(msg_ep, 1u) == IPC_OK, "a message arrives afterwards");
    CHECK(g_wake_calls == 0, "the send woke no thread, because none was parked");

    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------------- blocking receive */

static uint32_t g_hook_ep;
static uint32_t g_hook_arg0;

static void hook_send(void) {
    (void)ksend(g_hook_ep, g_hook_arg0);
}

static void test_blocking_recv_takes_a_ready_message_without_blocking(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    CHECK(ksend(ep, 77u) == IPC_OK, "a message is already queued");

    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    CHECK(ipc_recv_blocking_for(ctx, ep, &got) == IPC_OK, "the ready message is returned");
    CHECK(got.arg0 == 77u, "and it is the right one");
    CHECK(g_yield_calls == 0, "a ready message never blocks");
    CHECK(count_of(ep) == 0, "the message was consumed");

    ipc_endpoints_release_owner(ctx);
}

static void test_blocking_recv_receives_a_message_sent_while_parked(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* self = self_thread();

    g_hook_ep = ep;
    g_hook_arg0 = 88u;
    g_yield_hook = hook_send;

    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    CHECK(ipc_recv_blocking_for(ctx, ep, &got) == IPC_OK, "the parked receiver is served");
    CHECK(got.arg0 == 88u, "with the message the waker sent");
    CHECK(g_yield_calls == 1, "it blocked exactly once");
    CHECK(g_wake_calls == 1, "the send woke the parked receiver");
    CHECK(self->pend_state == SCHED_PEND_OK, "the wake carried a normal pend state");
    CHECK(self->wait_event == 0, "the waker unlinked it from the event");
    CHECK(list_head_empty(&self->event_node), "and from the wait list");
    CHECK(count_of(ep) == 0, "the message was consumed by the woken receiver");

    ipc_endpoints_release_owner(ctx);
}

static void test_blocking_recv_reports_a_spurious_wake_as_empty(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();

    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    CHECK(ipc_recv_blocking_for(ctx, ep, &got) == IPC_EMPTY,
          "waking with nothing queued reports EMPTY, not a stale message");
    CHECK(g_yield_calls == 1, "it did block");
    CHECK(list_head_empty(&self_thread()->event_node), "the caller is off the wait list");

    /* The documented contract is "retry", so a retry must still work. */
    CHECK(ksend(ep, 5u) == IPC_OK, "a message arrives");
    CHECK(ipc_recv_blocking_for(ctx, ep, &got) == IPC_OK && got.arg0 == 5u,
          "the retry after a spurious wake succeeds");

    ipc_endpoints_release_owner(ctx);
}

static void test_blocking_recv_enforces_ownership_and_arguments(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    ipc_message_t got;

    CHECK(ipc_recv_blocking_for(ctx, ep, 0) == IPC_ERR_INVALID, "a NULL out message is INVALID");
    CHECK(ipc_recv_blocking_for(ctx, 0x7FFFFFFFu, &got) == IPC_ERR_INVALID,
          "an unknown endpoint is INVALID");
    CHECK(ipc_recv_blocking_for(CTX_B, ep, &got) == IPC_ERR_PERM, "a non-owner may not block here");
    CHECK(g_yield_calls == 0, "none of the rejections blocked");

    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------ readable-without-consume */

static void test_endpoint_wait_does_not_consume_the_message(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    CHECK(ksend(ep, 3u) == IPC_OK, "a message is queued");

    CHECK(ipc_endpoint_wait_for(ctx, ep, 0) == IPC_OK, "an already-readable endpoint returns OK");
    CHECK(g_yield_calls == 0, "and does not block");
    CHECK(count_of(ep) == 1, "and does not dequeue");

    ipc_message_t got;
    CHECK(ipc_recv_for(ctx, ep, &got) == IPC_OK && got.arg0 == 3u,
          "the caller drains it itself afterwards");

    /* Now the blocking arm: parked, then a sender makes it readable. */
    g_hook_ep = ep;
    g_hook_arg0 = 4u;
    g_yield_hook = hook_send;
    CHECK(ipc_endpoint_wait_for(ctx, ep, 0) == IPC_OK, "the parked waiter is woken");
    CHECK(g_yield_calls == 1, "it blocked once");
    CHECK(count_of(ep) == 1, "the wake still leaves the message for the caller to drain");

    ipc_endpoints_release_owner(ctx);
}

static void test_endpoint_wait_enforces_ownership(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    CHECK(ipc_endpoint_wait_for(CTX_B, ep, 0) == IPC_ERR_PERM, "a non-owner may not wait");
    CHECK(ipc_endpoint_wait_for(ctx, 0x7FFFFFFFu, 0) == IPC_ERR_INVALID,
          "an unknown endpoint is INVALID");
    CHECK(g_yield_calls == 0, "neither rejection blocked");
    ipc_endpoints_release_owner(ctx);
}

/* ----------------------------------------------------- owner teardown */

static void test_release_owner_frees_only_that_owners_endpoints(void) {
    uint32_t keep = fresh_ctx();
    uint32_t drop = fresh_ctx();
    uint32_t k1 = 0, k2 = 0, d1 = 0, d2 = 0, d3 = 0;
    (void)ipc_endpoint_create(keep, &k1);
    (void)ipc_notification_create(keep, &k2);
    (void)ipc_endpoint_create(drop, &d1);
    (void)ipc_endpoint_create(drop, &d2);
    (void)ipc_notification_create(drop, &d3);
    CHECK(ksend(d1, 1u) == IPC_OK, "the doomed endpoint has traffic queued");
    CHECK(ksend(k1, 1u) == IPC_OK, "so does the surviving one");

    ipc_endpoints_release_owner(drop);

    uint32_t out = 0;
    CHECK(ipc_endpoint_owner(d1, &out) == IPC_ERR_INVALID, "a released endpoint is gone");
    CHECK(ipc_endpoint_owner(d2, &out) == IPC_ERR_INVALID, "every one of them is gone");
    CHECK(ipc_endpoint_owner(d3, &out) == IPC_ERR_INVALID, "notifications included");
    CHECK(ipc_endpoint_owner(k1, &out) == IPC_OK && out == keep, "another owner is untouched");
    CHECK(count_of(k1) == 1, "and keeps its queued traffic");
    CHECK(ipc_wait_for(keep, k2) == IPC_EMPTY, "its notification endpoint still answers");

    ipc_endpoints_release_owner(keep);
}

static void test_release_owner_ignores_the_kernel_context(void) {
    uint32_t ep = 0;
    (void)ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ep);
    ipc_endpoints_release_owner(IPC_CONTEXT_KERNEL);
    uint32_t out = 0xFFu;
    CHECK(ipc_endpoint_owner(ep, &out) == IPC_OK && out == IPC_CONTEXT_KERNEL,
          "release_owner(0) is a no-op, so it cannot sweep the whole table");
    /* No way to release it again; it stays for the rest of the run by design. */
}

static uint32_t g_release_ctx;

static void hook_release_owner(void) {
    ipc_endpoints_release_owner(g_release_ctx);
}

static void test_release_owner_aborts_a_blocked_receiver(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* self = self_thread();

    g_release_ctx = ctx;
    g_yield_hook = hook_release_owner;

    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    int rc = ipc_recv_blocking_for(ctx, ep, &got);
    CHECK(rc == IPC_ERR_INVALID, "a receiver blocked on a destroyed endpoint returns INVALID");
    CHECK(g_wake_calls == 1, "the teardown woke it");
    CHECK(self->pend_state == SCHED_PEND_ABORT, "with an ABORT pend state");
    CHECK(list_head_empty(&self->event_node), "and unlinked it from the wait list");
    CHECK(self->wait_event == 0, "and cleared its event pointer");
}

/* --------------------------------------------------------- select sets */

static void test_select_create_add_and_destroy(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);

    CHECK(ipc_select_create(ctx, 0) == IPC_ERR_INVALID, "create(NULL) is INVALID");
    CHECK(ipc_select_create(ctx, &sel) == IPC_OK, "create succeeds");
    CHECK(sel != 0, "select ids are never 0");
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "add succeeds");

    /* Adding an endpoint that does not exist is accepted into the set — it just
     * never becomes ready. Pinning this so a future change is a deliberate one. */
    CHECK(ipc_select_add(sel, 0x7FFFFFFFu, ctx) == IPC_OK,
          "an unknown endpoint is recorded but silently never signals");

    ipc_select_destroy(sel, ctx);
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_ERR_INVALID, "a destroyed set rejects add");
    ipc_endpoints_release_owner(ctx);
}

static void test_select_rejects_bad_ids_and_foreign_owners(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0, ready = 0;
    ipc_message_t msg;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_create(ctx, &sel);

    CHECK(ipc_select_add(0, ep, ctx) == IPC_ERR_INVALID, "select id 0 is not a handle");
    CHECK(ipc_select_add(0xFFFFu, ep, ctx) == IPC_ERR_INVALID, "an out-of-range select id");
    CHECK(ipc_select_add(sel, ep, CTX_B) == IPC_ERR_INVALID, "a foreign owner may not add");
    CHECK(ipc_select_wait(sel, CTX_B, &ready, 0) == IPC_ERR_INVALID,
          "a foreign owner may not wait");
    CHECK(ipc_select_wait(sel, ctx, 0, 0) == IPC_ERR_INVALID, "wait(NULL) is INVALID");
    CHECK(ipc_select_recv(sel, ctx, &ready, 0, 0) == IPC_ERR_INVALID, "recv(NULL) is INVALID");
    CHECK(ipc_select_recv(0xFFFFu, ctx, &ready, &msg, 0) == IPC_ERR_INVALID,
          "recv on an out-of-range select id");

    /* A foreign destroy must not tear down someone else's set. */
    ipc_select_destroy(sel, CTX_B);
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "the set survived the foreign destroy");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

static void test_select_watch_capacity_is_enforced(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0;
    uint32_t eps[IPC_SELECT_EPS_MAX + 1u];
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX + 1u; ++i) {
        eps[i] = 0;
        (void)ipc_endpoint_create(ctx, &eps[i]);
    }
    (void)ipc_select_create(ctx, &sel);

    int add_ok = 1;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX; ++i) {
        if (ipc_select_add(sel, eps[i], ctx) != IPC_OK) {
            add_ok = 0;
        }
    }
    CHECK(add_ok, "a set accepts IPC_SELECT_EPS_MAX endpoints");
    CHECK(ipc_select_add(sel, eps[IPC_SELECT_EPS_MAX], ctx) == IPC_ERR_FULL,
          "the next one is refused with FULL");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* The select table is a fixed array. Exhausting it must report FULL, and every
 * slot must come back after the sets are destroyed — a leaked slot would
 * silently cap how many services can ever run. */
static void test_select_table_exhausts_and_recovers(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ids[64];
    uint32_t n = 0;
    while (n < 64) {
        uint32_t sel = 0;
        if (ipc_select_create(ctx, &sel) != IPC_OK) {
            break;
        }
        ids[n++] = sel;
    }
    CHECK(n > 0, "at least one set can be created");
    uint32_t overflow = 0;
    CHECK(ipc_select_create(ctx, &overflow) == IPC_ERR_FULL, "an exhausted table reports FULL");

    for (uint32_t i = 0; i < n; ++i) {
        ipc_select_destroy(ids[i], ctx);
    }
    uint32_t again = 0;
    CHECK(ipc_select_create(ctx, &again) == IPC_OK, "destroying the sets returns the slots");
    ipc_select_destroy(again, ctx);
}

static void test_select_listen_validates_and_builds(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0;
    uint32_t eps[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        (void)ipc_endpoint_create(ctx, &eps[i]);
    }

    CHECK(ipc_select_listen(ctx, 0, 3, &sel) == IPC_ERR_INVALID, "listen(NULL endpoints)");
    CHECK(ipc_select_listen(ctx, eps, 0, &sel) == IPC_ERR_INVALID, "listen(count 0)");
    CHECK(ipc_select_listen(ctx, eps, 3, 0) == IPC_ERR_INVALID, "listen(NULL out)");

    CHECK(ipc_select_listen(ctx, eps, 3, &sel) == IPC_OK, "listen builds the set");
    CHECK(ksend(eps[1], 9u) == IPC_OK, "a message arrives on the middle endpoint");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK, "the set reports readiness");
    CHECK(ready == eps[1], "and names the endpoint that actually got the message");
    ipc_select_destroy(sel, ctx);

    ipc_endpoints_release_owner(ctx);
}

/* listen() destroys the half-built set when an add fails. If it leaked the
 * slot instead, a service that retried would burn through the table. */
static void test_select_listen_does_not_leak_on_failure(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t too_many[IPC_SELECT_EPS_MAX + 1u];
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX + 1u; ++i) {
        too_many[i] = 0;
        (void)ipc_endpoint_create(ctx, &too_many[i]);
    }

    int leaked = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        uint32_t sel = 0xFFFFu;
        if (ipc_select_listen(ctx, too_many, IPC_SELECT_EPS_MAX + 1u, &sel) != IPC_ERR_FULL) {
            leaked = 1;
            break;
        }
    }
    CHECK(!leaked, "a failed listen never runs the table out of slots");

    /* Prove the table really is intact: it must still hold a full complement. */
    uint32_t probe = 0;
    CHECK(ipc_select_create(ctx, &probe) == IPC_OK, "the table still has free slots");
    ipc_select_destroy(probe, ctx);
    ipc_endpoints_release_owner(ctx);
}

static void test_select_is_signalled_by_a_send_to_a_watched_endpoint(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, a = 0, b = 0;
    (void)ipc_endpoint_create(ctx, &a);
    (void)ipc_endpoint_create(ctx, &b);
    (void)ipc_select_listen(ctx, (uint32_t[]){a, b}, 2, &sel);
    reset_threads();

    CHECK(ksend(b, 1u) == IPC_OK, "a message arrives on a watched endpoint");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK, "the pending readiness is returned");
    CHECK(ready == b, "and names the right endpoint");
    CHECK(g_yield_calls == 0, "a pre-signalled set does not block");

    /* The readiness latch is consumed: the next wait must not re-report it. */
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY,
          "the readiness latch is cleared by the wait that reported it");
    CHECK(g_yield_calls == 1, "so the second wait actually blocks");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

static void test_select_wakes_a_parked_waiter(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &sel);
    reset_threads();

    g_hook_ep = ep;
    g_hook_arg0 = 12u;
    g_yield_hook = hook_send;

    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK, "the parked waiter is signalled");
    CHECK(ready == ep, "with the endpoint that received the message");
    CHECK(g_yield_calls == 1, "it blocked once");
    CHECK(self_thread()->pend_state == SCHED_PEND_OK, "the wake carried a normal pend state");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

static void test_select_recv_delivers_and_reports_a_lost_race(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &sel);
    reset_threads();

    CHECK(ksend(ep, 21u) == IPC_OK, "a message arrives");
    uint32_t from = 0;
    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    CHECK(ipc_select_recv(sel, ctx, &from, &got, 0) == IPC_OK, "select_recv delivers");
    CHECK(from == ep && got.arg0 == 21u, "with the endpoint and the message");
    CHECK(count_of(ep) == 0, "and consumes it");

    /* Lost race: the set is signalled, but a competing receiver drains the
     * message before select_recv reaches ipc_recv_for. The caller must see
     * EMPTY and loop, not a stale or duplicated message. */
    CHECK(ksend(ep, 22u) == IPC_OK, "another message arrives and signals the set");
    ipc_message_t stolen;
    CHECK(ipc_recv_for(ctx, ep, &stolen) == IPC_OK && stolen.arg0 == 22u,
          "a competing receiver takes it first");
    CHECK(ipc_select_recv(sel, ctx, &from, &got, 0) == IPC_EMPTY,
          "select_recv reports EMPTY rather than inventing a message");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

static uint32_t g_destroy_sel;
static uint32_t g_destroy_ctx;

static void hook_destroy_select(void) {
    ipc_select_destroy(g_destroy_sel, g_destroy_ctx);
}

static void test_select_destroy_aborts_a_parked_waiter(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &sel);
    reset_threads();

    g_destroy_sel = sel;
    g_destroy_ctx = ctx;
    g_yield_hook = hook_destroy_select;

    uint32_t ready = 0xAAu;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY,
          "a waiter on a destroyed set is released with EMPTY, not left parked");
    CHECK(g_wake_calls == 1, "the teardown woke it");
    CHECK(self_thread()->pend_state == SCHED_PEND_ABORT, "with an ABORT pend state");
    CHECK(list_head_empty(&self_thread()->event_node), "and unlinked it");

    ipc_endpoints_release_owner(ctx);
}

/* Destroy must remove the set's watcher from every endpoint it watched. A left
 * behind watcher points at a table slot that gets handed to the next service,
 * so a send on the old endpoint would signal a completely unrelated set. */
static void test_destroyed_set_stops_receiving_signals(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t first = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &first);
    ipc_select_destroy(first, ctx);

    /* Reuse the freed slot for an unrelated set watching nothing. */
    uint32_t second = 0;
    CHECK(ipc_select_create(ctx, &second) == IPC_OK, "a new set takes the freed slot");
    CHECK(second == first, "the test really is exercising slot reuse");

    reset_threads();
    CHECK(ksend(ep, 1u) == IPC_OK, "a message arrives on the formerly-watched endpoint");
    uint32_t ready = 0xAAu;
    CHECK(ipc_select_wait(second, ctx, &ready, 0) == IPC_EMPTY,
          "the unrelated set watching nothing is not signalled");

    ipc_select_destroy(second, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* An endpoint torn down with a select watcher still attached must release the
 * watcher storage and stop signalling; the set itself stays usable. */
static void test_releasing_a_watched_endpoint_leaves_the_set_usable(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, gone = 0, kept = 0;
    (void)ipc_endpoint_create(ctx, &gone);
    (void)ipc_endpoint_create(CTX_A, &kept);
    (void)ipc_select_listen(ctx, (uint32_t[]){gone, kept}, 2, &sel);
    reset_threads();

    ipc_endpoints_release_owner(ctx); /* drops `gone`, keeps `kept` (CTX_A) */

    CHECK(ksend(kept, 1u) == IPC_OK, "the surviving endpoint still takes traffic");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK, "the set is still signalled");
    CHECK(ready == kept, "by the endpoint that survived");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(CTX_A);
}

static void test_select_signal_tolerates_a_null_set(void) {
    ipc_select_signal(0, 5u);
    CHECK(1, "signalling a NULL set does not fault");
}

/* --------------------------------------------------- multiple waiters */

/* The endpoint's sched_event_t supports N waiters (the service model uses one,
 * but the transport must not corrupt itself when more show up). Nothing
 * exercised more than a single blocked receiver before. */
static void test_one_send_wakes_exactly_one_waiter_in_fifo_order(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* a = thread_get(1);
    thread_t* b = thread_get(2);
    thread_t* c = thread_get(3);
    thread_t* all[3] = {a, b, c};

    park_recv(ctx, ep, a);
    park_recv(ctx, ep, b);
    park_recv(ctx, ep, c);
    CHECK(waiters_on(ep, all, 3) == 3, "three receivers are parked on one endpoint");

    g_wake_calls = 0;
    CHECK(ksend(ep, 1u) == IPC_OK, "one message arrives");
    CHECK(g_wake_calls == 1, "exactly one waiter is woken");
    CHECK(g_last_woken == a, "and it is the one that blocked first");
    CHECK(waiters_on(ep, all, 3) == 2, "the other two stay parked");
    CHECK(b->wait_event != 0 && c->wait_event != 0, "with their event pointers intact");

    CHECK(ksend(ep, 2u) == IPC_OK, "a second message arrives");
    CHECK(g_last_woken == b, "the second waiter is next, in FIFO order");
    CHECK(ksend(ep, 3u) == IPC_OK, "a third message arrives");
    CHECK(g_last_woken == c, "then the third");
    CHECK(waiters_on(ep, all, 3) == 0, "the wait list is drained");

    /* Three messages are queued and none was consumed by the wake itself. */
    CHECK(count_of(ep) == 3, "waking a receiver does not dequeue on its behalf");

    ipc_endpoints_release_owner(ctx);
}

static void test_a_send_with_no_waiter_wakes_nobody(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* a = thread_get(1);
    thread_t* all[1] = {a};

    park_recv(ctx, ep, a);
    g_wake_calls = 0;
    CHECK(ksend(ep, 1u) == IPC_OK, "the first send wakes the waiter");
    CHECK(g_wake_calls == 1, "one wake");
    CHECK(waiters_on(ep, all, 1) == 0, "nobody is left parked");
    CHECK(ksend(ep, 2u) == IPC_OK, "a second send finds an empty wait list");
    CHECK(g_wake_calls == 1, "and wakes nobody");

    ipc_endpoints_release_owner(ctx);
}

static void test_release_owner_aborts_every_waiter(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* t[4] = {thread_get(1), thread_get(2), thread_get(3), thread_get(4)};
    for (int i = 0; i < 4; ++i) {
        park_recv(ctx, ep, t[i]);
    }
    CHECK(waiters_on(ep, t, 4) == 4, "four receivers are parked");

    g_wake_calls = 0;
    ipc_endpoints_release_owner(ctx);
    CHECK(g_wake_calls == 4, "teardown wakes all four, not just the first");

    int aborted = 0;
    for (int i = 0; i < 4; ++i) {
        if (t[i]->pend_state == SCHED_PEND_ABORT && t[i]->wait_event == 0 &&
            list_head_empty(&t[i]->event_node)) {
            aborted++;
        }
    }
    CHECK(aborted == 4, "every waiter is aborted and unlinked");
}

/* sched_event_wait removes a thread from a stale wait list before re-adding it.
 * Reached through IPC it means a receiver that moves from one endpoint to
 * another can never be linked into two wait lists at once — which would splice
 * the two lists together on the next wake. */
static void test_a_receiver_moving_between_endpoints_is_in_one_list(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t a = 0, b = 0;
    (void)ipc_endpoint_create(ctx, &a);
    (void)ipc_endpoint_create(ctx, &b);
    reset_threads();
    thread_t* t = thread_get(1);

    park_recv(ctx, a, t);
    CHECK(t->wait_event != 0, "parked on the first endpoint");
    park_recv(ctx, b, t); /* re-blocks without ever being woken from `a` */
    CHECK(t->wait_event != 0, "parked on the second endpoint");

    /* A send to the abandoned endpoint must not find it. */
    g_wake_calls = 0;
    CHECK(ksend(a, 1u) == IPC_OK, "a message arrives on the abandoned endpoint");
    CHECK(g_wake_calls == 0, "the moved receiver is no longer waiting there");
    CHECK(ksend(b, 2u) == IPC_OK, "a message arrives on the current endpoint");
    CHECK(g_wake_calls == 1, "and wakes it exactly once");
    CHECK(g_last_woken == t, "the right thread");

    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------------ queue back pressure */

static void test_a_full_queue_neither_blocks_nor_wakes(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; ++i) {
        (void)ksend(ep, i);
    }
    thread_t* t = thread_get(1);
    thread_t* all[1] = {t};
    /* Park a receiver even though the queue is full: it takes the blocking
     * path only when count == 0, so this parks nobody. Park on a second
     * endpoint instead and prove the FULL send leaves it alone. */
    uint32_t idle = 0;
    (void)ipc_endpoint_create(ctx, &idle);
    park_recv(ctx, idle, t);

    g_yield_calls = 0;
    g_wake_calls = 0;
    CHECK(ksend(ep, 99u) == IPC_ERR_FULL, "the send is refused");
    CHECK(g_yield_calls == 0, "a refused send never blocks — there is no back pressure");
    CHECK(g_wake_calls == 0, "and never wakes anybody");
    CHECK(waiters_on(idle, all, 1) == 1, "the unrelated waiter is untouched");

    ipc_endpoints_release_owner(ctx);
}

/* A receiver can only park while the queue is empty, so "parked receiver plus a
 * full queue" is reachable only by filling the queue behind its back. That is
 * exactly what a burst of senders does on SMP, and the wake must fire once — on
 * the send that made the endpoint readable — and not again per message. */
static void test_a_burst_wakes_the_parked_receiver_once_and_fills_the_queue(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* t = thread_get(1);
    thread_t* all[1] = {t};
    park_recv(ctx, ep, t);

    g_wake_calls = 0;
    int all_sent = 1;
    for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; ++i) {
        if (ksend(ep, i) != IPC_OK) {
            all_sent = 0;
        }
    }
    CHECK(all_sent, "the whole burst is accepted");
    CHECK(count_of(ep) == IPC_QUEUE_DEPTH, "the queue is full");
    CHECK(g_wake_calls == 1, "the receiver is woken once, by the first message, not per message");
    CHECK(waiters_on(ep, all, 1) == 0, "and is no longer parked");

    /* The woken receiver drains a full queue normally. */
    ipc_message_t got;
    g_current_tid = t->tid;
    CHECK(ipc_recv_blocking_for(ctx, ep, &got) == IPC_OK && got.arg0 == 0u,
          "the woken receiver takes the head of a full queue");
    CHECK(ksend(ep, 99u) == IPC_OK, "and the freed slot is immediately usable");

    ipc_endpoints_release_owner(ctx);
}

static void test_every_message_bit_survives_the_queue(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    ipc_message_t m;
    memset(&m, 0xFF, sizeof(m));
    m.source = IPC_ENDPOINT_NONE;
    CHECK(ipc_send(ep, &m) == IPC_OK, "an all-ones message is accepted");
    ipc_message_t got;
    memset(&got, 0, sizeof(got));
    CHECK(ipc_recv(ep, &got) == IPC_OK, "and comes back");
    CHECK(got.type == 0xFFFFFFFFu && got.request_id == 0xFFFFFFFFu && got.arg0 == 0xFFFFFFFFu &&
              got.arg1 == 0xFFFFFFFFu && got.arg2 == 0xFFFFFFFFu && got.arg3 == 0xFFFFFFFFu,
          "with every payload field intact");
    CHECK(got.source == IPC_ENDPOINT_NONE, "the source is carried through verbatim");
    CHECK(got.destination == ep, "only destination is rewritten");
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------- endpoint table and ids */

static void test_the_table_grows_past_one_chunk(void) {
    uint32_t ctx = fresh_ctx();
    enum { N = 50 };
    uint32_t ids[N];
    int ok = 1;
    for (int i = 0; i < N; ++i) {
        ids[i] = 0;
        if (ipc_endpoint_create(ctx, &ids[i]) != IPC_OK) {
            ok = 0;
        }
    }
    CHECK(ok, "the table grows well past IPC_ENDPOINT_TABLE_CHUNK");

    int distinct = 1, resolvable = 1;
    for (int i = 0; i < N; ++i) {
        uint32_t owner = 0;
        if (ipc_endpoint_owner(ids[i], &owner) != IPC_OK || owner != ctx) {
            resolvable = 0;
        }
        for (int j = i + 1; j < N; ++j) {
            if (ids[i] == ids[j]) {
                distinct = 0;
            }
        }
    }
    CHECK(distinct, "every id is distinct across chunks");
    CHECK(resolvable, "every endpoint resolves after the table has grown");

    /* Traffic on an endpoint in a later chunk must not disturb an earlier one. */
    CHECK(ksend(ids[N - 1], 7u) == IPC_OK, "a send to the last endpoint");
    CHECK(count_of(ids[0]) == 0, "does not land on the first");
    CHECK(count_of(ids[N - 1]) == 1, "it lands where it was addressed");

    ipc_endpoints_release_owner(ctx);
}

static void test_freed_slots_are_reused_without_disturbing_neighbours(void) {
    uint32_t keep_ctx = fresh_ctx();
    uint32_t drop_ctx = fresh_ctx();
    uint32_t keep[4] = {0, 0, 0, 0};
    uint32_t drop[4] = {0, 0, 0, 0};
    /* Interleave so the freed slots sit between live ones inside a chunk. */
    for (int i = 0; i < 4; ++i) {
        (void)ipc_endpoint_create(keep_ctx, &keep[i]);
        (void)ipc_endpoint_create(drop_ctx, &drop[i]);
    }
    for (int i = 0; i < 4; ++i) {
        (void)ksend(keep[i], (uint32_t)i);
    }

    ipc_endpoints_release_owner(drop_ctx);

    uint32_t fresh[4] = {0, 0, 0, 0};
    int ok = 1;
    for (int i = 0; i < 4; ++i) {
        if (ipc_endpoint_create(drop_ctx, &fresh[i]) != IPC_OK) {
            ok = 0;
        }
    }
    CHECK(ok, "the freed slots are handed out again");

    int intact = 1;
    for (int i = 0; i < 4; ++i) {
        ipc_message_t got;
        if (count_of(keep[i]) != 1 || ipc_recv(keep[i], &got) != IPC_OK ||
            got.arg0 != (uint32_t)i) {
            intact = 0;
        }
        if (count_of(fresh[i]) != 0) {
            intact = 0; /* a recycled slot must start empty */
        }
    }
    CHECK(intact, "neighbouring live endpoints keep their queues, recycled ones start clean");

    ipc_endpoints_release_owner(keep_ctx);
    ipc_endpoints_release_owner(drop_ctx);
}

/* The id counter wraps to 1 at IPC_ENDPOINT_NONE. Without a skip, the wrapped
 * id collides with a live endpoint and ipc_endpoint_get — which returns the
 * FIRST match — hands the newcomer's traffic to the older endpoint. */
static void test_a_wrapped_id_never_collides_with_a_live_endpoint(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t low = 0;
    ipc_test_set_next_endpoint_id(1u, 0);
    CHECK(ipc_endpoint_create(ctx, &low) == IPC_OK, "an endpoint takes the low id");
    CHECK(low == 1u, "id 1, as seeded");

    /* Drive the counter to the wrap point and over it. */
    ipc_test_set_next_endpoint_id(IPC_ENDPOINT_NONE - 1u, 0);
    uint32_t last = 0, wrapped = 0;
    CHECK(ipc_endpoint_create(ctx, &last) == IPC_OK, "the last pre-wrap id is issued");
    CHECK(ipc_endpoint_create(ctx, &wrapped) == IPC_OK, "the next create wraps");
    CHECK(wrapped != low, "the wrapped id skips the live endpoint holding id 1");
    CHECK(wrapped != IPC_ENDPOINT_NONE && wrapped != 0, "and is never a reserved value");

    /* Prove routing is unambiguous: traffic for each lands only on that one. */
    CHECK(ksend(wrapped, 42u) == IPC_OK, "a send to the wrapped endpoint");
    CHECK(count_of(low) == 0, "does not land on the endpoint it could have collided with");
    CHECK(count_of(wrapped) == 1, "it lands on the intended endpoint");

    ipc_endpoints_release_owner(ctx);
    ipc_test_set_next_endpoint_id(1u, 0);
}

static void test_endpoint_creation_reports_allocation_failure(void) {
    uint32_t ctx = fresh_ctx();
    /* With allocation failing, creation succeeds only while recycled slots are
     * left in the chunks already allocated; the first create that needs a new
     * chunk must report FULL rather than hand back a bad handle. The bound is
     * generous because how many free slots exist depends on what ran earlier;
     * everything created here is owned by ctx and released below. */
    int saw_full = 0;
    uint32_t created = 0;
    g_malloc_fail = 1;
    for (uint32_t i = 0; i < 4096u; ++i) {
        uint32_t id = 0xFFFFFFFFu;
        int rc = ipc_endpoint_create(ctx, &id);
        if (rc == IPC_ERR_FULL) {
            saw_full = 1;
            CHECK(id == 0xFFFFFFFFu, "a failed create does not write the out-param");
            break;
        }
        if (rc != IPC_OK) {
            break;
        }
        created++;
    }
    g_malloc_fail = 0;
    CHECK(saw_full, "an allocation failure surfaces as IPC_ERR_FULL, not a crash or a bad handle");
    CHECK(created < 4096u, "the loop terminated on the failure, not on its bound");

    /* And the table is still usable once allocation recovers. */
    uint32_t after = 0;
    CHECK(ipc_endpoint_create(ctx, &after) == IPC_OK, "the table recovers");
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------- notification counters */

static void test_the_notification_counter_saturates(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_notification_create(ctx, &ep);

    CHECK(ipc_test_set_notify_count(ep, UINT32_MAX) == IPC_OK, "seed the counter at the ceiling");
    CHECK(ipc_notify_from(ctx, ep) == IPC_OK, "a notify at the ceiling still succeeds");
    CHECK(ipc_wait_for(ctx, ep) == IPC_OK, "and the counter did not wrap to zero");

    CHECK(ipc_test_set_notify_count(ep, 1u) == IPC_OK, "seed the counter at one");
    CHECK(ipc_wait_for(ctx, ep) == IPC_OK, "the single signal is consumed");
    CHECK(ipc_wait_for(ctx, ep) == IPC_EMPTY, "and the counter stops at zero");

    ipc_endpoints_release_owner(ctx);
}

static void test_interleaved_notify_and_wait_keep_an_exact_count(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_notification_create(ctx, &ep);
    uint32_t expected = 0;
    int ok = 1;
    /* A deterministic but irregular interleaving. */
    for (uint32_t i = 0; i < 500u; ++i) {
        if ((i % 3u) != 2u) {
            if (ipc_notify_from(ctx, ep) != IPC_OK) {
                ok = 0;
            }
            expected++;
        } else {
            int rc = ipc_wait_for(ctx, ep);
            if (expected > 0) {
                if (rc != IPC_OK) {
                    ok = 0;
                }
                expected--;
            } else if (rc != IPC_EMPTY) {
                ok = 0;
            }
        }
    }
    CHECK(ok, "every notify/wait pair agrees over 500 interleaved operations");
    uint32_t drained = 0;
    while (ipc_wait_for(ctx, ep) == IPC_OK) {
        drained++;
        if (drained > 1000u) {
            break;
        }
    }
    CHECK(drained == expected, "the final count matches exactly");
    ipc_endpoints_release_owner(ctx);
}

static void test_release_invalidates_a_signalled_notification(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_notification_create(ctx, &ep);
    CHECK(ipc_notify_from(ctx, ep) == IPC_OK, "a pending signal exists");
    ipc_endpoints_release_owner(ctx);
    CHECK(ipc_wait_for(ctx, ep) == IPC_ERR_INVALID,
          "the released endpoint is gone, pending signal and all");
    CHECK(ipc_notify_from(ctx, ep) == IPC_ERR_INVALID, "and cannot be signalled again");
}

/* --------------------------------------------------- more select coverage */

static void test_select_add_reports_a_failed_watcher_registration(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_create(ctx, &sel);

    g_malloc_fail = 1;
    int rc = ipc_select_add(sel, ep, ctx);
    g_malloc_fail = 0;
    CHECK(rc != IPC_OK, "a watcher that could not be registered is not reported as success");

    /* The whole point: a set that reports OK must actually be signalled. */
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "the retry succeeds once allocation recovers");
    CHECK(ksend(ep, 1u) == IPC_OK, "a message arrives");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK && ready == ep,
          "and the set really is watching");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* A duplicate add is a no-op: the endpoint is already watched, so the set must
 * not gain a second watcher or spend a second of its eight slots. Refusing
 * would be wrong too -- callers build a set from several handles that can
 * legitimately coincide, and failing a harmless call would break them at
 * startup. */
static void test_adding_the_same_endpoint_twice_is_a_no_op(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_create(ctx, &sel);
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "first add");
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "a duplicate add reports success");
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "and stays idempotent however often it repeats");
    reset_threads();

    /* Seven more fit: the duplicates cost nothing. */
    uint32_t filler = 0;
    int fit = 0;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX; ++i) {
        (void)ipc_endpoint_create(ctx, &filler);
        if (ipc_select_add(sel, filler, ctx) != IPC_OK) {
            break;
        }
        fit++;
    }
    CHECK(fit == (int)IPC_SELECT_EPS_MAX - 1, "a duplicate consumes no watch slot");

    /* Re-adding an endpoint the set already watches must work even when the
     * set is full -- there is nothing to allocate. */
    CHECK(ipc_select_add(sel, ep, ctx) == IPC_OK, "a duplicate is still accepted on a full set");

    /* And the endpoint is watched exactly once, so one message yields one
     * readiness report. A second watcher would fire ipc_select_signal twice;
     * that is masked today by ready_ep being a latch, so the slot accounting
     * above is what actually proves the dedupe -- this pins the behaviour the
     * caller sees. */
    CHECK(ksend(ep, 1u) == IPC_OK, "one message arrives on the endpoint");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK && ready == ep,
          "the set reports it ready");
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY,
          "and reports it exactly once, not once per registration");
    CHECK(count_of(ep) == 1, "with a single message actually queued");
    ipc_message_t got;
    CHECK(ipc_recv_for(ctx, ep, &got) == IPC_OK && got.arg0 == 1u, "which the caller drains");

    /* Destroy must clear BOTH entries. A surviving one would point at a table
     * slot the next service gets handed. */
    ipc_select_destroy(sel, ctx);
    uint32_t next = 0;
    CHECK(ipc_select_create(ctx, &next) == IPC_OK && next == sel, "the slot is recycled");
    reset_threads();
    CHECK(ksend(ep, 2u) == IPC_OK, "a message arrives on the old endpoint");
    ready = 0xAAu;
    CHECK(ipc_select_wait(next, ctx, &ready, 0) == IPC_EMPTY,
          "no leftover watcher signals the set that inherited the slot");

    ipc_select_destroy(next, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* listen() is the convenience wrapper most services use, so a caller that
 * passes the same endpoint twice -- easy to do when the reply and event
 * endpoints turn out to be the same handle -- must behave like two explicit
 * adds rather than failing or double-reporting. */
static void test_listen_tolerates_a_repeated_endpoint(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t a = 0, b = 0, sel = 0;
    (void)ipc_endpoint_create(ctx, &a);
    (void)ipc_endpoint_create(ctx, &b);
    reset_threads();

    CHECK(ipc_select_listen(ctx, (uint32_t[]){a, b, a}, 3, &sel) == IPC_OK,
          "listen accepts a repeated endpoint");

    /* The repeat is deduplicated, so the set spent two slots and not three:
     * IPC_SELECT_EPS_MAX - 2 more still fit. */
    uint32_t filler = 0;
    int fit = 0;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX; ++i) {
        (void)ipc_endpoint_create(ctx, &filler);
        if (ipc_select_add(sel, filler, ctx) != IPC_OK) {
            break;
        }
        fit++;
    }
    CHECK(fit == (int)IPC_SELECT_EPS_MAX - 2, "the repeated endpoint cost one slot, not two");

    CHECK(ksend(a, 5u) == IPC_OK, "a message arrives on the repeated endpoint");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK && ready == a, "the set reports it ready");
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY, "exactly once");

    /* The endpoint that was NOT repeated still works — the duplicate did not
     * displace it. */
    CHECK(ksend(b, 6u) == IPC_OK, "a message arrives on the other endpoint");
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK && ready == b,
          "which is still watched too");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* sched_event_wait unlinks a thread from its current wait list before adding
 * it again. Re-blocking on the endpoint you are ALREADY parked on is that
 * unlink and add against the SAME list -- get it wrong and the list head is
 * rewired around the node, silently dropping every waiter queued behind it. */
static void test_re_blocking_on_the_same_endpoint_keeps_the_wait_list_intact(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();
    thread_t* a = thread_get(1);
    thread_t* b = thread_get(2);
    thread_t* both[2] = {a, b};

    park_recv(ctx, ep, a);
    park_recv(ctx, ep, b);
    CHECK(waiters_on(ep, both, 2) == 2, "two receivers are parked");

    park_recv(ctx, ep, a); /* a re-blocks on the list it is already in */
    CHECK(waiters_on(ep, both, 2) == 2, "both are still parked after the re-block");

    g_wake_calls = 0;
    CHECK(ksend(ep, 1u) == IPC_OK, "a message arrives");
    CHECK(g_wake_calls == 1, "exactly one waiter is woken");
    CHECK(ksend(ep, 2u) == IPC_OK, "and another");
    CHECK(g_wake_calls == 2, "waking the second — neither was lost from the list");
    CHECK(waiters_on(ep, both, 2) == 0, "the list drains completely");

    /* A third send must find the list genuinely empty, not a stale node. */
    CHECK(ksend(ep, 3u) == IPC_OK, "a third message arrives");
    CHECK(g_wake_calls == 2, "and wakes nobody — no duplicate entry survived");

    ipc_endpoints_release_owner(ctx);
}

static void test_two_sets_watching_one_endpoint_are_both_signalled(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0, s1 = 0, s2 = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &s1);
    (void)ipc_select_listen(ctx, &ep, 1, &s2);
    reset_threads();

    CHECK(ksend(ep, 1u) == IPC_OK, "one message arrives");
    uint32_t r1 = 0, r2 = 0;
    CHECK(ipc_select_wait(s1, ctx, &r1, 0) == IPC_OK && r1 == ep, "the first set is signalled");
    CHECK(ipc_select_wait(s2, ctx, &r2, 0) == IPC_OK && r2 == ep, "and so is the second");
    CHECK(g_yield_calls == 0, "neither had to block");

    ipc_select_destroy(s1, ctx);
    ipc_select_destroy(s2, ctx);
    ipc_endpoints_release_owner(ctx);
}

static void test_a_full_set_reports_each_of_its_endpoints(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t eps[IPC_SELECT_EPS_MAX];
    uint32_t sel = 0;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX; ++i) {
        eps[i] = 0;
        (void)ipc_endpoint_create(ctx, &eps[i]);
    }
    CHECK(ipc_select_listen(ctx, eps, IPC_SELECT_EPS_MAX, &sel) == IPC_OK,
          "a set watching the maximum number of endpoints");

    int ok = 1;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX; ++i) {
        uint32_t ready = 0;
        ipc_message_t got;
        if (ksend(eps[i], i) != IPC_OK) {
            ok = 0;
        }
        if (ipc_select_recv(sel, ctx, &ready, &got, 0) != IPC_OK || ready != eps[i] ||
            got.arg0 != i) {
            ok = 0;
        }
    }
    CHECK(ok, "each endpoint in turn is reported by id with its own message");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* ready_ep is a single latch, not a queue. Two sends before a wait collapse to
 * one report — which is why the documented contract is "re-poll every watched
 * endpoint", not "handle the one you were told about". */
static void test_the_readiness_latch_is_last_writer_wins(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t a = 0, b = 0, sel = 0;
    (void)ipc_endpoint_create(ctx, &a);
    (void)ipc_endpoint_create(ctx, &b);
    (void)ipc_select_listen(ctx, (uint32_t[]){a, b}, 2, &sel);
    reset_threads();

    CHECK(ksend(a, 1u) == IPC_OK, "the first endpoint gets a message");
    CHECK(ksend(b, 2u) == IPC_OK, "then the second");
    uint32_t ready = 0;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_OK, "the set reports ready");
    CHECK(ready == b, "naming the most recent signal");
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY,
          "the earlier signal was collapsed, not queued");
    CHECK(count_of(a) == 1, "but its message is still queued for a re-poll");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* The set owner, not the endpoint owner, is what select_recv runs as. A
 * kernel-owned set may therefore drain another context's endpoint. The
 * converse -- a set whose owner does NOT own the endpoint -- is the
 * "select_recv(endpoint not owned)" row of the error contract. */
static void test_a_kernel_owned_set_may_drain_another_contexts_endpoint(void) {
    uint32_t owner = fresh_ctx();
    uint32_t ep = 0, sel = 0;
    (void)ipc_endpoint_create(owner, &ep);
    /* A kernel-owned set may watch another context's endpoint, but the recv
     * that follows runs as that set's owner. */
    (void)ipc_select_listen(IPC_CONTEXT_KERNEL, &ep, 1, &sel);
    CHECK(ksend(ep, 1u) == IPC_OK, "a message arrives");

    uint32_t ready = 0;
    ipc_message_t got;
    CHECK(ipc_select_recv(sel, IPC_CONTEXT_KERNEL, &ready, &got, 0) == IPC_OK,
          "the kernel owner may drain it");
    CHECK(ready == ep, "from the right endpoint");

    ipc_select_destroy(sel, IPC_CONTEXT_KERNEL);
    ipc_endpoints_release_owner(owner);
}

static void test_a_set_survives_its_owner_losing_every_endpoint(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0, sel = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &sel);
    reset_threads();

    ipc_endpoints_release_owner(ctx);

    uint32_t ready = 0xAAu;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY,
          "a set whose endpoints are gone reports nothing ready rather than a stale id");
    ipc_message_t got;
    CHECK(ipc_select_recv(sel, ctx, &ready, &got, 0) == IPC_EMPTY, "and select_recv agrees");
    ipc_select_destroy(sel, ctx); /* must not fault on the dead endpoint ids */
    CHECK(1, "destroying a set holding dead endpoint ids is safe");
}

static void test_select_destroy_is_idempotent(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t sel = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &sel);
    ipc_select_destroy(sel, ctx);
    ipc_select_destroy(sel, ctx); /* second one must be a no-op, not a double free */
    CHECK(1, "a repeated destroy is a no-op");

    /* And the slot really is free, not double-released into some bad state. */
    uint32_t again = 0;
    CHECK(ipc_select_create(ctx, &again) == IPC_OK, "the slot is reusable");
    CHECK(again == sel, "and it is the same slot");
    ipc_select_destroy(again, ctx);
    ipc_endpoints_release_owner(ctx);
}

static void test_listen_cleans_up_when_an_add_fails_midway(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t eps[3] = {0, 0, 0};
    (void)ipc_endpoint_create(ctx, &eps[0]);
    (void)ipc_endpoint_create(ctx, &eps[2]);
    /* Slot 1 is a live endpoint whose watcher registration is made to fail. */
    (void)ipc_endpoint_create(ctx, &eps[1]);

    uint32_t before = 0, sel = 0;
    (void)ipc_select_create(ctx, &before);
    ipc_select_destroy(before, ctx); /* note which slot is next in line */

    g_malloc_fail = 1;
    int rc = ipc_select_listen(ctx, eps, 3, &sel);
    g_malloc_fail = 0;
    CHECK(rc != IPC_OK, "the partial listen fails");

    uint32_t after = 0;
    CHECK(ipc_select_create(ctx, &after) == IPC_OK, "a set can still be created");
    CHECK(after == before, "the half-built set was destroyed, not leaked");
    ipc_select_destroy(after, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* poll_notify is only wired into ipc_send_from, so a notification endpoint in a
 * select set never becomes ready however often it is signalled. */
static void test_a_notification_endpoint_never_signals_a_set(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t note = 0, sel = 0;
    (void)ipc_notification_create(ctx, &note);
    (void)ipc_select_listen(ctx, &note, 1, &sel);
    reset_threads();

    CHECK(ipc_notify_from(ctx, note) == IPC_OK, "the notification is raised");
    uint32_t ready = 0xAAu;
    CHECK(ipc_select_wait(sel, ctx, &ready, 0) == IPC_EMPTY,
          "select does not observe notification endpoints");
    CHECK(ipc_wait_for(ctx, note) == IPC_OK, "the signal is still there for a direct wait");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------------------- timeouts */

static uint64_t g_observed_deadline;

static void hook_record_deadline(void) {
    thread_t* self = thread_get(g_current_tid);
    g_observed_deadline = self ? self->sched_timeout_tick : 0;
}

static void test_endpoint_wait_hands_the_timeout_through_unchanged(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    reset_threads();

    g_observed_deadline = 0;
    g_yield_hook = hook_record_deadline;
    CHECK(ipc_endpoint_wait_for(ctx, ep, 250u) == IPC_OK, "the timed wait runs");
    CHECK(g_observed_deadline == g_now + 250u,
          "the caller's timeout reaches the scheduler unscaled and uncoerced");

    /* Zero means "forever", and must arm nothing. */
    g_observed_deadline = 0xFFFFu;
    g_yield_hook = hook_record_deadline;
    CHECK(ipc_endpoint_wait_for(ctx, ep, 0u) == IPC_OK, "the untimed wait runs");
    CHECK(g_observed_deadline == 0, "a zero timeout arms no deadline");

    ipc_endpoints_release_owner(ctx);
}

static void test_a_timed_select_wait_expires_as_empty(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0, sel = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_listen(ctx, &ep, 1, &sel);
    reset_threads();
    thread_t* t = thread_get(1);

    /* Park in the timed wait, then let the deadline pass. */
    g_park = 1;
    uint32_t ready = 0xAAu;
    int rc = ipc_select_wait(sel, ctx, &ready, 100u);
    g_park = 0;
    t->state = THREAD_STATE_BLOCKED;
    t->sched_timeout_tick = g_now + 100u; /* see the MODELLING NOTE at the top */
    CHECK(rc == IPC_EMPTY, "the wait that has not been signalled reports EMPTY");
    CHECK(ready == 0xAAu, "and leaves the caller's out-param alone");

    g_now += 200u;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 1, "the expired deadline wakes the waiter");
    CHECK(t->pend_state == SCHED_PEND_TIMEOUT, "as a timeout, not a delivery");

    ipc_select_destroy(sel, ctx);
    ipc_endpoints_release_owner(ctx);
}

/* ------------------------------------------------------- the error contract
 *
 * Every value each entry point can return, enumerated in one table against the
 * precondition that produces it. The tests above assert codes incidentally,
 * while checking behaviour; this asserts the code IS the contract, so a
 * refactor that starts returning INVALID where it used to return PERM -- or
 * quietly turns a distinguishable failure into a generic one -- fails here
 * with the case named, rather than passing because no test happened to look.
 *
 * The kernel wrappers (ipc_send/recv/notify/wait) are covered by their own
 * equivalence case rather than repeating every row twice.
 */

#define BAD_EP 0x7FFFFFFFu
#define BAD_SEL 0xFFFFu

static struct {
    uint32_t ctx;          /* the acting context */
    uint32_t other;        /* a foreign context */
    uint32_t msg_ep;       /* MESSAGE, owned by ctx, empty */
    uint32_t note_ep;      /* NOTIFICATION, owned by ctx, unsignalled */
    uint32_t ready_ep;     /* MESSAGE, owned by ctx, one message queued */
    uint32_t full_ep;      /* MESSAGE, owned by ctx, queue at capacity */
    uint32_t foreign_ep;   /* MESSAGE, owned by other */
    uint32_t foreign_note; /* NOTIFICATION, owned by other */
    uint32_t sel;          /* select set owned by ctx, watching msg_ep */
} g_env;

static ipc_message_t g_scratch;

static const char* code_name(int rc) {
    switch (rc) {
    case IPC_OK:
        return "IPC_OK";
    case IPC_EMPTY:
        return "IPC_EMPTY";
    case IPC_ERR_INVALID:
        return "IPC_ERR_INVALID";
    case IPC_ERR_PERM:
        return "IPC_ERR_PERM";
    case IPC_ERR_FULL:
        return "IPC_ERR_FULL";
    default:
        return "<unknown>";
    }
}

/* --- creation ---------------------------------------------------------- */
static int c_create_null_out(void) {
    return ipc_endpoint_create(g_env.ctx, 0);
}
static int c_note_create_null_out(void) {
    return ipc_notification_create(g_env.ctx, 0);
}
static int c_create_ok(void) {
    uint32_t id = 0;
    return ipc_endpoint_create(g_env.ctx, &id);
}
static int c_note_create_ok(void) {
    uint32_t id = 0;
    return ipc_notification_create(g_env.ctx, &id);
}
/* Exhaust the backing allocator rather than the table: list_alloc only fails
 * when it must grow, so keep creating until it does. */
static int c_create_alloc_fail(void) {
    uint32_t junk = fresh_ctx();
    int rc = IPC_OK;
    g_malloc_fail = 1;
    for (uint32_t i = 0; i < 4096u && rc == IPC_OK; ++i) {
        uint32_t id = 0;
        rc = ipc_endpoint_create(junk, &id);
    }
    g_malloc_fail = 0;
    ipc_endpoints_release_owner(junk);
    return rc;
}
static int c_note_create_alloc_fail(void) {
    uint32_t junk = fresh_ctx();
    int rc = IPC_OK;
    g_malloc_fail = 1;
    for (uint32_t i = 0; i < 4096u && rc == IPC_OK; ++i) {
        uint32_t id = 0;
        rc = ipc_notification_create(junk, &id);
    }
    g_malloc_fail = 0;
    ipc_endpoints_release_owner(junk);
    return rc;
}

/* --- queries ----------------------------------------------------------- */
static int c_owner_unknown(void) {
    uint32_t o = 0;
    return ipc_endpoint_owner(BAD_EP, &o);
}
static int c_owner_reserved_zero(void) {
    uint32_t o = 0;
    return ipc_endpoint_owner(0, &o);
}
static int c_owner_reserved_none(void) {
    uint32_t o = 0;
    return ipc_endpoint_owner(IPC_ENDPOINT_NONE, &o);
}
static int c_owner_null_out(void) {
    return ipc_endpoint_owner(g_env.msg_ep, 0);
}
static int c_owner_ok(void) {
    uint32_t o = 0;
    return ipc_endpoint_owner(g_env.msg_ep, &o);
}
static int c_count_unknown(void) {
    uint32_t n = 0;
    return ipc_endpoint_count(BAD_EP, &n);
}
static int c_count_null_out(void) {
    return ipc_endpoint_count(g_env.msg_ep, 0);
}
static int c_count_ok(void) {
    uint32_t n = 0;
    return ipc_endpoint_count(g_env.msg_ep, &n);
}

/* --- send -------------------------------------------------------------- */
static ipc_message_t owned_msg(void) {
    ipc_message_t m = msg_of(g_env.msg_ep, 1u, 1u); /* source owned by ctx */
    return m;
}
static int c_send_null_message(void) {
    return ipc_send_from(g_env.ctx, g_env.msg_ep, 0);
}
static int c_send_unknown_dest(void) {
    ipc_message_t m = owned_msg();
    return ipc_send_from(g_env.ctx, BAD_EP, &m);
}
static int c_send_notification_dest(void) {
    ipc_message_t m = owned_msg();
    return ipc_send_from(g_env.ctx, g_env.note_ep, &m);
}
static int c_send_no_source(void) {
    ipc_message_t m = msg_of(IPC_ENDPOINT_NONE, 1u, 1u);
    return ipc_send_from(g_env.ctx, g_env.msg_ep, &m);
}
static int c_send_foreign_source(void) {
    ipc_message_t m = msg_of(g_env.foreign_ep, 1u, 1u);
    return ipc_send_from(g_env.ctx, g_env.msg_ep, &m);
}
static int c_send_unknown_source(void) {
    ipc_message_t m = msg_of(BAD_EP, 1u, 1u);
    return ipc_send_from(g_env.ctx, g_env.msg_ep, &m);
}
static int c_send_full(void) {
    ipc_message_t m = owned_msg();
    return ipc_send_from(g_env.ctx, g_env.full_ep, &m);
}
static int c_send_ok(void) {
    ipc_message_t m = owned_msg();
    return ipc_send_from(g_env.ctx, g_env.msg_ep, &m);
}

/* --- non-blocking receive ---------------------------------------------- */
static int c_recv_null_out(void) {
    return ipc_recv_for(g_env.ctx, g_env.ready_ep, 0);
}
static int c_recv_unknown(void) {
    return ipc_recv_for(g_env.ctx, BAD_EP, &g_scratch);
}
static int c_recv_notification(void) {
    return ipc_recv_for(g_env.ctx, g_env.note_ep, &g_scratch);
}
static int c_recv_non_owner(void) {
    return ipc_recv_for(g_env.other, g_env.ready_ep, &g_scratch);
}
static int c_recv_empty(void) {
    return ipc_recv_for(g_env.ctx, g_env.msg_ep, &g_scratch);
}
static int c_recv_ok(void) {
    return ipc_recv_for(g_env.ctx, g_env.ready_ep, &g_scratch);
}

/* --- blocking receive --------------------------------------------------- */
static int c_brecv_null_out(void) {
    return ipc_recv_blocking_for(g_env.ctx, g_env.ready_ep, 0);
}
static int c_brecv_unknown(void) {
    return ipc_recv_blocking_for(g_env.ctx, BAD_EP, &g_scratch);
}
static int c_brecv_notification(void) {
    return ipc_recv_blocking_for(g_env.ctx, g_env.note_ep, &g_scratch);
}
static int c_brecv_non_owner(void) {
    return ipc_recv_blocking_for(g_env.other, g_env.ready_ep, &g_scratch);
}
static int c_brecv_spurious(void) {
    return ipc_recv_blocking_for(g_env.ctx, g_env.msg_ep, &g_scratch);
}
static int c_brecv_ok(void) {
    return ipc_recv_blocking_for(g_env.ctx, g_env.ready_ep, &g_scratch);
}
/* The endpoint vanishing while its receiver is parked: the post-wake re-lookup
 * fails, and that must be INVALID rather than EMPTY, or the caller retries
 * forever on a handle that no longer exists. */
static uint32_t g_doomed_ctx;
static void hook_release_doomed(void) {
    ipc_endpoints_release_owner(g_doomed_ctx);
}
static int c_brecv_endpoint_destroyed(void) {
    uint32_t ep = 0;
    g_doomed_ctx = fresh_ctx();
    (void)ipc_endpoint_create(g_doomed_ctx, &ep);
    g_yield_hook = hook_release_doomed;
    return ipc_recv_blocking_for(g_doomed_ctx, ep, &g_scratch);
}

/* --- readable wait ------------------------------------------------------ */
static int c_epwait_unknown(void) {
    return ipc_endpoint_wait_for(g_env.ctx, BAD_EP, 0);
}
static int c_epwait_notification(void) {
    return ipc_endpoint_wait_for(g_env.ctx, g_env.note_ep, 0);
}
static int c_epwait_non_owner(void) {
    return ipc_endpoint_wait_for(g_env.other, g_env.ready_ep, 0);
}
static int c_epwait_ok(void) {
    return ipc_endpoint_wait_for(g_env.ctx, g_env.ready_ep, 0);
}

/* --- notify / wait ------------------------------------------------------ */
static int c_notify_unknown(void) {
    return ipc_notify_from(g_env.ctx, BAD_EP);
}
static int c_notify_message_ep(void) {
    return ipc_notify_from(g_env.ctx, g_env.msg_ep);
}
static int c_notify_foreign(void) {
    return ipc_notify_from(g_env.ctx, g_env.foreign_note);
}
static int c_notify_ok(void) {
    return ipc_notify_from(g_env.ctx, g_env.note_ep);
}
static int c_wait_unknown(void) {
    return ipc_wait_for(g_env.ctx, BAD_EP);
}
static int c_wait_message_ep(void) {
    return ipc_wait_for(g_env.ctx, g_env.msg_ep);
}
static int c_wait_foreign(void) {
    return ipc_wait_for(g_env.ctx, g_env.foreign_note);
}
static int c_wait_empty(void) {
    return ipc_wait_for(g_env.ctx, g_env.note_ep);
}
static int c_wait_ok(void) {
    (void)ipc_notify_from(g_env.ctx, g_env.note_ep);
    return ipc_wait_for(g_env.ctx, g_env.note_ep);
}

/* --- select ------------------------------------------------------------- */
static int c_sel_create_null_out(void) {
    return ipc_select_create(g_env.ctx, 0);
}
static int c_sel_create_ok(void) {
    uint32_t s = 0;
    int rc = ipc_select_create(g_env.ctx, &s);
    if (rc == IPC_OK) {
        ipc_select_destroy(s, g_env.ctx);
    }
    return rc;
}
static int c_sel_create_exhausted(void) {
    uint32_t ids[64];
    uint32_t n = 0;
    int rc;
    while (n < 64) {
        uint32_t s = 0;
        if (ipc_select_create(g_env.ctx, &s) != IPC_OK) {
            break;
        }
        ids[n++] = s;
    }
    uint32_t overflow = 0;
    rc = ipc_select_create(g_env.ctx, &overflow);
    for (uint32_t i = 0; i < n; ++i) {
        ipc_select_destroy(ids[i], g_env.ctx);
    }
    return rc;
}
static int c_sel_add_zero_id(void) {
    return ipc_select_add(0, g_env.msg_ep, g_env.ctx);
}
static int c_sel_add_out_of_range(void) {
    return ipc_select_add(BAD_SEL, g_env.msg_ep, g_env.ctx);
}
static int c_sel_add_foreign_owner(void) {
    return ipc_select_add(g_env.sel, g_env.msg_ep, g_env.other);
}
static int c_sel_add_duplicate(void) {
    return ipc_select_add(g_env.sel, g_env.msg_ep, g_env.ctx); /* already watched */
}
static int c_sel_add_capacity(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t s = 0;
    int rc = IPC_OK;
    (void)ipc_select_create(ctx, &s);
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX + 1u && rc == IPC_OK; ++i) {
        uint32_t ep = 0;
        (void)ipc_endpoint_create(ctx, &ep);
        rc = ipc_select_add(s, ep, ctx);
    }
    ipc_select_destroy(s, ctx);
    ipc_endpoints_release_owner(ctx);
    return rc;
}
static int c_sel_add_watcher_alloc_fail(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t s = 0, ep = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    (void)ipc_select_create(ctx, &s);
    g_malloc_fail = 1;
    int rc = ipc_select_add(s, ep, ctx);
    g_malloc_fail = 0;
    ipc_select_destroy(s, ctx);
    ipc_endpoints_release_owner(ctx);
    return rc;
}
static int c_sel_add_ok(void) {
    uint32_t ep = 0;
    (void)ipc_endpoint_create(g_env.ctx, &ep);
    return ipc_select_add(g_env.sel, ep, g_env.ctx);
}
static int c_sel_wait_null_out(void) {
    return ipc_select_wait(g_env.sel, g_env.ctx, 0, 0);
}
static int c_sel_wait_bad_id(void) {
    uint32_t r = 0;
    return ipc_select_wait(BAD_SEL, g_env.ctx, &r, 0);
}
static int c_sel_wait_foreign_owner(void) {
    uint32_t r = 0;
    return ipc_select_wait(g_env.sel, g_env.other, &r, 0);
}
static int c_sel_wait_unsignalled(void) {
    uint32_t r = 0;
    return ipc_select_wait(g_env.sel, g_env.ctx, &r, 0);
}
static int c_sel_wait_ok(void) {
    uint32_t r = 0;
    ipc_message_t m = owned_msg();
    (void)ipc_send_from(g_env.ctx, g_env.msg_ep, &m);
    int rc = ipc_select_wait(g_env.sel, g_env.ctx, &r, 0);
    (void)ipc_recv_for(g_env.ctx, g_env.msg_ep, &g_scratch);
    return rc;
}
static int c_listen_null_endpoints(void) {
    uint32_t s = 0;
    return ipc_select_listen(g_env.ctx, 0, 1, &s);
}
static int c_listen_null_out(void) {
    uint32_t eps[1] = {g_env.msg_ep};
    return ipc_select_listen(g_env.ctx, eps, 1, 0);
}
static int c_listen_zero_count(void) {
    uint32_t s = 0;
    uint32_t eps[1] = {g_env.msg_ep};
    return ipc_select_listen(g_env.ctx, eps, 0, &s);
}
/* listen propagates whatever the failing add returned, rather than flattening
 * it to one generic code. */
static int c_listen_propagates_full(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t eps[IPC_SELECT_EPS_MAX + 1u];
    uint32_t s = 0;
    for (uint32_t i = 0; i < IPC_SELECT_EPS_MAX + 1u; ++i) {
        eps[i] = 0;
        (void)ipc_endpoint_create(ctx, &eps[i]);
    }
    int rc = ipc_select_listen(ctx, eps, IPC_SELECT_EPS_MAX + 1u, &s);
    ipc_endpoints_release_owner(ctx);
    return rc;
}
static int c_listen_ok(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t ep = 0, s = 0;
    (void)ipc_endpoint_create(ctx, &ep);
    int rc = ipc_select_listen(ctx, &ep, 1, &s);
    if (rc == IPC_OK) {
        ipc_select_destroy(s, ctx);
    }
    ipc_endpoints_release_owner(ctx);
    return rc;
}
static int c_sel_recv_null_message(void) {
    uint32_t r = 0;
    return ipc_select_recv(g_env.sel, g_env.ctx, &r, 0, 0);
}
static int c_sel_recv_bad_id(void) {
    uint32_t r = 0;
    return ipc_select_recv(BAD_SEL, g_env.ctx, &r, &g_scratch, 0);
}
static int c_sel_recv_unsignalled(void) {
    uint32_t r = 0;
    return ipc_select_recv(g_env.sel, g_env.ctx, &r, &g_scratch, 0);
}
/* A set may watch an endpoint its owner does not own -- add checks the SET's
 * owner, not the endpoint's. The dequeue that follows then fails the receive
 * ownership check, and select_recv must propagate that PERM rather than
 * reporting EMPTY, which would send the caller into a retry loop. */
static int c_sel_recv_endpoint_not_owned(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t s = 0, r = 0;
    (void)ipc_select_create(ctx, &s);
    (void)ipc_select_add(s, g_env.foreign_ep, ctx);
    (void)ksend(g_env.foreign_ep, 1u);
    int rc = ipc_select_recv(s, ctx, &r, &g_scratch, 0);
    ipc_select_destroy(s, ctx);
    (void)ipc_recv_for(IPC_CONTEXT_KERNEL, g_env.foreign_ep, &g_scratch);
    return rc;
}
static int c_sel_recv_ok(void) {
    uint32_t r = 0;
    ipc_message_t m = owned_msg();
    (void)ipc_send_from(g_env.ctx, g_env.msg_ep, &m);
    return ipc_select_recv(g_env.sel, g_env.ctx, &r, &g_scratch, 0);
}

static void contract_env_build(void) {
    memset(&g_env, 0, sizeof(g_env));
    g_env.ctx = fresh_ctx();
    g_env.other = fresh_ctx();
    (void)ipc_endpoint_create(g_env.ctx, &g_env.msg_ep);
    (void)ipc_notification_create(g_env.ctx, &g_env.note_ep);
    (void)ipc_endpoint_create(g_env.ctx, &g_env.ready_ep);
    (void)ipc_endpoint_create(g_env.ctx, &g_env.full_ep);
    (void)ipc_endpoint_create(g_env.other, &g_env.foreign_ep);
    (void)ipc_notification_create(g_env.other, &g_env.foreign_note);
    (void)ksend(g_env.ready_ep, 1u);
    for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; ++i) {
        (void)ksend(g_env.full_ep, i);
    }
    (void)ipc_select_listen(g_env.ctx, &g_env.msg_ep, 1, &g_env.sel);
}

static void contract_env_teardown(void) {
    ipc_select_destroy(g_env.sel, g_env.ctx);
    ipc_endpoints_release_owner(g_env.ctx);
    ipc_endpoints_release_owner(g_env.other);
}

static void test_error_code_contract(void) {
    struct {
        const char* what;
        int expect;
        int (*run)(void);
    } cases[] = {
        {"endpoint_create(NULL out)", IPC_ERR_INVALID, c_create_null_out},
        {"notification_create(NULL out)", IPC_ERR_INVALID, c_note_create_null_out},
        {"endpoint_create(valid)", IPC_OK, c_create_ok},
        {"notification_create(valid)", IPC_OK, c_note_create_ok},
        {"endpoint_create(allocation fails)", IPC_ERR_FULL, c_create_alloc_fail},
        {"notification_create(allocation fails)", IPC_ERR_FULL, c_note_create_alloc_fail},

        {"endpoint_owner(unknown)", IPC_ERR_INVALID, c_owner_unknown},
        {"endpoint_owner(id 0)", IPC_ERR_INVALID, c_owner_reserved_zero},
        {"endpoint_owner(NONE)", IPC_ERR_INVALID, c_owner_reserved_none},
        {"endpoint_owner(NULL out)", IPC_ERR_INVALID, c_owner_null_out},
        {"endpoint_owner(valid)", IPC_OK, c_owner_ok},
        {"endpoint_count(unknown)", IPC_ERR_INVALID, c_count_unknown},
        {"endpoint_count(NULL out)", IPC_ERR_INVALID, c_count_null_out},
        {"endpoint_count(valid)", IPC_OK, c_count_ok},

        {"send_from(NULL message)", IPC_ERR_INVALID, c_send_null_message},
        {"send_from(unknown destination)", IPC_ERR_INVALID, c_send_unknown_dest},
        {"send_from(notification destination)", IPC_ERR_INVALID, c_send_notification_dest},
        {"send_from(no source)", IPC_ERR_PERM, c_send_no_source},
        {"send_from(foreign source)", IPC_ERR_PERM, c_send_foreign_source},
        {"send_from(unknown source)", IPC_ERR_PERM, c_send_unknown_source},
        {"send_from(queue full)", IPC_ERR_FULL, c_send_full},
        {"send_from(valid)", IPC_OK, c_send_ok},

        {"recv_for(NULL out)", IPC_ERR_INVALID, c_recv_null_out},
        {"recv_for(unknown)", IPC_ERR_INVALID, c_recv_unknown},
        {"recv_for(notification endpoint)", IPC_ERR_INVALID, c_recv_notification},
        {"recv_for(non-owner)", IPC_ERR_PERM, c_recv_non_owner},
        {"recv_for(empty)", IPC_EMPTY, c_recv_empty},
        {"recv_for(valid)", IPC_OK, c_recv_ok},

        {"recv_blocking_for(NULL out)", IPC_ERR_INVALID, c_brecv_null_out},
        {"recv_blocking_for(unknown)", IPC_ERR_INVALID, c_brecv_unknown},
        {"recv_blocking_for(notification endpoint)", IPC_ERR_INVALID, c_brecv_notification},
        {"recv_blocking_for(non-owner)", IPC_ERR_PERM, c_brecv_non_owner},
        {"recv_blocking_for(spurious wake)", IPC_EMPTY, c_brecv_spurious},
        {"recv_blocking_for(endpoint destroyed while parked)", IPC_ERR_INVALID,
         c_brecv_endpoint_destroyed},
        {"recv_blocking_for(valid)", IPC_OK, c_brecv_ok},

        {"endpoint_wait_for(unknown)", IPC_ERR_INVALID, c_epwait_unknown},
        {"endpoint_wait_for(notification endpoint)", IPC_ERR_INVALID, c_epwait_notification},
        {"endpoint_wait_for(non-owner)", IPC_ERR_PERM, c_epwait_non_owner},
        {"endpoint_wait_for(readable)", IPC_OK, c_epwait_ok},

        {"notify_from(unknown)", IPC_ERR_INVALID, c_notify_unknown},
        {"notify_from(message endpoint)", IPC_ERR_INVALID, c_notify_message_ep},
        {"notify_from(foreign endpoint)", IPC_ERR_PERM, c_notify_foreign},
        {"notify_from(valid)", IPC_OK, c_notify_ok},
        {"wait_for(unknown)", IPC_ERR_INVALID, c_wait_unknown},
        {"wait_for(message endpoint)", IPC_ERR_INVALID, c_wait_message_ep},
        {"wait_for(foreign endpoint)", IPC_ERR_PERM, c_wait_foreign},
        {"wait_for(unsignalled)", IPC_EMPTY, c_wait_empty},
        {"wait_for(signalled)", IPC_OK, c_wait_ok},

        {"select_create(NULL out)", IPC_ERR_INVALID, c_sel_create_null_out},
        {"select_create(valid)", IPC_OK, c_sel_create_ok},
        {"select_create(table exhausted)", IPC_ERR_FULL, c_sel_create_exhausted},
        {"select_add(id 0)", IPC_ERR_INVALID, c_sel_add_zero_id},
        {"select_add(out of range id)", IPC_ERR_INVALID, c_sel_add_out_of_range},
        {"select_add(foreign owner)", IPC_ERR_INVALID, c_sel_add_foreign_owner},
        {"select_add(already watched)", IPC_OK, c_sel_add_duplicate},
        {"select_add(watch slots full)", IPC_ERR_FULL, c_sel_add_capacity},
        {"select_add(watcher allocation fails)", IPC_ERR_FULL, c_sel_add_watcher_alloc_fail},
        {"select_add(valid)", IPC_OK, c_sel_add_ok},

        {"select_wait(NULL out)", IPC_ERR_INVALID, c_sel_wait_null_out},
        {"select_wait(bad id)", IPC_ERR_INVALID, c_sel_wait_bad_id},
        {"select_wait(foreign owner)", IPC_ERR_INVALID, c_sel_wait_foreign_owner},
        {"select_wait(nothing signalled)", IPC_EMPTY, c_sel_wait_unsignalled},
        {"select_wait(signalled)", IPC_OK, c_sel_wait_ok},

        {"select_listen(NULL endpoints)", IPC_ERR_INVALID, c_listen_null_endpoints},
        {"select_listen(NULL out)", IPC_ERR_INVALID, c_listen_null_out},
        {"select_listen(zero count)", IPC_ERR_INVALID, c_listen_zero_count},
        {"select_listen(propagates FULL)", IPC_ERR_FULL, c_listen_propagates_full},
        {"select_listen(valid)", IPC_OK, c_listen_ok},

        {"select_recv(NULL message)", IPC_ERR_INVALID, c_sel_recv_null_message},
        {"select_recv(bad id)", IPC_ERR_INVALID, c_sel_recv_bad_id},
        {"select_recv(nothing signalled)", IPC_EMPTY, c_sel_recv_unsignalled},
        {"select_recv(endpoint not owned)", IPC_ERR_PERM, c_sel_recv_endpoint_not_owned},
        {"select_recv(valid)", IPC_OK, c_sel_recv_ok},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset_threads();
        contract_env_build();
        int rc = cases[i].run();
        g_checks++;
        if (rc != cases[i].expect) {
            g_failures++;
            printf("  [FAIL] %s: expected %s, got %s (%s:%d)\n", cases[i].what,
                   code_name(cases[i].expect), code_name(rc), __FILE__, __LINE__);
        }
        contract_env_teardown();
    }
}

/* The kernel wrappers are documented as their _from counterparts with
 * IPC_CONTEXT_KERNEL. If one ever grew a check of its own, callers would see
 * two different contracts for the same operation. */
static void test_the_kernel_wrappers_match_their_from_variants(void) {
    uint32_t ctx = fresh_ctx();
    uint32_t msg_ep = 0, note_ep = 0;
    (void)ipc_endpoint_create(ctx, &msg_ep);
    (void)ipc_notification_create(ctx, &note_ep);
    ipc_message_t m = msg_of(IPC_ENDPOINT_NONE, 1u, 1u);
    ipc_message_t got;

    CHECK(ipc_send(BAD_EP, &m) == ipc_send_from(IPC_CONTEXT_KERNEL, BAD_EP, &m),
          "send matches send_from on an unknown endpoint");
    CHECK(ipc_send(note_ep, &m) == ipc_send_from(IPC_CONTEXT_KERNEL, note_ep, &m),
          "send matches send_from on a type mismatch");
    CHECK(ipc_send(msg_ep, &m) == ipc_send_from(IPC_CONTEXT_KERNEL, msg_ep, &m),
          "send matches send_from on the happy path");

    CHECK(ipc_recv(BAD_EP, &got) == ipc_recv_for(IPC_CONTEXT_KERNEL, BAD_EP, &got),
          "recv matches recv_for on an unknown endpoint");
    CHECK(ipc_recv(msg_ep, &got) == ipc_recv_for(IPC_CONTEXT_KERNEL, msg_ep, &got),
          "recv matches recv_for while messages remain");
    CHECK(ipc_recv(msg_ep, &got) == ipc_recv_for(IPC_CONTEXT_KERNEL, msg_ep, &got),
          "recv matches recv_for once drained");

    CHECK(ipc_notify(BAD_EP) == ipc_notify_from(IPC_CONTEXT_KERNEL, BAD_EP),
          "notify matches notify_from on an unknown endpoint");
    CHECK(ipc_notify(msg_ep) == ipc_notify_from(IPC_CONTEXT_KERNEL, msg_ep),
          "notify matches notify_from on a type mismatch");
    CHECK(ipc_notify(note_ep) == ipc_notify_from(IPC_CONTEXT_KERNEL, note_ep),
          "notify matches notify_from on the happy path");

    CHECK(ipc_wait(BAD_EP) == ipc_wait_for(IPC_CONTEXT_KERNEL, BAD_EP),
          "wait matches wait_for on an unknown endpoint");
    CHECK(ipc_wait(note_ep) == ipc_wait_for(IPC_CONTEXT_KERNEL, note_ep),
          "wait matches wait_for while signals remain");
    CHECK(ipc_wait(note_ep) == ipc_wait_for(IPC_CONTEXT_KERNEL, note_ep),
          "wait matches wait_for once drained");

    ipc_endpoints_release_owner(ctx);
}

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"E1 create assigns distinct ids", test_create_assigns_distinct_ids},
        {"E2 create rejects a NULL out param", test_create_rejects_a_null_out_param},
        {"E3 reserved and unknown ids are invalid", test_reserved_and_unknown_ids_are_invalid},
        {"E4 queries reject NULL out params", test_query_rejects_null_out_params},
        {"Q1 queue is FIFO across ring wrap", test_queue_is_fifo_across_ring_wrap},
        {"Q2 queue full is reported and recovers", test_queue_full_is_reported_and_recovers},
        {"Q3 send stamps the destination", test_send_stamps_the_destination},
        {"Q4 send/recv reject NULL payloads", test_send_and_recv_reject_null_payloads},
        {"P1 send requires an owned source", test_send_requires_a_source_the_sender_owns},
        {"P2 recv requires ownership", test_recv_requires_ownership},
        {"T1 message and notification endpoints do not mix",
         test_message_and_notification_endpoints_do_not_mix},
        {"N1 notifications count up and down", test_notifications_count_up_and_down},
        {"W1 non-blocking polls do not arm a waiter", test_non_blocking_polls_do_not_arm_a_waiter},
        {"B1 blocking recv takes a ready message",
         test_blocking_recv_takes_a_ready_message_without_blocking},
        {"B2 blocking recv is served while parked",
         test_blocking_recv_receives_a_message_sent_while_parked},
        {"B3 spurious wake reports EMPTY", test_blocking_recv_reports_a_spurious_wake_as_empty},
        {"B4 blocking recv enforces ownership",
         test_blocking_recv_enforces_ownership_and_arguments},
        {"B5 endpoint_wait does not consume", test_endpoint_wait_does_not_consume_the_message},
        {"B6 endpoint_wait enforces ownership", test_endpoint_wait_enforces_ownership},
        {"R1 release frees only that owner", test_release_owner_frees_only_that_owners_endpoints},
        {"R2 release ignores the kernel context", test_release_owner_ignores_the_kernel_context},
        {"R3 release aborts a blocked receiver", test_release_owner_aborts_a_blocked_receiver},
        {"S1 select create/add/destroy", test_select_create_add_and_destroy},
        {"S2 select rejects bad ids and foreign owners",
         test_select_rejects_bad_ids_and_foreign_owners},
        {"S3 select watch capacity is enforced", test_select_watch_capacity_is_enforced},
        {"S4 select table exhausts and recovers", test_select_table_exhausts_and_recovers},
        {"S5 select listen validates and builds", test_select_listen_validates_and_builds},
        {"S6 select listen does not leak on failure", test_select_listen_does_not_leak_on_failure},
        {"S7 select is signalled by a send",
         test_select_is_signalled_by_a_send_to_a_watched_endpoint},
        {"S8 select wakes a parked waiter", test_select_wakes_a_parked_waiter},
        {"S9 select_recv delivers and reports a lost race",
         test_select_recv_delivers_and_reports_a_lost_race},
        {"S10 select destroy aborts a parked waiter", test_select_destroy_aborts_a_parked_waiter},
        {"S11 a destroyed set stops receiving signals", test_destroyed_set_stops_receiving_signals},
        {"S12 releasing a watched endpoint leaves the set usable",
         test_releasing_a_watched_endpoint_leaves_the_set_usable},
        {"S13 signal tolerates a NULL set", test_select_signal_tolerates_a_null_set},
        {"M1 one send wakes exactly one waiter, FIFO",
         test_one_send_wakes_exactly_one_waiter_in_fifo_order},
        {"M2 a send with no waiter wakes nobody", test_a_send_with_no_waiter_wakes_nobody},
        {"M3 release aborts every waiter", test_release_owner_aborts_every_waiter},
        {"M4 a receiver moving endpoints is in one list",
         test_a_receiver_moving_between_endpoints_is_in_one_list},
        {"Q5 a full queue neither blocks nor wakes", test_a_full_queue_neither_blocks_nor_wakes},
        {"Q6 a burst wakes the parked receiver once",
         test_a_burst_wakes_the_parked_receiver_once_and_fills_the_queue},
        {"Q7 every message bit survives the queue", test_every_message_bit_survives_the_queue},
        {"E5 the table grows past one chunk", test_the_table_grows_past_one_chunk},
        {"E6 freed slots are reused cleanly",
         test_freed_slots_are_reused_without_disturbing_neighbours},
        {"E7 a wrapped id never collides", test_a_wrapped_id_never_collides_with_a_live_endpoint},
        {"E8 creation reports allocation failure",
         test_endpoint_creation_reports_allocation_failure},
        {"N2 the notification counter saturates", test_the_notification_counter_saturates},
        {"N3 interleaved notify/wait keep an exact count",
         test_interleaved_notify_and_wait_keep_an_exact_count},
        {"N4 release invalidates a signalled notification",
         test_release_invalidates_a_signalled_notification},
        {"S14 add reports a failed watcher registration",
         test_select_add_reports_a_failed_watcher_registration},
        {"S15 a duplicate add is a no-op", test_adding_the_same_endpoint_twice_is_a_no_op},
        {"S24 listen tolerates a repeated endpoint", test_listen_tolerates_a_repeated_endpoint},
        {"M5 re-blocking on the same endpoint keeps the list intact",
         test_re_blocking_on_the_same_endpoint_keeps_the_wait_list_intact},
        {"S16 two sets on one endpoint both signal",
         test_two_sets_watching_one_endpoint_are_both_signalled},
        {"S17 a full set reports each endpoint", test_a_full_set_reports_each_of_its_endpoints},
        {"S18 the readiness latch is last-writer-wins",
         test_the_readiness_latch_is_last_writer_wins},
        {"S19 a kernel-owned set may drain a foreign endpoint",
         test_a_kernel_owned_set_may_drain_another_contexts_endpoint},
        {"S20 a set survives losing every endpoint",
         test_a_set_survives_its_owner_losing_every_endpoint},
        {"S21 destroy is idempotent", test_select_destroy_is_idempotent},
        {"S22 listen cleans up a midway failure", test_listen_cleans_up_when_an_add_fails_midway},
        {"S23 a notification endpoint never signals a set",
         test_a_notification_endpoint_never_signals_a_set},
        {"B7 endpoint_wait hands the timeout through",
         test_endpoint_wait_hands_the_timeout_through_unchanged},
        {"B8 a timed select wait expires as EMPTY", test_a_timed_select_wait_expires_as_empty},
        {"X1 every documented error code, by precondition", test_error_code_contract},
        {"X2 the kernel wrappers match their _from variants",
         test_the_kernel_wrappers_match_their_from_variants},
    };

    ipc_init();
    reset_threads();

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[i].name);
        fflush(stdout);
        reset_threads();
        tests[i].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[i].name);
        }
    }
    printf("test_ipc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}