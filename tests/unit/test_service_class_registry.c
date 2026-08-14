/* Host unit test for the class-based service discovery table
 * (service_class_registry.c). Drives registration, class enumeration,
 * subscriptions/existence events, anti-spoof, dynamic growth, and death
 * reaping through the injected event-sink and liveness callbacks.
 *
 * service_class_registry.c, the three list sources and kmem.c are compiled in
 * for real, with the libc string.c; only the slab allocator underneath kmem is
 * replaced, by tests/unit/stubs_slab.c, which forwards to the host heap — so the
 * negative returns service_class_registry_add gives on allocation failure are
 * not reachable here. The registry's tables are process-global file statics, so
 * every case starts with fresh(); the cases run in a shuffled order.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h). */

#include "service_class_registry.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "test_shuffle.h"

/* --- captured existence events --- */
typedef struct {
    uint32_t notify;
    uint32_t event;
    uint32_t instance;
    uint32_t endpoint;
    uint32_t pid;
    char class_name[SVC_CLASS_NAME_MAX];
} ev_t;

/* Event log, in emission order. Capped at 128 entries; a run that would exceed
 * it drops the excess silently, so a case must not depend on a count past that.
 * Several cases zero g_ev_n mid-case to forget the setup's ADDs. */
static ev_t g_ev[128];
static int g_ev_n;

/* service_class_event_fn sink, standing in for the PM's real one, which pushes
 * an SVC_IPC_CLASS_EVENT to notify_endpoint. The signature reports nothing, so
 * neither sink can refuse an event; this one records the arguments verbatim,
 * copying class_name into a buffer bounded by SVC_CLASS_NAME_MAX. */
static void cap_event(void* user, uint32_t notify, uint32_t event, const char* class_name,
                      uint32_t instance, uint32_t endpoint, uint32_t pid) {
    (void)user;
    if (g_ev_n >= (int)(sizeof(g_ev) / sizeof(g_ev[0]))) {
        return;
    }
    ev_t* e = &g_ev[g_ev_n++];
    e->notify = notify;
    e->event = event;
    e->instance = instance;
    e->endpoint = endpoint;
    e->pid = pid;
    uint32_t i = 0;
    for (; class_name[i] && i + 1u < SVC_CLASS_NAME_MAX; ++i) {
        e->class_name[i] = class_name[i];
    }
    e->class_name[i] = '\0';
}

/* --- controllable liveness predicate --- */
/* Owner contexts a case has declared dead. Empty means everything is alive. */
static uint32_t g_dead[32];
static int g_dead_n;

/* service_class_alive_fn predicate: returns 1 when `ctx` is alive and 0 when a
 * case has marked it dead — non-zero is alive, as the registry expects. It
 * consults a list rather than the process table, so liveness here is whatever
 * the case declares and never changes underneath a reap. */
static int alive_cb(void* user, uint32_t ctx) {
    (void)user;
    for (int i = 0; i < g_dead_n; ++i) {
        if (g_dead[i] == ctx) {
            return 0;
        }
    }
    return 1;
}

/* Declare one owner context dead for the rest of the case. Silently ignored past
 * 32 marks, and there is no way to revive one. */
static void mark_dead(uint32_t ctx) {
    if (g_dead_n < (int)(sizeof(g_dead) / sizeof(g_dead[0]))) {
        g_dead[g_dead_n++] = ctx;
    }
}

/* Fixture reset: empty registry, this file's sink installed, and both logs
 * cleared. Every case starts here, since the registry state is process-global
 * and the cases run in a shuffled order. */
static void fresh(void) {
    service_class_registry_reset();
    service_class_registry_set_event_sink(cap_event, 0);
    g_ev_n = 0;
    g_dead_n = 0;
}

/* Count captured events of a kind. */
static int ev_count(uint32_t event) {
    int n = 0;
    for (int i = 0; i < g_ev_n; ++i) {
        if (g_ev[i].event == event) {
            n++;
        }
    }
    return n;
}

static int test_add_and_lookup(void) {
    fresh();
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 1, 101, 8, 80) != 0)
        return __LINE__;

    service_class_provider_t out[8];
    uint32_t n = service_class_registry_lookup("net.ifc", out, 8);
    if (n != 2)
        return __LINE__;
    /* ascending instance order */
    if (out[0].instance != 0 || out[0].endpoint != 100 || out[0].pid != 70)
        return __LINE__;
    if (out[1].instance != 1 || out[1].endpoint != 101 || out[1].pid != 80)
        return __LINE__;
    return 0;
}

static int test_lookup_unknown_and_empty(void) {
    fresh();
    service_class_provider_t out[4];
    if (service_class_registry_lookup("nope", out, 4) != 0)
        return __LINE__;
    if (service_class_registry_lookup("", out, 4) != 0)
        return __LINE__;
    if (service_class_registry_lookup(0, out, 4) != 0)
        return __LINE__;
    return 0;
}

static int test_lookup_truncates_but_reports_total(void) {
    fresh();
    for (uint32_t i = 0; i < 5; ++i) {
        if (service_class_registry_add("c", i, 200 + i, 1, i) != 0)
            return __LINE__;
    }
    service_class_provider_t out[2];
    out[0].instance = out[1].instance = 0xFFFFFFFF;
    uint32_t n = service_class_registry_lookup("c", out, 2);
    if (n != 5)
        return __LINE__; /* total reported */
    if (out[0].instance != 0 || out[1].instance != 1)
        return __LINE__; /* only 2 written, in order */
    return 0;
}

static int test_reregister_updates_no_event(void) {
    fresh();
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    if (ev_count(SVC_CLASS_EVENT_ADD) != 1)
        return __LINE__;
    /* same owner re-registers instance 0 with a new endpoint: update, no event */
    if (service_class_registry_add("net.ifc", 0, 555, 7, 71) != 0)
        return __LINE__;
    if (ev_count(SVC_CLASS_EVENT_ADD) != 1)
        return __LINE__;
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 1)
        return __LINE__;
    if (out[0].endpoint != 555 || out[0].pid != 71)
        return __LINE__;
    return 0;
}

static int test_antispoof(void) {
    fresh();
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    /* different owner cannot take the same (class, instance) */
    if (service_class_registry_add("net.ifc", 0, 666, 9, 90) != -1)
        return __LINE__;
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 1)
        return __LINE__;
    if (out[0].endpoint != 100)
        return __LINE__; /* original intact */
    return 0;
}

static int test_classes_independent(void) {
    fresh();
    if (service_class_registry_add("net.ifc", 0, 100, 1, 10) != 0)
        return __LINE__;
    if (service_class_registry_add("block.dev", 0, 300, 2, 20) != 0)
        return __LINE__;
    if (service_class_registry_add("block.dev", 1, 301, 2, 21) != 0)
        return __LINE__;
    service_class_provider_t out[8];
    if (service_class_registry_lookup("net.ifc", out, 8) != 1)
        return __LINE__;
    if (out[0].endpoint != 100)
        return __LINE__;
    if (service_class_registry_lookup("block.dev", out, 8) != 2)
        return __LINE__;
    return 0;
}

static int test_bad_class_names(void) {
    fresh();
    if (service_class_registry_add("", 0, 1, 1, 1) != -1)
        return __LINE__;
    if (service_class_registry_add(0, 0, 1, 1, 1) != -1)
        return __LINE__;
    /* SVC_CLASS_NAME_MAX includes the NUL; a name of that many chars does not fit */
    char toolong[SVC_CLASS_NAME_MAX + 4];
    for (uint32_t i = 0; i < sizeof(toolong) - 1; ++i)
        toolong[i] = 'a';
    toolong[sizeof(toolong) - 1] = '\0';
    if (service_class_registry_add(toolong, 0, 1, 1, 1) != -1)
        return __LINE__;
    if (service_class_registry_subscribe(toolong, 1, 1) != -1)
        return __LINE__;
    return 0;
}

static int test_subscribe_event_fields(void) {
    fresh();
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 3, 100, 7, 70) != 0)
        return __LINE__;
    if (g_ev_n != 1)
        return __LINE__;
    if (g_ev[0].event != SVC_CLASS_EVENT_ADD)
        return __LINE__;
    if (g_ev[0].notify != 999)
        return __LINE__;
    if (g_ev[0].instance != 3 || g_ev[0].endpoint != 100 || g_ev[0].pid != 70)
        return __LINE__;
    if (strcmp(g_ev[0].class_name, "net.ifc") != 0)
        return __LINE__;
    return 0;
}

static int test_subscribe_idempotent_and_multi(void) {
    fresh();
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__; /* dup */
    if (service_class_registry_subscribe("net.ifc", 888, 6) != 0)
        return __LINE__; /* 2nd sub */
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    /* one ADD to each distinct subscriber, not three */
    if (ev_count(SVC_CLASS_EVENT_ADD) != 2)
        return __LINE__;
    return 0;
}

static int test_subscribe_class_isolation(void) {
    fresh();
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_add("block.dev", 0, 300, 2, 20) != 0)
        return __LINE__;
    if (g_ev_n != 0)
        return __LINE__; /* not notified for a different class */
    return 0;
}

/* A subscriber that subscribes after providers already exist gets no retroactive
 * ADD; it takes the current set from lookup and only future changes as events. */
static int test_subscribe_is_not_retroactive(void) {
    fresh();
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 1, 101, 8, 80) != 0)
        return __LINE__;
    g_ev_n = 0;
    /* subscribe after the providers already registered */
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (g_ev_n != 0)
        return __LINE__; /* no replay of existing providers */
    /* the current set is available via lookup */
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 2)
        return __LINE__;
    /* a subsequent registration IS delivered to the subscriber */
    if (service_class_registry_add("net.ifc", 2, 102, 9, 90) != 0)
        return __LINE__;
    if (ev_count(SVC_CLASS_EVENT_ADD) != 1)
        return __LINE__;
    if (g_ev[0].instance != 2 || g_ev[0].endpoint != 102)
        return __LINE__;
    return 0;
}

static int test_reap_provider_fires_remove(void) {
    fresh();
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 1, 101, 8, 80) != 0)
        return __LINE__;
    g_ev_n = 0; /* forget the ADDs */

    mark_dead(7); /* owner of instance 0 died */
    service_class_registry_reap_dead(alive_cb, 0);

    if (ev_count(SVC_CLASS_EVENT_REMOVE) != 1)
        return __LINE__;
    if (g_ev[0].instance != 0 || g_ev[0].endpoint != 100)
        return __LINE__;
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 1)
        return __LINE__;
    if (out[0].instance != 1)
        return __LINE__; /* survivor */
    return 0;
}

static int test_reap_dead_subscriber_gets_no_remove(void) {
    fresh();
    /* subscriber owned by ctx 5, provider owned by ctx 7; both die */
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    g_ev_n = 0;
    mark_dead(5);
    mark_dead(7);
    service_class_registry_reap_dead(alive_cb, 0);
    /* dead subscriber dropped first => provider REMOVE reaches nobody */
    if (ev_count(SVC_CLASS_EVENT_REMOVE) != 0)
        return __LINE__;
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 0)
        return __LINE__; /* purged */
    return 0;
}

static int test_no_event_sink(void) {
    fresh();
    service_class_registry_set_event_sink(0, 0);
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    mark_dead(7);
    service_class_registry_reap_dead(alive_cb, 0); /* must not crash without a sink */
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 0)
        return __LINE__;
    return 0;
}

/* The provider table is a dynamic list_t with no per-class cap: 200 providers
 * in one class all register, and lookup enumerates every one of them in
 * instance order. */
static int test_dynamic_growth(void) {
    fresh();
    const uint32_t count = 200;
    for (uint32_t i = 0; i < count; ++i) {
        if (service_class_registry_add("big.class", i, 1000 + i, 3, i) != 0)
            return __LINE__;
    }
    static service_class_provider_t out[256];
    uint32_t n = service_class_registry_lookup("big.class", out, 256);
    if (n != count)
        return __LINE__;
    for (uint32_t i = 0; i < count; ++i) {
        if (out[i].instance != i || out[i].endpoint != 1000 + i)
            return __LINE__;
    }
    return 0;
}

static int test_reset_clears(void) {
    fresh();
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    if (service_class_registry_subscribe("net.ifc", 999, 5) != 0)
        return __LINE__;
    service_class_registry_reset();
    service_class_provider_t out[4];
    if (service_class_registry_lookup("net.ifc", out, 4) != 0)
        return __LINE__;
    /* subscribers cleared too: an add fires no event (sink also cleared) */
    service_class_registry_set_event_sink(cap_event, 0);
    g_ev_n = 0;
    if (service_class_registry_add("net.ifc", 0, 100, 7, 70) != 0)
        return __LINE__;
    if (g_ev_n != 0)
        return __LINE__;
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_add_and_lookup),
        WASMOS_TEST_CASE(test_lookup_unknown_and_empty),
        WASMOS_TEST_CASE(test_lookup_truncates_but_reports_total),
        WASMOS_TEST_CASE(test_reregister_updates_no_event),
        WASMOS_TEST_CASE(test_antispoof),
        WASMOS_TEST_CASE(test_classes_independent),
        WASMOS_TEST_CASE(test_bad_class_names),
        WASMOS_TEST_CASE(test_subscribe_event_fields),
        WASMOS_TEST_CASE(test_subscribe_idempotent_and_multi),
        WASMOS_TEST_CASE(test_subscribe_class_isolation),
        WASMOS_TEST_CASE(test_subscribe_is_not_retroactive),
        WASMOS_TEST_CASE(test_reap_provider_fires_remove),
        WASMOS_TEST_CASE(test_reap_dead_subscriber_gets_no_remove),
        WASMOS_TEST_CASE(test_no_event_sink),
        WASMOS_TEST_CASE(test_dynamic_growth),
        WASMOS_TEST_CASE(test_reset_clears),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    if (wasmos_test_run_all(cases, groups) != 0) {
        return 1;
    }
    printf("test_service_class_registry: %d groups passed\n", groups);
    return 0;
}
