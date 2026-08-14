/* Runtime-behaviour tests for the native IPC-to-future adapter.
 *
 * Native services do not make host calls; they are handed a wasmos_driver_api_t
 * function table by the kernel loader. That table is the only thing stubbed
 * here: the adapter, the event loop, the service bootstrap and the stackful
 * coroutine runtime are the real sources (ipc_future_native.c,
 * libsys_native.c, service_runtime_native.c, coroutine_native.c plus the host's
 * context-switch assembly), compiled for the build host.
 *
 * A case fills in only the api slots its path touches, so an adapter that
 * started calling a further slot would trap on a NULL rather than pass.
 *
 * MODELLING NOTE (blocking). Two kernel facilities that would suspend the
 * caller are flattened. api->ipc_recv never waits -- it reports "empty"
 * immediately, which is the same code a non-blocking receive gives, so the
 * event loop's drain path is unchanged. api->sched_yield, really
 * process_yield(PROCESS_RUN_IDLE), only counts calls: nothing else can run on
 * the host, so all forward progress in the service-run loop has to come from
 * the root coroutine itself, and the yield COUNT is what pins the loop's shape.
 */
#include <stdint.h>

#include "test_shuffle.h"

#include "wasmos/libsys_native.h"

/* The scripted transport. sent_message keeps only the LAST message given to
 * fake_ipc_send -- which is how a case learns the request id the loop minted --
 * and queued_reply plus reply_queued are one pending reply the next receive
 * hands over. reset_transport() clears these four; service_steps and
 * service_yields belong to the service-runtime case and are cleared there. */
static nd_ipc_message_t sent_message;
static nd_ipc_message_t queued_reply;
static int32_t send_status;
static uint8_t reply_queued;
static uint32_t service_steps;
static uint32_t service_yields;

/* Records the outgoing message and reports send_status. The context id and
 * endpoint are ignored, so no ownership or delivery rule is modelled and
 * nothing is queued for anyone. The real nd_ipc_send returns 0 or a negative
 * IPC code; send_status is returned verbatim and the failure case uses +1,
 * which the real transport never produces but which still takes the failure
 * path because the intent layer only tests for non-zero. */
static int fake_ipc_send(uint32_t sender_context_id, uint32_t endpoint,
                         const nd_ipc_message_t* message) {
    (void)sender_context_id;
    (void)endpoint;
    sent_message = *message;
    return send_status;
}

/* driver_api ipc_recv convention: 0 delivered a message, 1 means the endpoint is
 * empty, and any other value is an error the poll loop reports as -1. */
static int fake_ipc_recv(uint32_t receiver_context_id, uint32_t endpoint,
                         nd_ipc_message_t* out_message) {
    (void)receiver_context_id;
    (void)endpoint;
    if (!reply_queued) {
        return 1;
    }
    *out_message = queued_reply;
    reply_queued = 0u;
    return 0;
}

/* A fixed caller context. The loop reads it once per send and once per poll and
 * passes it to the transport; both stubs ignore it, so context checks are not
 * modelled at all here. */
static uint32_t fake_current_pid(void) {
    return 9u;
}

/* Counts only -- see the MODELLING NOTE in the file header. */
static void fake_sched_yield(void) {
    service_yields++;
}

/* Stands in for a service's main coroutine: checks that it is handed the api,
 * the runtime and its own user pointer, records a step either side of one
 * suspension, and returns 37 so the case can prove service_run reports the
 * root's return value rather than a status of its own. */
static int32_t service_main(wasmos_driver_api_t* api, wasmos_native_coroutine_runtime_t* runtime,
                            void* user) {
    if (api == NULL || runtime == NULL || user != (void*)(uintptr_t)0x1234u) {
        return -1;
    }
    service_steps++;
    wasmos_native_coroutine_yield();
    service_steps++;
    return 37;
}

/* A reply classifier: the adapter resolves the future when this returns 0 and
 * rejects it otherwise, carrying a negative status through unchanged and
 * normalising any other non-zero one to -1. Reading the status straight out of
 * arg0 lets a case script either outcome from the reply it queues. */
static int32_t reject_from_arg0(void* user, const nd_ipc_message_t* reply) {
    (void)user;
    return (int32_t)reply->arg0;
}

/* Clears the scripted transport between cases: no recorded send, no queued
 * reply, and sends succeeding again. It does not touch the event loop or the
 * futures, which each case declares on its own stack. */
static void reset_transport(void) {
    sent_message = (nd_ipc_message_t){0};
    queued_reply = (nd_ipc_message_t){0};
    send_status = 0;
    reply_queued = 0u;
}

static int test_resolve_and_copy_reply(void) {
    wasmos_driver_api_t api = {0};
    wasmos_sys_native_event_loop_t loop;
    wasmos_sys_native_ipc_future_t operation;
    wasmos_future_t* future;
    uint32_t request_id = 0u;
    int32_t status = -1;
    uintptr_t value = 0u;

    reset_transport();
    api.ipc_send = fake_ipc_send;
    api.ipc_recv = fake_ipc_recv;
    api.sched_current_pid = fake_current_pid;
    wasmos_sys_native_event_loop_init(&loop, &api, 55u, 700u);
    wasmos_sys_native_ipc_future_init(&operation, NULL, NULL);
    future = wasmos_sys_native_ipc_future_send(&loop, &operation, 44u, 55u, 0x123u, 1u, 2u, 3u, 4u,
                                               &request_id);
    if (future != &operation.future || request_id != 700u || !operation.active ||
        sent_message.type != 0x123u || sent_message.source != 55u ||
        sent_message.destination != 44u || sent_message.request_id != request_id ||
        wasmos_future_poll(future, &status, &value)) {
        return __LINE__;
    }
    queued_reply = (nd_ipc_message_t){
        .type = 0x280u, .source = 44u, .destination = 55u, .request_id = request_id, .arg0 = 77u};
    reply_queued = 1u;
    if (wasmos_sys_native_event_loop_poll(&loop, 1u) != 1 || operation.active ||
        !wasmos_future_poll(future, &status, &value) || status != 0 ||
        value != (uintptr_t)&operation.reply || operation.reply.arg0 != 77u ||
        operation.reply.request_id != request_id) {
        return __LINE__;
    }
    return 0;
}

static int test_reject_send_failure_and_cancel(void) {
    wasmos_driver_api_t api = {0};
    wasmos_sys_native_event_loop_t loop;
    wasmos_sys_native_ipc_future_t rejected;
    wasmos_sys_native_ipc_future_t failed_send;
    wasmos_sys_native_ipc_future_t cancelled;
    wasmos_future_t* future;
    int32_t status = 0;
    uintptr_t value = 1u;

    reset_transport();
    api.ipc_send = fake_ipc_send;
    api.ipc_recv = fake_ipc_recv;
    api.sched_current_pid = fake_current_pid;
    wasmos_sys_native_event_loop_init(&loop, &api, 55u, 800u);

    wasmos_sys_native_ipc_future_init(&rejected, reject_from_arg0, NULL);
    future =
        wasmos_sys_native_ipc_future_send(&loop, &rejected, 44u, 55u, 1u, 0u, 0u, 0u, 0u, NULL);
    queued_reply = (nd_ipc_message_t){.request_id = sent_message.request_id, .arg0 = (uint32_t)-29};
    reply_queued = 1u;
    if (!future || wasmos_sys_native_event_loop_poll(&loop, 1u) != 1 ||
        !wasmos_future_poll(future, &status, &value) || status != -29 || value != 0u) {
        return __LINE__;
    }

    send_status = 1;
    wasmos_sys_native_ipc_future_init(&failed_send, NULL, NULL);
    future =
        wasmos_sys_native_ipc_future_send(&loop, &failed_send, 44u, 55u, 1u, 0u, 0u, 0u, 0u, NULL);
    if (future != &failed_send.future || failed_send.active ||
        !wasmos_future_poll(future, &status, &value) || status != -1 || value != 0u) {
        return __LINE__;
    }

    send_status = 0;
    wasmos_sys_native_ipc_future_init(&cancelled, NULL, NULL);
    future =
        wasmos_sys_native_ipc_future_send(&loop, &cancelled, 44u, 55u, 1u, 0u, 0u, 0u, 0u, NULL);
    if (!future || !cancelled.active) {
        return __LINE__;
    }
    wasmos_sys_native_ipc_future_cancel(&cancelled, -61);
    if (cancelled.active || !wasmos_future_poll(future, &status, &value) || status != -61 ||
        value != 0u) {
        return __LINE__;
    }
    queued_reply = (nd_ipc_message_t){.request_id = sent_message.request_id, .arg0 = 99u};
    reply_queued = 1u;
    if (wasmos_sys_native_event_loop_poll(&loop, 1u) != 1 ||
        !wasmos_future_poll(future, &status, &value) || status != -61 || value != 0u) {
        return __LINE__;
    }
    return 0;
}

static int test_service_root_runtime(void) {
    wasmos_driver_api_t api = {0};
    wasmos_sys_native_service_t service;
    uint8_t root_stack[4096] __attribute__((aligned(16)));

    service_steps = 0u;
    service_yields = 0u;
    api.sched_yield = fake_sched_yield;
    /* One coroutine yield costs exactly one api->sched_yield: the run loop
     * resumes the root, finds it still alive, and only then yields the host
     * thread. The root's return value is what service_run reports. */
    wasmos_sys_native_service_init(&service, root_stack, sizeof(root_stack));
    if (wasmos_sys_native_service_run(&service, &api, service_main, (void*)(uintptr_t)0x1234u) !=
            37 ||
        service_steps != 2u || service_yields != 1u ||
        service.root.state != WASMOS_NATIVE_COROUTINE_DEAD) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_resolve_and_copy_reply),
        WASMOS_TEST_CASE(test_reject_send_failure_and_cancel),
        WASMOS_TEST_CASE(test_service_root_runtime),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
