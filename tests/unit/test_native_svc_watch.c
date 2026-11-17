/* Runtime-behaviour tests for the native service-name watch and await
 * (src/libsys/native/svc_watch_native.c). The contract is the WASM one, which
 * tests/unit/test_libsys_svc_watch.c covers in full; this file pins that the
 * native variant keeps it: the order of requests, binding by lookup and by
 * event, dedupe and rebind, the PM's failure code, stop with a request in
 * flight, and an await's resolve and wrap-safe timeout.
 *
 * Only the wasmos_driver_api_t table is stubbed; the event loop, the watch and
 * the future core are the real sources. api->ipc_recv drains a scripted inbox
 * and reports "empty" when it runs out, as a non-blocking receive does.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos/libsys_native.h"

#define LOOP_EP 40u
#define PROC_EP 2u

#define INBOX_MAX 16
static nd_ipc_message_t g_inbox[INBOX_MAX];
static int g_inbox_head, g_inbox_count;

#define SENT_MAX 16
static nd_ipc_message_t g_sent[SENT_MAX];
static int g_sent_count;

static uint8_t g_buf[256];
static int g_buf_live;
static uint32_t g_ticks;
static int g_default_hits;

static int fake_ipc_send(uint32_t ctx, uint32_t endpoint, const nd_ipc_message_t* m) {
    (void)ctx;
    (void)endpoint;
    if (g_sent_count < SENT_MAX) {
        g_sent[g_sent_count] = *m;
    }
    g_sent_count++;
    return 0;
}

static int fake_ipc_recv(uint32_t ctx, uint32_t endpoint, nd_ipc_message_t* out) {
    (void)ctx;
    (void)endpoint;
    if (g_inbox_count == 0) {
        return 1;
    }
    *out = g_inbox[g_inbox_head];
    g_inbox_head = (g_inbox_head + 1) % INBOX_MAX;
    g_inbox_count--;
    return 0;
}

static uint32_t fake_current_pid(void) {
    return 9u;
}

static uint32_t fake_ticks(void) {
    return g_ticks;
}

static void* fake_acquire(uint32_t kind, uint32_t size, uint32_t* out_id) {
    (void)kind;
    if (g_buf_live || size > sizeof(g_buf)) {
        return NULL;
    }
    g_buf_live = 1;
    *out_id = 31u;
    return g_buf;
}

static int fake_release(uint32_t id) {
    if (id != 31u || !g_buf_live) {
        return -1;
    }
    g_buf_live = 0;
    return 0;
}

static void push(uint32_t type, uint32_t request_id, uint32_t a0, uint32_t a1, uint32_t a2) {
    int idx = (g_inbox_head + g_inbox_count) % INBOX_MAX;
    g_inbox[idx] = (nd_ipc_message_t){
        .type = type, .request_id = request_id, .arg0 = a0, .arg1 = a1, .arg2 = a2};
    g_inbox_count++;
}

static wasmos_driver_api_t g_api;
static wasmos_sys_native_event_loop_t g_loop;
static wasmos_sys_native_svc_watch_set_t g_set;

typedef struct {
    int binds;
    uint32_t endpoint;
    int fails;
    int32_t status;
} obs_t;

static void on_bind(void* user, wasmos_sys_native_svc_watch_t* w, uint32_t endpoint) {
    (void)w;
    obs_t* o = (obs_t*)user;
    o->binds++;
    o->endpoint = endpoint;
}

static void on_fail(void* user, wasmos_sys_native_svc_watch_t* w, int32_t status) {
    (void)w;
    obs_t* o = (obs_t*)user;
    o->fails++;
    o->status = status;
}

static void on_default(void* user, const nd_ipc_message_t* m) {
    (void)user;
    (void)m;
    g_default_hits++;
}

static void fresh(void) {
    g_inbox_head = g_inbox_count = 0;
    g_sent_count = 0;
    g_buf_live = 0;
    g_ticks = 1000u;
    g_default_hits = 0;
    g_api = (wasmos_driver_api_t){0};
    g_api.ipc_send = fake_ipc_send;
    g_api.ipc_recv = fake_ipc_recv;
    g_api.sched_current_pid = fake_current_pid;
    g_api.sched_ticks = fake_ticks;
    g_api.xfer_buffer_acquire = fake_acquire;
    g_api.xfer_buffer_release = fake_release;
    wasmos_sys_native_event_loop_init(&g_loop, &g_api, LOOP_EP, 100u);
    wasmos_sys_native_event_set_default(&g_loop, on_default, NULL);
    wasmos_sys_native_svc_watch_set_init(&g_set, &g_loop, PROC_EP);
}

static int sent_of(uint32_t type, int nth) {
    for (int i = 0; i < g_sent_count && i < SENT_MAX; ++i) {
        if (g_sent[i].type == type && nth-- == 0) {
            return i;
        }
    }
    return -1;
}

/* A request that was never sent is not answered; the case's own checks then
 * fail on the missing bind. */
static void answer(uint32_t lookup_ep) {
    int s = sent_of(SVC_IPC_SUBSCRIBE_NAME_REQ, 0);
    if (s < 0) {
        return;
    }
    push(SVC_IPC_SUBSCRIBE_NAME_RESP, g_sent[s].request_id, 0, 0, 0);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
    int l = sent_of(SVC_IPC_LOOKUP_REQ, 0);
    if (l < 0) {
        return;
    }
    push(SVC_IPC_LOOKUP_RESP, g_sent[l].request_id, lookup_ep, 0, 0);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
}

static int test_order_descriptor_and_lookup_bind(void) {
    fresh();
    obs_t o = {0};
    wasmos_sys_native_svc_watch_t w = {0};
    if (wasmos_sys_native_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &o) != 0)
        return __LINE__;
    if (g_sent_count != 1 || g_sent[0].type != SVC_IPC_SUBSCRIBE_NAME_REQ ||
        g_sent[0].destination != PROC_EP || g_sent[0].arg0 != 31u ||
        g_sent[0].arg2 != sizeof(svc_name_subscribe_desc_t))
        return __LINE__;
    const svc_name_subscribe_desc_t* d = (const svc_name_subscribe_desc_t*)g_buf;
    if (d->version != WASMOS_SVC_NAME_SUBSCRIBE_DESC_VERSION || d->notify_endpoint != LOOP_EP ||
        d->cookie != w.cookie || strcmp(d->name, "kbd") != 0)
        return __LINE__;
    answer(77u);
    if (g_buf_live || sent_of(SVC_IPC_LOOKUP_REQ, 0) != 1)
        return __LINE__;
    if (o.binds != 1 || o.endpoint != 77u || !w.active)
        return __LINE__;
    return 0;
}

static int test_event_bind_dedupe_and_rebind(void) {
    fresh();
    obs_t o = {0};
    wasmos_sys_native_svc_watch_t w = {0};
    wasmos_sys_native_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &o);
    answer(0xFFFFFFFFu);
    if (o.binds != 0)
        return __LINE__;
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie + 1u, 90u);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 91u);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 91u);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 92u);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 8u);
    if (o.binds != 2 || o.endpoint != 92u)
        return __LINE__;
    /* Endpoint 0 is a valid endpoint, not "unbound". */
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 0u);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
    if (o.binds != 3 || o.endpoint != 0u)
        return __LINE__;
    return 0;
}

static int test_forget_reports_a_rebind_to_the_same_endpoint(void) {
    fresh();
    obs_t o = {0};
    wasmos_sys_native_svc_watch_t w = {0};
    wasmos_sys_native_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &o);
    answer(77u);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 77u);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
    if (o.binds != 1)
        return __LINE__;
    wasmos_sys_native_svc_watch_forget(&w);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 77u);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
    if (o.binds != 2 || !w.active)
        return __LINE__;
    return 0;
}

static int test_refused_subscribe_carries_the_pm_code(void) {
    fresh();
    obs_t o = {0};
    wasmos_sys_native_svc_watch_t w = {0};
    wasmos_sys_native_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &o);
    push(SVC_IPC_ERROR,
         g_sent[0].request_id,
         SVC_IPC_SUBSCRIBE_NAME_REQ,
         (uint32_t)WASMOS_ERR_PROC_PM_SVC_SUB_NO_SPACE,
         0);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
    if (o.fails != 1 || o.status != WASMOS_ERR_PROC_PM_SVC_SUB_NO_SPACE || w.active || g_buf_live)
        return __LINE__;
    if (sent_of(SVC_IPC_UNSUBSCRIBE_NAME_REQ, 0) < 0 || sent_of(SVC_IPC_LOOKUP_REQ, 0) >= 0)
        return __LINE__;
    return 0;
}

static int test_stop_with_subscribe_in_flight(void) {
    fresh();
    obs_t o = {0};
    wasmos_sys_native_svc_watch_t w = {0};
    wasmos_sys_native_svc_watch_start(&g_set, &w, "kbd", 0, on_bind, on_fail, &o);
    uint32_t rid = g_sent[0].request_id;
    wasmos_sys_native_svc_watch_stop(&w);
    if (w.active || g_buf_live || sent_of(SVC_IPC_UNSUBSCRIBE_NAME_REQ, 0) < 0)
        return __LINE__;
    push(SVC_IPC_SUBSCRIBE_NAME_RESP, rid, 0, 0, 0);
    push(SVC_IPC_NAME_EVENT, 0, SVC_NAME_EVENT_BIND, w.cookie, 77u);
    (void)wasmos_sys_native_event_loop_poll(&g_loop, 4u);
    if (o.binds != 0 || o.fails != 0 || sent_of(SVC_IPC_LOOKUP_REQ, 0) >= 0)
        return __LINE__;
    if (g_default_hits != 1)
        return __LINE__;
    return 0;
}

static int test_await_resolves(void) {
    fresh();
    wasmos_sys_native_svc_await_t a;
    memset(&a, 0, sizeof(a));
    wasmos_future_t* f = wasmos_sys_native_svc_await_start(&g_set, &a, "fs.vfs", 0);
    int32_t st = 1;
    uintptr_t v = 0;
    if (!f)
        return __LINE__;
    answer(55u);
    if (!wasmos_future_poll(f, &st, &v) || st != 0 || v != 55u || a.watch.active)
        return __LINE__;
    return 0;
}

static int test_await_timeout_is_wrap_safe(void) {
    const uint32_t window = (100u * WASMOS_SCHED_TICK_HZ) / 1000u;
    fresh();
    g_ticks = 0xFFFFFFFFu - window - 4u;
    wasmos_sys_native_svc_await_t a;
    memset(&a, 0, sizeof(a));
    wasmos_future_t* f = wasmos_sys_native_svc_await_start(&g_set, &a, "gfx", 100);
    int32_t st = 0;
    uintptr_t v = 0;
    answer(0xFFFFFFFFu);
    g_ticks += window - 1u;
    wasmos_sys_native_svc_watch_set_expire(&g_set);
    if (wasmos_future_poll(f, &st, &v))
        return __LINE__;
    g_ticks = 0x10u; /* wrapped past the deadline */
    wasmos_sys_native_svc_watch_set_expire(&g_set);
    if (!wasmos_future_poll(f, &st, &v) || st != WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT)
        return __LINE__;
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_order_descriptor_and_lookup_bind),
        WASMOS_TEST_CASE(test_event_bind_dedupe_and_rebind),
        WASMOS_TEST_CASE(test_refused_subscribe_carries_the_pm_code),
        WASMOS_TEST_CASE(test_stop_with_subscribe_in_flight),
        WASMOS_TEST_CASE(test_await_resolves),
        WASMOS_TEST_CASE(test_await_timeout_is_wrap_safe),
        WASMOS_TEST_CASE(test_forget_reports_a_rebind_to_the_same_endpoint),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    if (wasmos_test_run_all(cases, groups) != 0) {
        return 1;
    }
    printf("test_native_svc_watch: %d groups passed\n", groups);
    return 0;
}
