/* test_libc_ipc_call.c - the guest libc's synchronous call
 * (wasmos_ipc_call_retry in src/libc/include/wasmos/ipc.h).
 *
 * The host calls it reaches are stubbed over a script of what arrives on the
 * reply endpoint: wasmos_ipc_select_one takes the next scripted message and
 * wasmos_ipc_last_field reads it. An exhausted script answers select_one with
 * WOULD_BLOCK -- the real call would park there forever -- so a case that
 * expects the call to return never hangs the suite.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasmos/ipc.h"
#include "test_shuffle.h"

#define SERVER_EP 40
#define OTHER_EP 41
#define REPLY_EP 50
#define WOULD_BLOCK (-1000)
#define SCRIPT_MAX 8

typedef struct {
    int32_t fields[8]; /* WASMOS_IPC_FIELD_* order */
} scripted_t;

static scripted_t g_script[SCRIPT_MAX];
static uint32_t g_script_len;
static uint32_t g_script_next;
static const scripted_t* g_last;
static int32_t g_sends;
/* What wasmos_ipc_peer_await answers: 0, or WASMOS_NOENT for a destination
 * that has no owner left. */
static int32_t g_await_rc;
static int32_t g_awaits;

static void script_reset(void) {
    memset(g_script, 0, sizeof(g_script));
    g_script_len = 0;
    g_script_next = 0;
    g_last = 0;
    g_sends = 0;
    g_await_rc = 0;
    g_awaits = 0;
}

static void script_push(int32_t type, int32_t request_id, int32_t source, int32_t arg0) {
    scripted_t* m = &g_script[g_script_len++];
    m->fields[WASMOS_IPC_FIELD_TYPE] = type;
    m->fields[WASMOS_IPC_FIELD_REQUEST_ID] = request_id;
    m->fields[WASMOS_IPC_FIELD_SOURCE] = source;
    m->fields[WASMOS_IPC_FIELD_DESTINATION] = REPLY_EP;
    m->fields[WASMOS_IPC_FIELD_ARG0] = arg0;
}

int32_t wasmos_ipc_send(int32_t dest, int32_t src, int32_t type, int32_t request_id, int32_t a0,
                        int32_t a1, int32_t a2, int32_t a3) {
    (void)dest;
    (void)src;
    (void)type;
    (void)request_id;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    g_sends++;
    return 0;
}

int32_t wasmos_ipc_select_one(int32_t endpoint) {
    if (endpoint != REPLY_EP || g_script_next >= g_script_len) {
        return WOULD_BLOCK;
    }
    g_last = &g_script[g_script_next++];
    return endpoint;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    if (!g_last || field < 0 || field > 7) {
        return -1;
    }
    return g_last->fields[field];
}

int32_t wasmos_sched_yield(void) {
    return 0;
}

int32_t wasmos_ipc_peer_await(int32_t waiter, int32_t awaited) {
    (void)waiter;
    (void)awaited;
    g_awaits++;
    return g_await_rc;
}

static int test_the_matching_reply_is_returned(void) {
    wasmos_ipc_message_t reply;
    script_reset();
    script_push(0x123, 9, OTHER_EP, 0);   /* another sender: discarded */
    script_push(0x123, 8, SERVER_EP, 0);  /* another request: discarded */
    script_push(0x124, 9, SERVER_EP, 77); /* the reply */
    if (wasmos_ipc_call(SERVER_EP, REPLY_EP, 0x100, 9, 0, 0, 0, 0, &reply) != 0) {
        return __LINE__;
    }
    if (reply.type != 0x124 || reply.arg0 != 77 || g_sends != 1) {
        return __LINE__;
    }
    return 0;
}

/* Regression: 2026-11-20-ipc-call-ignores-peer-hangup -- a call whose server
 * died before answering discarded the server's WASMOS_IPC_HANGUP like any
 * other stray message and parked on the reply endpoint for good. A client of a
 * supervised provider (menu_bar's clock reading rtc, every libui app measuring
 * text through the font service) froze for the rest of its life on the
 * provider's first restart. */
static int test_a_hangup_from_the_server_ends_the_call(void) {
    wasmos_ipc_message_t reply;
    script_reset();
    script_push(WASMOS_IPC_HANGUP, 0, 0, SERVER_EP);
    if (wasmos_ipc_call(SERVER_EP, REPLY_EP, 0x100, 9, 0, 0, 0, 0, &reply) != WASMOS_PEER_GONE) {
        return __LINE__;
    }
    return 0;
}

/* Another peer's end says nothing about this call: the reply still comes. */
static int test_another_peers_hangup_is_not_this_calls(void) {
    wasmos_ipc_message_t reply;
    script_reset();
    script_push(WASMOS_IPC_HANGUP, 0, 0, OTHER_EP);
    script_push(0x124, 9, SERVER_EP, 5);
    if (wasmos_ipc_call(SERVER_EP, REPLY_EP, 0x100, 9, 0, 0, 0, 0, &reply) != 0 ||
        reply.arg0 != 5) {
        return __LINE__;
    }
    return 0;
}

/* A server that is already gone is not sent to and not waited for: the
 * await finds no owner for the destination. */
static int test_a_server_already_gone_is_not_waited_for(void) {
    wasmos_ipc_message_t reply;
    script_reset();
    g_await_rc = WASMOS_NOENT;
    if (wasmos_ipc_call(SERVER_EP, REPLY_EP, 0x100, 9, 0, 0, 0, 0, &reply) != WASMOS_PEER_GONE ||
        g_sends != 0) {
        return __LINE__;
    }
    return 0;
}

/* The await is registered before the request goes out, so a server that dies
 * before it first answers is reported on the reply endpoint too. */
static int test_the_call_awaits_its_server_first(void) {
    wasmos_ipc_message_t reply;
    script_reset();
    script_push(0x124, 9, SERVER_EP, 0);
    if (wasmos_ipc_call(SERVER_EP, REPLY_EP, 0x100, 9, 0, 0, 0, 0, &reply) != 0 || g_awaits != 1) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_the_matching_reply_is_returned),
        WASMOS_TEST_CASE(test_a_hangup_from_the_server_ends_the_call),
        WASMOS_TEST_CASE(test_another_peers_hangup_is_not_this_calls),
        WASMOS_TEST_CASE(test_a_server_already_gone_is_not_waited_for),
        WASMOS_TEST_CASE(test_the_call_awaits_its_server_first),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_libc_ipc_call: ok\n");
    return 0;
}
