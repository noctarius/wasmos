/* Host unit test for name-binding subscriptions (service_name_watch.c): the
 * table behind SVC_IPC_SUBSCRIBE_NAME_REQ and SVC_IPC_NAME_EVENT. Drives
 * subscribe / unsubscribe / bound through the injected event-sink and liveness
 * callbacks.
 *
 * service_name_watch.c, the three list sources and kmem.c are compiled in for
 * real, with the libc string.c; only the slab allocator underneath kmem is
 * replaced, by tests/unit/stubs_slab.c, which forwards to the host heap. The
 * table is a process-global file static, so every case starts with fresh(); the
 * cases run in a shuffled order.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h). */

#include "service_name_watch.h"

#include <stdint.h>
#include <stdio.h>

#include "test_shuffle.h"

typedef struct {
    uint32_t notify;
    uint32_t event;
    uint32_t cookie;
    uint32_t endpoint;
    uint32_t pid;
} ev_t;

/* Event log, in emission order; excess past 64 entries is dropped. */
static ev_t g_ev[64];
static int g_ev_n;

static void cap_event(void* user, uint32_t notify, uint32_t event, uint32_t cookie,
                      uint32_t endpoint, uint32_t pid) {
    (void)user;
    if (g_ev_n >= (int)(sizeof(g_ev) / sizeof(g_ev[0]))) {
        return;
    }
    ev_t* e = &g_ev[g_ev_n++];
    e->notify = notify;
    e->event = event;
    e->cookie = cookie;
    e->endpoint = endpoint;
    e->pid = pid;
}

/* Owner contexts a case has declared dead. Empty means everything is alive. */
static uint32_t g_dead[16];
static int g_dead_n;

static int alive_cb(void* user, uint32_t ctx) {
    (void)user;
    for (int i = 0; i < g_dead_n; ++i) {
        if (g_dead[i] == ctx) {
            return 0;
        }
    }
    return 1;
}

static void mark_dead(uint32_t ctx) {
    if (g_dead_n < (int)(sizeof(g_dead) / sizeof(g_dead[0]))) {
        g_dead[g_dead_n++] = ctx;
    }
}

static void fresh(void) {
    service_name_watch_reset();
    service_name_watch_set_event_sink(cap_event, 0);
    g_ev_n = 0;
    g_dead_n = 0;
}

/* A bind reaches the subscriber with the subscriber's own cookie and the
 * provider's endpoint and pid. */
static int test_bind_reaches_subscriber_with_its_cookie(void) {
    fresh();
    if (service_name_watch_subscribe("kbd", 500, 0xC0FFEE, 5) != 0)
        return __LINE__;
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 1)
        return __LINE__;
    if (g_ev[0].notify != 500 || g_ev[0].event != SVC_NAME_EVENT_BIND ||
        g_ev[0].cookie != 0xC0FFEE || g_ev[0].endpoint != 42 || g_ev[0].pid != 7)
        return __LINE__;
    return 0;
}

/* One endpoint can watch several names, told apart by cookie; a bind of one name
 * reaches only the subscription for that name. */
static int test_one_endpoint_watches_several_names(void) {
    fresh();
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("fb", 500, 2, 5) != 0)
        return __LINE__;
    service_name_watch_bound("fb", 43, 8, alive_cb, 0);
    if (g_ev_n != 1 || g_ev[0].cookie != 2 || g_ev[0].endpoint != 43)
        return __LINE__;
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 2 || g_ev[1].cookie != 1 || g_ev[1].endpoint != 42)
        return __LINE__;
    return 0;
}

/* Every subscriber of a name receives the bind; subscribers of other names, and
 * a name sharing only a prefix, do not. */
static int test_bind_fans_out_to_exact_name_only(void) {
    fresh();
    if (service_name_watch_subscribe("fs", 500, 1, 5) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("fs.vfs", 501, 1, 6) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("fs.vfs", 502, 9, 7) != 0)
        return __LINE__;
    service_name_watch_bound("fs.vfs", 60, 3, alive_cb, 0);
    if (g_ev_n != 2)
        return __LINE__;
    if (!((g_ev[0].notify == 501 && g_ev[1].notify == 502) ||
          (g_ev[0].notify == 502 && g_ev[1].notify == 501)))
        return __LINE__;
    return 0;
}

/* Subscribing does not replay: a bind that happened before the subscription is
 * not reported, which is why a client looks the name up after subscribing. */
static int test_subscribe_is_not_retroactive(void) {
    fresh();
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    if (g_ev_n != 0)
        return __LINE__;
    return 0;
}

/* After unsubscribe the key receives nothing; unsubscribing an absent key, or
 * the same key twice, is not an error. */
static int test_unsubscribe_stops_events_and_is_idempotent(void) {
    fresh();
    service_name_watch_unsubscribe(500, 1);
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    service_name_watch_unsubscribe(500, 1);
    service_name_watch_unsubscribe(500, 1);
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 0 || service_name_watch_count() != 0)
        return __LINE__;
    return 0;
}

/* Unsubscribe is keyed by (endpoint, cookie): the same endpoint's other cookie
 * survives. */
static int test_unsubscribe_is_keyed_by_cookie(void) {
    fresh();
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("kbd", 500, 2, 5) != 0)
        return __LINE__;
    service_name_watch_unsubscribe(500, 1);
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 1 || g_ev[0].cookie != 2)
        return __LINE__;
    return 0;
}

/* Subscribing an existing key re-targets it instead of adding a second entry. */
static int test_resubscribe_retargets_the_key(void) {
    fresh();
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("fb", 500, 1, 5) != 0)
        return __LINE__;
    if (service_name_watch_count() != 1)
        return __LINE__;
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 0)
        return __LINE__;
    service_name_watch_bound("fb", 43, 8, alive_cb, 0);
    if (g_ev_n != 1 || g_ev[0].endpoint != 43)
        return __LINE__;
    return 0;
}

/* A key whose endpoint id has passed to a new owner is taken over, not refused:
 * the PM has verified the new caller owns the endpoint, so the old entry can
 * only be a dead process's leftover. */
static int test_new_owner_of_an_endpoint_takes_over_the_key(void) {
    fresh();
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    mark_dead(5);
    if (service_name_watch_subscribe("fb", 500, 1, 6) != 0)
        return __LINE__;
    service_name_watch_bound("fb", 43, 8, alive_cb, 0);
    if (g_ev_n != 1 || g_ev[0].notify != 500)
        return __LINE__;
    return 0;
}

/* A subscriber whose owner died receives no event and is reclaimed by the bind
 * that would have addressed it; a live subscriber of the same name still hears
 * the bind. */
static int test_bind_reclaims_dead_subscribers(void) {
    fresh();
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("kbd", 501, 1, 6) != 0)
        return __LINE__;
    if (service_name_watch_subscribe("fb", 502, 1, 5) != 0)
        return __LINE__;
    mark_dead(5);
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 1 || g_ev[0].notify != 501)
        return __LINE__;
    /* Only the dead subscriber of the bound name is reclaimed; its "fb"
     * subscription waits for a bind of "fb". */
    if (service_name_watch_count() != 2)
        return __LINE__;
    return 0;
}

/* Empty, NULL and over-long names are refused rather than truncated. */
static int test_bad_names_are_refused(void) {
    fresh();
    if (service_name_watch_subscribe("", 500, 1, 5) >= 0)
        return __LINE__;
    if (service_name_watch_subscribe(0, 500, 1, 5) >= 0)
        return __LINE__;
    char longname[SVC_WATCH_NAME_MAX + 1];
    for (uint32_t i = 0; i < SVC_WATCH_NAME_MAX; ++i) {
        longname[i] = 'a';
    }
    longname[SVC_WATCH_NAME_MAX] = '\0';
    if (service_name_watch_subscribe(longname, 500, 1, 5) >= 0)
        return __LINE__;
    if (service_name_watch_count() != 0)
        return __LINE__;
    /* The longest name that fits, terminator included, is accepted. */
    longname[SVC_WATCH_NAME_MAX - 1] = '\0';
    if (service_name_watch_subscribe(longname, 500, 1, 5) != 0)
        return __LINE__;
    return 0;
}

/* With no sink installed, binds record nothing and still reclaim the dead. */
static int test_no_event_sink(void) {
    fresh();
    service_name_watch_set_event_sink(0, 0);
    if (service_name_watch_subscribe("kbd", 500, 1, 5) != 0)
        return __LINE__;
    mark_dead(5);
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 0 || service_name_watch_count() != 0)
        return __LINE__;
    return 0;
}

/* The table grows past any fixed size: many subscribers all receive the bind. */
static int test_dynamic_growth(void) {
    fresh();
    for (uint32_t i = 0; i < 48; ++i) {
        if (service_name_watch_subscribe("kbd", 1000 + i, i, 5) != 0)
            return __LINE__;
    }
    service_name_watch_bound("kbd", 42, 7, alive_cb, 0);
    if (g_ev_n != 48)
        return __LINE__;
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_bind_reaches_subscriber_with_its_cookie),
        WASMOS_TEST_CASE(test_one_endpoint_watches_several_names),
        WASMOS_TEST_CASE(test_bind_fans_out_to_exact_name_only),
        WASMOS_TEST_CASE(test_subscribe_is_not_retroactive),
        WASMOS_TEST_CASE(test_unsubscribe_stops_events_and_is_idempotent),
        WASMOS_TEST_CASE(test_unsubscribe_is_keyed_by_cookie),
        WASMOS_TEST_CASE(test_resubscribe_retargets_the_key),
        WASMOS_TEST_CASE(test_new_owner_of_an_endpoint_takes_over_the_key),
        WASMOS_TEST_CASE(test_bind_reclaims_dead_subscribers),
        WASMOS_TEST_CASE(test_bad_names_are_refused),
        WASMOS_TEST_CASE(test_no_event_sink),
        WASMOS_TEST_CASE(test_dynamic_growth),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    if (wasmos_test_run_all(cases, groups) != 0) {
        return 1;
    }
    printf("test_service_name_watch: %d groups passed\n", groups);
    return 0;
}
