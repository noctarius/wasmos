/* test_libsys_event_loop.c — host tests for the libsys service event loop.
 *
 * wasmos_sys_event_loop_* is the dispatch core every WASM service runs: it
 * correlates replies to outstanding intents, routes unsolicited traffic to
 * type handlers, and falls back to a default. The documented rule -- "this
 * gives replies priority over unsolicited traffic" -- lives only in
 * docs/architecture/09-process-and-ipc.md and in this header's control flow;
 * nothing checked it.
 *
 * The header is entirely static-inline over the WASM hostcall imports, and off
 * wasm the import attribute is a no-op, so the host build just links its own
 * definitions of those imports. That makes the kernel side a scripted queue and
 * leaves the dispatch logic itself genuinely under test.
 */

/* The libsys headers must come first: our string.h declares strcpy, and the
 * host's <string.h> turns it into a fortify macro, so pulling the host header
 * in first makes that declaration fail to parse. memset/strcmp are declared by
 * our string.h and resolved by the host libc at link. */
#include "wasmos/libsys.h"

#include <stdio.h>

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

#define INBOX_MAX 32
static wasmos_ipc_message_t g_inbox[INBOX_MAX];
static int g_inbox_head, g_inbox_count;
static wasmos_ipc_message_t g_last; /* what read_last reports */

/* Outgoing sends, so a test can assert what actually went on the wire. */
#define SENT_MAX 32
static wasmos_ipc_message_t g_sent[SENT_MAX];
static int g_sent_count;
static int32_t g_send_result; /* forced return of wasmos_ipc_send */

static int32_t g_select_create_result = 1;
static int32_t g_select_add_result = 0;
static int g_select_wait_calls;
static int g_select_destroy_calls;
/* Messages delivered by the blocking wait, i.e. traffic that only shows up
 * once the loop actually blocks. */
static int g_deliver_on_wait;

static void inbox_push(int32_t type, int32_t request_id, int32_t arg0) {
    if (g_inbox_count >= INBOX_MAX) {
        return;
    }
    int idx = (g_inbox_head + g_inbox_count) % INBOX_MAX;
    memset(&g_inbox[idx], 0, sizeof(g_inbox[idx]));
    g_inbox[idx].type = type;
    g_inbox[idx].request_id = request_id;
    g_inbox[idx].arg0 = arg0;
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

int32_t wasmos_ipc_select_create(void) {
    return g_select_create_result;
}
int32_t wasmos_ipc_select_add(int32_t sel, int32_t endpoint) {
    (void)sel;
    (void)endpoint;
    return g_select_add_result;
}
int32_t wasmos_ipc_select_destroy(int32_t sel) {
    (void)sel;
    g_select_destroy_calls++;
    return 0;
}
int32_t wasmos_ipc_select_wait(int32_t sel) {
    (void)sel;
    g_select_wait_calls++;
    if (g_deliver_on_wait > 0) {
        g_deliver_on_wait--;
        inbox_push(0x7000, 0, 0xBB);
    }
    return 0;
}

/* Reached by other parts of the header; never by the dispatch paths tested. */
int32_t wasmos_ipc_create_endpoint(void) {
    return 5;
}
int32_t wasmos_ipc_select_one(int32_t endpoint) {
    (void)endpoint;
    return 0;
}
int32_t wasmos_ipc_yield(void) {
    return 0;
}

/* -------------------------------------------------------------- observers */

#define TRACE_MAX 32
static struct {
    const char* who;
    int32_t type;
    int32_t request_id;
    int32_t arg0;
} g_trace[TRACE_MAX];
static int g_trace_count;

static void trace(const char* who, const wasmos_ipc_message_t* m) {
    if (g_trace_count < TRACE_MAX) {
        g_trace[g_trace_count].who = who;
        g_trace[g_trace_count].type = m ? m->type : -1;
        g_trace[g_trace_count].request_id = m ? m->request_id : -1;
        g_trace[g_trace_count].arg0 = m ? m->arg0 : -1;
    }
    g_trace_count++;
}

static void on_intent(void* user, const wasmos_ipc_message_t* m) {
    trace(user ? (const char*)user : "intent", m);
}
static void on_handler(void* user, const wasmos_ipc_message_t* m) {
    trace(user ? (const char*)user : "handler", m);
}
static void on_default(void* user, const wasmos_ipc_message_t* m) {
    trace(user ? (const char*)user : "default", m);
}

static void reset(void) {
    memset(g_inbox, 0, sizeof(g_inbox));
    g_inbox_head = 0;
    g_inbox_count = 0;
    memset(&g_last, 0, sizeof(g_last));
    memset(g_sent, 0, sizeof(g_sent));
    g_sent_count = 0;
    g_send_result = 0;
    g_select_create_result = 1;
    g_select_add_result = 0;
    g_select_wait_calls = 0;
    g_select_destroy_calls = 0;
    g_deliver_on_wait = 0;
    g_trace_count = 0;
    memset(g_trace, 0, sizeof(g_trace));
}

static int traced(const char* who) {
    int n = 0;
    for (int i = 0; i < g_trace_count && i < TRACE_MAX; ++i) {
        if (g_trace[i].who && strcmp(g_trace[i].who, who) == 0) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ tests */

static void test_init_builds_a_select_set(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 7, 100);
    CHECK(loop.receiver_endpoint == 7, "the receiver endpoint is recorded");
    CHECK(loop.next_request_id == 100, "the request-id base is honoured");
    CHECK(loop.select_id == 1, "a select set is created and kept");

    /* A set that cannot watch the endpoint is worse than none: it would block
     * forever on something it is not watching. It must be destroyed, not kept. */
    reset();
    g_select_add_result = -1;
    wasmos_sys_event_loop_init(&loop, 7, 1);
    CHECK(loop.select_id == -1, "a set that could not watch the endpoint is discarded");
    CHECK(g_select_destroy_calls == 1, "and destroyed rather than leaked");

    /* No endpoint means no set at all. */
    reset();
    wasmos_sys_event_loop_init(&loop, -1, 1);
    CHECK(loop.select_id == -1, "a loop with no receiver endpoint has no select set");
}

/* The documented priority rule: a reply resolves its intent even when a
 * handler is registered for the same message type. Without it, a service that
 * handles type X and also calls out with type X eats its own reply. */
static void test_an_intent_wins_over_a_type_handler(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 500);
    wasmos_sys_event_register(&loop, 0x4242, on_handler, (void*)"handler");
    wasmos_sys_event_set_default(&loop, on_default, (void*)"default");

    int32_t rid = 0;
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 0x4242, 1, 2, 3, 4, on_intent, (void*)"intent",
                                 &rid) == 0,
          "the intent is sent");
    CHECK(rid == 500, "and takes the seeded request id");
    CHECK(g_sent_count == 1 && g_sent[0].request_id == 500 && g_sent[0].destination == 9,
          "the request really went out, correlated");

    inbox_push(0x4242, rid, 0x77);
    CHECK(wasmos_sys_event_loop_poll(&loop, 4) == 1, "one message is handled");
    CHECK(traced("intent") == 1, "the intent callback ran");
    CHECK(traced("handler") == 0, "the same-type handler did not");
    CHECK(traced("default") == 0, "nor the default");
    CHECK(g_trace[0].arg0 == 0x77, "and it received the reply payload");

    /* The intent slot is released, so the same reply arriving twice is not
     * delivered twice. */
    reset();
    inbox_push(0x4242, rid, 0x77);
    CHECK(wasmos_sys_event_loop_poll(&loop, 4) == 1, "a repeat of the same reply is consumed");
    CHECK(traced("intent") == 0, "but the resolved intent does not fire again");
    CHECK(traced("handler") == 1, "it falls through to the type handler instead");
}

static void test_unmatched_replies_fall_through_to_handler_then_default(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);
    wasmos_sys_event_register(&loop, 0x10, on_handler, (void*)"handler");
    wasmos_sys_event_set_default(&loop, on_default, (void*)"default");

    inbox_push(0x10, 999, 1); /* a request_id no intent is waiting for */
    inbox_push(0x11, 999, 2); /* and a type nothing handles */
    CHECK(wasmos_sys_event_loop_poll(&loop, 4) == 2, "both messages are handled");
    CHECK(traced("handler") == 1, "the known type goes to its handler");
    CHECK(traced("default") == 1, "the unknown type goes to the default");

    /* With no default registered, an unroutable message is still consumed --
     * it must not wedge the loop. */
    reset();
    wasmos_sys_event_loop_t bare;
    wasmos_sys_event_loop_init(&bare, 1, 1);
    inbox_push(0x99, 1, 1);
    CHECK(wasmos_sys_event_loop_poll(&bare, 4) == 1,
          "an unroutable message is consumed rather than left to block the loop");
    CHECK(g_trace_count == 0, "and nothing was dispatched");
}

static void test_the_budget_bounds_the_work_per_poll(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);
    wasmos_sys_event_set_default(&loop, on_default, (void*)"default");
    for (int i = 0; i < 10; ++i) {
        inbox_push(0x20, 0, i);
    }

    CHECK(wasmos_sys_event_loop_poll(&loop, 3) == 3, "a budget of three handles three");
    CHECK(traced("default") == 3, "three dispatches");
    CHECK(g_inbox_count == 7, "and the rest stay queued for the next poll");

    /* Zero is coerced to one, so a caller cannot accidentally build a poll
     * that makes no progress. */
    g_trace_count = 0;
    CHECK(wasmos_sys_event_loop_poll(&loop, 0) == 1, "a budget of zero still handles one");
    CHECK(g_inbox_count == 6, "exactly one was taken");
}

/* The no-busy-poll rule: an empty endpoint must reach the blocking wait rather
 * than spin. */
static void test_an_empty_endpoint_blocks_instead_of_spinning(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);
    wasmos_sys_event_set_default(&loop, on_default, (void*)"default");

    CHECK(wasmos_sys_event_loop_poll(&loop, 4) == 0, "an empty poll returns having handled none");
    CHECK(g_select_wait_calls == 1, "having blocked exactly once, not spun the whole budget");

    /* And a message that only arrives once the loop is blocked is picked up. */
    reset();
    g_deliver_on_wait = 1;
    CHECK(wasmos_sys_event_loop_poll(&loop, 4) == 1, "the message delivered during the block runs");
    CHECK(traced("default") == 1, "and is dispatched");
    CHECK(g_select_wait_calls == 1, "with a single block");

    /* A loop with no select set must not block; it just reports nothing. */
    reset();
    wasmos_sys_event_loop_t bare;
    g_select_create_result = -1;
    wasmos_sys_event_loop_init(&bare, 1, 1);
    CHECK(bare.select_id == -1, "no select set");
    CHECK(wasmos_sys_event_loop_poll(&bare, 4) == 0, "the poll returns immediately");
    CHECK(g_select_wait_calls == 0, "without attempting to block on a set it does not have");
}

static void test_intent_capacity_and_release(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);

    int32_t ids[WASMOS_SYS_INTENT_MAX];
    int filled = 0;
    for (int i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (wasmos_sys_intent_send(&loop, 9, 1, 0x30, 0, 0, 0, 0, on_intent, 0, &ids[i]) == 0) {
            filled++;
        }
    }
    CHECK(filled == WASMOS_SYS_INTENT_MAX, "the table holds exactly WASMOS_SYS_INTENT_MAX intents");

    int32_t overflow = 0;
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 0x30, 0, 0, 0, 0, on_intent, 0, &overflow) == -1,
          "the next intent is refused rather than overwriting one");
    CHECK(g_sent_count == WASMOS_SYS_INTENT_MAX, "and nothing extra went on the wire");

    /* Every id is distinct — a duplicate would cross-wire two replies. */
    int distinct = 1;
    for (int i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        for (int j = i + 1; j < WASMOS_SYS_INTENT_MAX; ++j) {
            if (ids[i] == ids[j]) {
                distinct = 0;
            }
        }
    }
    CHECK(distinct, "every outstanding intent has a distinct request id");

    /* Resolving one frees its slot. */
    inbox_push(0x30, ids[3], 1);
    (void)wasmos_sys_event_loop_poll(&loop, 1);
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 0x30, 0, 0, 0, 0, on_intent, 0, &overflow) == 0,
          "resolving an intent returns its slot");

    /* So does cancelling one. */
    wasmos_sys_intent_cancel(&loop, ids[5]);
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 0x30, 0, 0, 0, 0, on_intent, 0, &overflow) == 0,
          "cancelling an intent returns its slot");

    /* A cancelled intent must not fire when its reply finally turns up. */
    g_trace_count = 0;
    inbox_push(0x30, ids[5], 1);
    (void)wasmos_sys_event_loop_poll(&loop, 1);
    CHECK(traced("intent") == 0, "the cancelled intent's late reply does not run its callback");
}

static void test_a_failed_send_does_not_strand_an_intent_slot(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);

    g_send_result = -3; /* transport full */
    int32_t rid = 0;
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 0x40, 0, 0, 0, 0, on_intent, 0, &rid) == -1,
          "a send failure is reported");
    g_send_result = 0;

    /* If the slot leaked, only 15 more would fit. */
    int filled = 0;
    for (int i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        int32_t id = 0;
        if (wasmos_sys_intent_send(&loop, 9, 1, 0x40, 0, 0, 0, 0, on_intent, 0, &id) == 0) {
            filled++;
        }
    }
    CHECK(filled == WASMOS_SYS_INTENT_MAX, "the failed send released its slot");
}

static void test_intent_send_validates_its_arguments(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);
    int32_t rid = 0;
    CHECK(wasmos_sys_intent_send(0, 9, 1, 1, 0, 0, 0, 0, on_intent, 0, &rid) == -1,
          "a NULL loop is refused");
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 1, 0, 0, 0, 0, 0, 0, &rid) == -1,
          "an intent with no resolve callback is refused — it could never be delivered");
    CHECK(g_sent_count == 0, "and neither reaches the wire");

    /* out_request_id is optional. */
    CHECK(wasmos_sys_intent_send(&loop, 9, 1, 1, 0, 0, 0, 0, on_intent, 0, 0) == 0,
          "omitting out_request_id is allowed");
}

/* The caller-supplied-id variant is what the FS and future bridges use, so its
 * duplicate check matters: two intents on one id would make the first reply
 * resolve the wrong one. */
static void test_a_caller_supplied_request_id_must_be_unique(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);

    CHECK(wasmos_sys_intent_send_with_request_id(&loop, 9, 1, 77, 0x50, 0, 0, 0, 0, on_intent,
                                                 (void*)"first") == 0,
          "a caller-supplied id is accepted");
    CHECK(wasmos_sys_intent_send_with_request_id(&loop, 9, 1, 77, 0x50, 0, 0, 0, 0, on_intent,
                                                 (void*)"second") == -1,
          "a second intent on the same id is refused");
    CHECK(g_sent_count == 1, "and the duplicate never reaches the wire");

    CHECK(wasmos_sys_intent_send_with_request_id(&loop, 9, 1, 0, 0x50, 0, 0, 0, 0, on_intent, 0) ==
              -1,
          "a zero request id is refused — zero is the unset marker");
    CHECK(wasmos_sys_intent_send_with_request_id(&loop, 9, 1, -5, 0x50, 0, 0, 0, 0, on_intent, 0) ==
              -1,
          "so is a negative one");

    inbox_push(0x50, 77, 0xC1);
    (void)wasmos_sys_event_loop_poll(&loop, 1);
    CHECK(traced("first") == 1, "the reply resolves the intent that owns the id");
    CHECK(traced("second") == 0, "and only that one");
}

static void test_handler_registration_replaces_and_bounds(void) {
    reset();
    wasmos_sys_event_loop_t loop;
    wasmos_sys_event_loop_init(&loop, 1, 1);

    CHECK(wasmos_sys_event_register(&loop, 0x60, on_handler, (void*)"first") == 0, "register");
    CHECK(wasmos_sys_event_register(&loop, 0x60, on_handler, (void*)"second") == 0,
          "re-registering the same type is accepted");
    inbox_push(0x60, 0, 1);
    (void)wasmos_sys_event_loop_poll(&loop, 1);
    CHECK(traced("second") == 1 && traced("first") == 0,
          "the second registration replaces the first rather than adding a duplicate");

    int filled = 1; /* 0x60 already occupies one slot */
    for (int i = 1; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        if (wasmos_sys_event_register(&loop, 0x60 + i, on_handler, 0) == 0) {
            filled++;
        }
    }
    CHECK(filled == WASMOS_SYS_HANDLER_MAX, "the table holds exactly WASMOS_SYS_HANDLER_MAX types");
    CHECK(wasmos_sys_event_register(&loop, 0x1000, on_handler, 0) == -1,
          "one more distinct type is refused");
    CHECK(wasmos_sys_event_register(&loop, 0x60, on_handler, (void*)"third") == 0,
          "but replacing an already-registered type still works when full");
    CHECK(wasmos_sys_event_register(0, 1, on_handler, 0) == -1, "a NULL loop is refused");
}

static void test_a_null_loop_poll_is_safe(void) {
    reset();
    CHECK(wasmos_sys_event_loop_poll(0, 4) == 0, "polling a NULL loop handles nothing");
    wasmos_sys_intent_cancel(0, 1);
    wasmos_sys_event_set_default(0, on_default, 0);
    CHECK(g_trace_count == 0, "and none of the NULL-tolerant calls dispatch anything");
}

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"L1 init builds a select set", test_init_builds_a_select_set},
        {"L2 an intent wins over a type handler", test_an_intent_wins_over_a_type_handler},
        {"L3 unmatched replies fall through",
         test_unmatched_replies_fall_through_to_handler_then_default},
        {"L4 the budget bounds the work", test_the_budget_bounds_the_work_per_poll},
        {"L5 an empty endpoint blocks", test_an_empty_endpoint_blocks_instead_of_spinning},
        {"L6 intent capacity and release", test_intent_capacity_and_release},
        {"L7 a failed send frees its slot", test_a_failed_send_does_not_strand_an_intent_slot},
        {"L8 intent send validates arguments", test_intent_send_validates_its_arguments},
        {"L9 a caller-supplied id must be unique",
         test_a_caller_supplied_request_id_must_be_unique},
        {"L10 handler registration replaces and bounds",
         test_handler_registration_replaces_and_bounds},
        {"L11 a NULL loop poll is safe", test_a_null_loop_poll_is_safe},
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
    printf("test_libsys_event_loop: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
