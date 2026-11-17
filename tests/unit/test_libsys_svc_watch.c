/* test_libsys_svc_watch.c - host tests for the WASM libsys service-name watch
 * and await (src/libsys/wasm/svc_watch_wasm.c, contract in wasmos/libsys.h).
 *
 * The watch runs over the real event loop (static inline in libsys.h) and the
 * real future/promise core (coroutine_wasm.c). Off wasm the host-call import
 * attribute expands to nothing, so this file defines the imports: the process
 * manager becomes a scripted inbox the case fills with the replies and events it
 * wants delivered, and every outgoing send is recorded so a case can assert
 * what went on the wire and in which order.
 */

/* The libsys headers first: the in-tree string.h must be parsed before any host
 * header that would turn its declarations into fortify macros. */
#include "wasmos/libsys.h"

#include <stdio.h>

#include "test_shuffle.h"

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

/* ------------------------------------------------- scripted kernel side */

#define LOOP_EP 40
#define PROC_EP 2

#define INBOX_MAX 32
static wasmos_ipc_message_t g_inbox[INBOX_MAX];
static int g_inbox_head, g_inbox_count;
static wasmos_ipc_message_t g_last;

#define SENT_MAX 32
static wasmos_ipc_message_t g_sent[SENT_MAX];
static int g_sent_count;
static int32_t g_send_result;

/* Transfer buffers: one slot per id, the bytes last written, and whether it is
 * still held. */
#define BUF_MAX 8
static uint8_t g_buf[BUF_MAX][128];
static int g_buf_live[BUF_MAX];
static int g_acquire_fail;

static uint32_t g_ticks;

static void push(int32_t type, int32_t request_id, int32_t a0, int32_t a1, int32_t a2, int32_t a3) {
    if (g_inbox_count >= INBOX_MAX) {
        return;
    }
    int idx = (g_inbox_head + g_inbox_count) % INBOX_MAX;
    memset(&g_inbox[idx], 0, sizeof(g_inbox[idx]));
    g_inbox[idx].type = type;
    g_inbox[idx].request_id = request_id;
    g_inbox[idx].arg0 = a0;
    g_inbox[idx].arg1 = a1;
    g_inbox[idx].arg2 = a2;
    g_inbox[idx].arg3 = a3;
    g_inbox_count++;
}

int32_t wasmos_ipc_drain(int32_t endpoint) {
    (void)endpoint;
    if (g_inbox_count == 0) {
        return 0;
    }
    g_last = g_inbox[g_inbox_head];
    g_inbox_head = (g_inbox_head + 1) % INBOX_MAX;
    g_inbox_count--;
    return 1;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    switch (field) {
    case 0:
        return g_last.type;
    case 1:
        return g_last.request_id;
    case 2:
        return g_last.arg0;
    case 3:
        return g_last.arg1;
    case 4:
        return g_last.source;
    case 5:
        return g_last.destination;
    case 6:
        return g_last.arg2;
    default:
        return g_last.arg3;
    }
}

int32_t wasmos_ipc_send(int32_t destination, int32_t source, int32_t type, int32_t request_id,
                        int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    if (g_send_result != 0) {
        return g_send_result;
    }
    if (g_sent_count < SENT_MAX) {
        wasmos_ipc_message_t* m = &g_sent[g_sent_count];
        memset(m, 0, sizeof(*m));
        m->destination = destination;
        m->source = source;
        m->type = type;
        m->request_id = request_id;
        m->arg0 = arg0;
        m->arg1 = arg1;
        m->arg2 = arg2;
        m->arg3 = arg3;
    }
    g_sent_count++;
    return 0;
}

/* No select set, so poll() never parks: it drains what the case queued. */
int32_t wasmos_ipc_select_create(void) {
    return -1;
}
int32_t wasmos_ipc_select_add(int32_t sel, int32_t endpoint) {
    (void)sel;
    (void)endpoint;
    return -1;
}
int32_t wasmos_ipc_select_destroy(int32_t sel) {
    (void)sel;
    return 0;
}
int32_t wasmos_ipc_select_wait(int32_t sel) {
    (void)sel;
    return 0;
}
int32_t wasmos_ipc_select_wait_timeout(int32_t sel, int32_t timeout_ms) {
    (void)sel;
    (void)timeout_ms;
    return WASMOS_TIMEOUT;
}

int32_t wasmos_xfer_buffer_acquire(int32_t size) {
    (void)size;
    if (g_acquire_fail) {
        return -1;
    }
    for (int i = 0; i < BUF_MAX; ++i) {
        if (!g_buf_live[i]) {
            g_buf_live[i] = 1;
            memset(g_buf[i], 0, sizeof(g_buf[i]));
            return i + 10;
        }
    }
    return -1;
}
int32_t wasmos_xfer_buffer_write(int32_t id, const void* src, int32_t len, int32_t offset) {
    int i = id - 10;
    if (i < 0 || i >= BUF_MAX || !g_buf_live[i] || offset + len > (int32_t)sizeof(g_buf[i])) {
        return -1;
    }
    memcpy(&g_buf[i][offset], src, (size_t)len);
    return 0;
}
int32_t wasmos_xfer_buffer_release(int32_t id) {
    int i = id - 10;
    if (i < 0 || i >= BUF_MAX || !g_buf_live[i]) {
        return -1;
    }
    g_buf_live[i] = 0;
    return 0;
}

int32_t wasmos_sched_ticks(void) {
    return (int32_t)g_ticks;
}

/* ------------------------------------------------------------- fixture */

static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_set;

typedef struct {
    int binds;
    int32_t last_endpoint;
    int fails;
    int32_t last_status;
} obs_t;

static obs_t g_obs;
static int g_default_hits;

static void on_bind(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)watch;
    obs_t* o = (obs_t*)user;
    o->binds++;
    o->last_endpoint = endpoint;
}

static void on_fail(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)watch;
    obs_t* o = (obs_t*)user;
    o->fails++;
    o->last_status = status;
}

static void on_default(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
    g_default_hits++;
}

static void fresh(void) {
    memset(g_inbox, 0, sizeof(g_inbox));
    g_inbox_head = g_inbox_count = 0;
    g_sent_count = 0;
    g_send_result = 0;
    memset(g_buf_live, 0, sizeof(g_buf_live));
    g_acquire_fail = 0;
    g_ticks = 1000;
    memset(&g_obs, 0, sizeof(g_obs));
    g_default_hits = 0;
    wasmos_sys_event_loop_init(&g_loop, LOOP_EP, 100);
    wasmos_sys_event_set_default(&g_loop, on_default, 0);
    wasmos_sys_svc_watch_set_init(&g_set, &g_loop, PROC_EP);
}

static int live_buffers(void) {
    int n = 0;
    for (int i = 0; i < BUF_MAX; ++i) {
        n += g_buf_live[i];
    }
    return n;
}

/* Index of the n-th sent message of `type` (0-based), or -1. */
static int sent_of(int32_t type, int nth) {
    for (int i = 0; i < g_sent_count && i < SENT_MAX; ++i) {
        if (g_sent[i].type == type && nth-- == 0) {
            return i;
        }
    }
    return -1;
}

/* Answer the watch's subscribe and its lookup (`lookup_ep` < 0 = not
 * registered), then poll until both are consumed. */
static void answer_subscribe_and_lookup(int32_t lookup_ep) {
    int s = sent_of(SVC_IPC_SUBSCRIBE_NAME_REQ, 0);
    if (s < 0) {
        return; /* never sent: the case's own checks fail on the missing bind */
    }
    push(SVC_IPC_SUBSCRIBE_NAME_RESP, g_sent[s].request_id, 0, 0, 0, 0);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    int l = sent_of(SVC_IPC_LOOKUP_REQ, 0);
    if (l < 0) {
        return;
    }
    push(SVC_IPC_LOOKUP_RESP, g_sent[l].request_id, lookup_ep < 0 ? -1 : lookup_ep, 0, 0, 0);
    wasmos_sys_event_loop_poll(&g_loop, 4);
}

/* ---------------------------------------------------------------- cases */

/* The watch subscribes BEFORE it looks the name up, with a descriptor naming the
 * loop's endpoint, its cookie and the name; the lookup follows only once the
 * subscription is confirmed, and the descriptor buffer is released then. */
static int test_subscribes_then_looks_up(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    CHECK(wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs) == 0,
          "start succeeds");
    CHECK(g_sent_count == 1 && g_sent[0].type == SVC_IPC_SUBSCRIBE_NAME_REQ,
          "only the subscribe is sent first");
    CHECK(g_sent[0].destination == PROC_EP && g_sent[0].source == LOOP_EP, "sent to the PM");
    CHECK(g_sent[0].arg2 == (int32_t)sizeof(svc_name_subscribe_desc_t), "descriptor size");
    const svc_name_subscribe_desc_t* d =
        (const svc_name_subscribe_desc_t*)g_buf[g_sent[0].arg0 - 10];
    CHECK(d->version == WASMOS_SVC_NAME_SUBSCRIBE_DESC_VERSION, "descriptor version");
    CHECK(d->notify_endpoint == LOOP_EP, "events go to the loop's endpoint");
    CHECK(d->cookie == w.cookie && w.cookie != 0, "descriptor carries the watch's cookie");
    CHECK(strcmp(d->name, "kbd") == 0, "descriptor carries the name");
    CHECK(live_buffers() == 1, "descriptor buffer held while the subscribe is in flight");

    push(SVC_IPC_SUBSCRIBE_NAME_RESP, g_sent[0].request_id, 0, 0, 0, 0);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(live_buffers() == 0, "buffer released once subscribed");
    int l = sent_of(SVC_IPC_LOOKUP_REQ, 0);
    CHECK(l == 1, "lookup sent after the subscribe reply");
    int32_t packed[4];
    wasmos_ipc_pack_name16("kbd", packed);
    CHECK(g_sent[l].arg0 == packed[0], "lookup carries the packed name");
    return 0;
}

/* A lookup hit binds at once. */
static int test_lookup_hit_binds(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs);
    answer_subscribe_and_lookup(77);
    CHECK(g_obs.binds == 1 && g_obs.last_endpoint == 77, "bound to the looked-up endpoint");
    CHECK(w.endpoint == 77 && w.active, "watch stays active and records the endpoint");
    return 0;
}

/* A name not registered yet binds on the event that reports its registration;
 * an event carrying another watch's cookie is not this watch's. */
static int test_event_binds_after_a_miss(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs);
    answer_subscribe_and_lookup(-1);
    CHECK(g_obs.binds == 0, "a miss binds nothing");
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)(w.cookie + 1), 90, 9);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(g_obs.binds == 0, "another cookie's event is ignored");
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)w.cookie, 91, 9);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(g_obs.binds == 1 && g_obs.last_endpoint == 91, "own cookie's event binds");
    CHECK(g_default_hits == 0, "events never reach the default handler");
    return 0;
}

/* The same endpoint reported twice -- by the lookup and by the event of the
 * registration that raced it -- is one bind; a different endpoint later is a
 * rebind (a restarted provider). */
static int test_duplicate_is_one_bind_and_new_endpoint_rebinds(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs);
    int s = sent_of(SVC_IPC_SUBSCRIBE_NAME_REQ, 0);
    push(SVC_IPC_SUBSCRIBE_NAME_RESP, g_sent[s].request_id, 0, 0, 0, 0);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)w.cookie, 77, 7);
    int l = sent_of(SVC_IPC_LOOKUP_REQ, 0);
    push(SVC_IPC_LOOKUP_RESP, g_sent[l].request_id, 77, 0, 0, 0);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(g_obs.binds == 1, "event and lookup of the same endpoint bind once");
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)w.cookie, 78, 8);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(g_obs.binds == 2 && g_obs.last_endpoint == 78, "a new endpoint rebinds");
    return 0;
}

/* A refused subscribe fails the watch with the PM's packed code, stops it, and
 * still sends the unsubscribe that makes the stop complete. */
static int test_refused_subscribe_fails_with_the_pm_code(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs);
    push(SVC_IPC_ERROR,
         g_sent[0].request_id,
         SVC_IPC_SUBSCRIBE_NAME_REQ,
         WASMOS_ERR_PROC_PM_SVC_SUB_NO_SPACE,
         0,
         0);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(g_obs.fails == 1 && g_obs.last_status == WASMOS_ERR_PROC_PM_SVC_SUB_NO_SPACE,
          "fails with the code the PM sent");
    CHECK(!w.active, "watch is stopped");
    CHECK(sent_of(SVC_IPC_LOOKUP_REQ, 0) < 0, "no lookup after a refused subscribe");
    CHECK(sent_of(SVC_IPC_UNSUBSCRIBE_NAME_REQ, 0) >= 0, "unsubscribe sent");
    CHECK(live_buffers() == 0, "descriptor buffer released");
    return 0;
}

/* An await resolves with the first endpoint and then stops its watch. */
static int test_await_resolves_and_unsubscribes(void) {
    fresh();
    wasmos_sys_svc_await_t a;
    memset(&a, 0, sizeof(a));
    wasmos_future_t* f = wasmos_sys_svc_await_start(&g_set, &a, "fs.vfs", 0);
    CHECK(f != 0, "await started");
    answer_subscribe_and_lookup(-1);
    int32_t st = 1;
    uintptr_t v = 0;
    CHECK(!wasmos_future_poll(f, &st, &v), "pending until a bind");
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)a.watch.cookie, 55, 5);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(wasmos_future_poll(f, &st, &v) && st == 0 && v == 55, "resolved with the endpoint");
    CHECK(!a.watch.active, "watch stopped after the first bind");
    int u = sent_of(SVC_IPC_UNSUBSCRIBE_NAME_REQ, 0);
    CHECK(u >= 0 && g_sent[u].arg0 == LOOP_EP && g_sent[u].arg1 == (int32_t)a.watch.cookie,
          "unsubscribe names the watch's key");
    return 0;
}

/* A deadline that passes before a bind rejects the await with
 * SVC_AWAIT_TIMEOUT; one checked before it passes does nothing; a bind clears it. */
static int test_await_times_out_only_after_its_deadline(void) {
    fresh();
    wasmos_sys_svc_await_t a;
    memset(&a, 0, sizeof(a));
    wasmos_future_t* f = wasmos_sys_svc_await_start(&g_set, &a, "gfx", 100);
    answer_subscribe_and_lookup(-1);
    int32_t st = 0;
    uintptr_t v = 0;
    g_ticks += (100u * WASMOS_SCHED_TICK_HZ) / 1000u - 1u;
    wasmos_sys_svc_watch_set_expire(&g_set);
    CHECK(!wasmos_future_poll(f, &st, &v), "still pending one tick before the deadline");
    g_ticks += 1;
    wasmos_sys_svc_watch_set_expire(&g_set);
    CHECK(wasmos_future_poll(f, &st, &v) && st == WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT,
          "rejected with the timeout code at the deadline");
    CHECK(!a.watch.active && g_set.head == 0, "timed-out watch left the set");

    fresh();
    memset(&a, 0, sizeof(a));
    f = wasmos_sys_svc_await_start(&g_set, &a, "gfx", 100);
    answer_subscribe_and_lookup(66);
    g_ticks += 10000;
    wasmos_sys_svc_watch_set_expire(&g_set);
    CHECK(wasmos_future_poll(f, &st, &v) && st == 0 && v == 66, "a bind beats the deadline");
    return 0;
}

/* The deadline comparison survives the 32-bit tick counter wrapping, in both
 * directions an unsigned `now >= deadline` gets wrong. */
static int test_deadline_is_wrap_safe(void) {
    const uint32_t window = (200u * WASMOS_SCHED_TICK_HZ) / 1000u;

    /* The deadline itself wraps: a counter still high must not read as past it. */
    fresh();
    g_ticks = 0xFFFFFFF0u;
    wasmos_sys_svc_watch_t w = {0};
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 200, on_bind, on_fail, &g_obs);
    g_ticks = 0xFFFFFFF5u;
    wasmos_sys_svc_watch_set_expire(&g_set);
    CHECK(g_obs.fails == 0 && w.active, "a wrapped deadline is not past for a high counter");
    g_ticks = 0xFFFFFFF0u + window;
    wasmos_sys_svc_watch_set_expire(&g_set);
    CHECK(g_obs.fails == 1 && g_obs.last_status == WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT,
          "expired once the wrapped deadline passes");

    /* The counter wraps past a deadline that did not: it must still expire. */
    fresh();
    g_ticks = 0xFFFFFFFFu - window - 4u;
    memset(&w, 0, sizeof(w));
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 200, on_bind, on_fail, &g_obs);
    g_ticks = 0x10u;
    wasmos_sys_svc_watch_set_expire(&g_set);
    CHECK(g_obs.fails == 1, "a counter that wrapped past the deadline expires it");
    return 0;
}

/* Stopping a watch whose subscribe is still in flight releases its buffer,
 * sends the unsubscribe, and runs no callback when the late reply lands. */
static int test_stop_with_a_request_in_flight(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs);
    int32_t sub_rid = g_sent[0].request_id;
    wasmos_sys_svc_watch_stop(&w);
    CHECK(!w.active && live_buffers() == 0, "stopped and buffer released");
    CHECK(sent_of(SVC_IPC_UNSUBSCRIBE_NAME_REQ, 0) >= 0, "unsubscribe sent");
    push(SVC_IPC_SUBSCRIBE_NAME_RESP, sub_rid, 0, 0, 0, 0);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)w.cookie, 77, 7);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(g_obs.binds == 0 && g_obs.fails == 0, "no callback after stop");
    CHECK(sent_of(SVC_IPC_LOOKUP_REQ, 0) < 0, "no lookup after stop");
    CHECK(g_default_hits == 1, "the late subscribe reply reaches the default handler");
    return 0;
}

/* Start failures leave the record inactive and hold no buffer: a failed send is
 * SVC_WATCH_SEND, a failed stage SVC_WATCH_NO_BUFFER, and invalid arguments -1. */
static int test_start_failures(void) {
    fresh();
    wasmos_sys_svc_watch_t w = {0};
    g_send_result = -3;
    CHECK(wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs) ==
              WASMOS_ERR_PROC_PM_SVC_WATCH_SEND,
          "failed send");
    CHECK(!w.active && live_buffers() == 0 && g_set.head == 0, "nothing held after a failed send");
    g_send_result = 0;
    g_acquire_fail = 1;
    CHECK(wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs) ==
              WASMOS_ERR_PROC_PM_SVC_WATCH_NO_BUFFER,
          "failed stage");
    g_acquire_fail = 0;
    CHECK(wasmos_sys_svc_watch_start(&g_set, &w, "", 0, on_bind, on_fail, &g_obs) == -1,
          "empty name");
    CHECK(wasmos_sys_svc_watch_start(
              &g_set, &w, "seventeen-chars-x", 0, on_bind, on_fail, &g_obs) == -1,
          "name longer than the packed lookup carries");
    CHECK(wasmos_sys_svc_watch_start(&g_set, &w, "sixteen-chars-xx", 0, on_bind, on_fail, &g_obs) ==
              0,
          "a 16-character name is accepted");
    CHECK(wasmos_sys_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &g_obs) == -1,
          "an active record is refused");

    fresh();
    wasmos_sys_svc_await_t a;
    memset(&a, 0, sizeof(a));
    g_send_result = -3;
    wasmos_future_t* f = wasmos_sys_svc_await_start(&g_set, &a, "kbd", 0);
    int32_t st = 0;
    uintptr_t v = 0;
    CHECK(f && wasmos_future_poll(f, &st, &v) && st == WASMOS_ERR_PROC_PM_SVC_WATCH_SEND,
          "an await whose start fails is returned already rejected");
    return 0;
}

/* Two watches on one loop are routed by cookie, each to its own callback. */
static int test_two_watches_share_one_loop(void) {
    fresh();
    obs_t o1 = {0}, o2 = {0};
    wasmos_sys_svc_watch_t w1 = {0}, w2 = {0};
    wasmos_sys_svc_watch_start(&g_set, &w1, "kbd", 0, on_bind, on_fail, &o1);
    wasmos_sys_svc_watch_start(&g_set, &w2, "fb", 0, on_bind, on_fail, &o2);
    CHECK(w1.cookie != w2.cookie, "distinct cookies");
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, (int32_t)w2.cookie, 12, 1);
    wasmos_sys_event_loop_poll(&g_loop, 4);
    CHECK(o2.binds == 1 && o2.last_endpoint == 12 && o1.binds == 0, "routed to the fb watch");
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_subscribes_then_looks_up),
        WASMOS_TEST_CASE(test_lookup_hit_binds),
        WASMOS_TEST_CASE(test_event_binds_after_a_miss),
        WASMOS_TEST_CASE(test_duplicate_is_one_bind_and_new_endpoint_rebinds),
        WASMOS_TEST_CASE(test_refused_subscribe_fails_with_the_pm_code),
        WASMOS_TEST_CASE(test_await_resolves_and_unsubscribes),
        WASMOS_TEST_CASE(test_await_times_out_only_after_its_deadline),
        WASMOS_TEST_CASE(test_deadline_is_wrap_safe),
        WASMOS_TEST_CASE(test_stop_with_a_request_in_flight),
        WASMOS_TEST_CASE(test_start_failures),
        WASMOS_TEST_CASE(test_two_watches_share_one_loop),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    (void)wasmos_test_run_all(cases, groups);
    printf("test_libsys_svc_watch: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
