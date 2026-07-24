/* Runtime-behaviour tests for the WASM stackless coroutine/future core. */
#include <stdint.h>

#include "wasmos/libsys.h"

static wasmos_ipc_message_t sent_message;
static wasmos_ipc_message_t queued_message;
static int queued_reply;
static int send_status;

/* Typed filesystem operations use transfer buffers.  This coroutine unit
 * fixture only validates IPC/future behaviour, so minimal hostcall stubs keep
 * that ABI linkable; transfer-copy semantics are covered by FS tests. */
int32_t wasmos_xfer_buffer_acquire(int32_t size) {
    return size > 0 ? 1 : -1;
}
int32_t wasmos_xfer_buffer_borrow(int32_t endpoint, int32_t buffer, int32_t flags) {
    (void)endpoint;
    (void)buffer;
    (void)flags;
    return 1;
}
int32_t wasmos_xfer_buffer_release(int32_t buffer) {
    (void)buffer;
    return 0;
}
int32_t wasmos_xfer_buffer_write(int32_t buffer, int32_t ptr, int32_t len, int32_t offset) {
    (void)buffer;
    (void)ptr;
    (void)len;
    (void)offset;
    return 0;
}
int32_t wasmos_xfer_buffer_read(int32_t buffer, int32_t ptr, int32_t len, int32_t offset) {
    (void)buffer;
    (void)ptr;
    (void)len;
    (void)offset;
    return 0;
}

int32_t wasmos_ipc_send(int32_t destination, int32_t source, int32_t type, int32_t request_id,
                        int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    sent_message = (wasmos_ipc_message_t){.type = type,
                                          .request_id = request_id,
                                          .arg0 = arg0,
                                          .arg1 = arg1,
                                          .arg2 = arg2,
                                          .arg3 = arg3,
                                          .source = source,
                                          .destination = destination};
    return send_status;
}

int32_t wasmos_ipc_drain(int32_t endpoint) {
    (void)endpoint;
    if (!queued_reply) {
        return 0;
    }
    queued_reply = 0;
    return 1;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    switch (field) {
    case 0:
        return queued_message.type;
    case 1:
        return queued_message.request_id;
    case 2:
        return queued_message.arg0;
    case 3:
        return queued_message.arg1;
    case 4:
        return queued_message.source;
    case 5:
        return queued_message.destination;
    case 6:
        return queued_message.arg2;
    case 7:
        return queued_message.arg3;
    default:
        return 0;
    }
}

int32_t wasmos_ipc_select_create(void) {
    return -1;
}
int32_t wasmos_ipc_select_add(int32_t select_id, int32_t endpoint) {
    (void)select_id;
    (void)endpoint;
    return -1;
}
int32_t wasmos_ipc_select_wait(int32_t select_id) {
    (void)select_id;
    return -1;
}
int32_t wasmos_ipc_select_destroy(int32_t select_id) {
    (void)select_id;
    return 0;
}

typedef struct {
    int pc;
    uint32_t* events;
    size_t* event_count;
} yield_state_t;

static int32_t yield_task(void* user, uintptr_t* out_value) {
    yield_state_t* state = user;
    if (state->pc == 0) {
        state->events[(*state->event_count)++] = 1u;
        state->pc = 1;
        return wasmos_wasm_coroutine_yield();
    }
    state->events[(*state->event_count)++] = 3u;
    *out_value = 7u;
    return WASMOS_WASM_TASK_COMPLETE;
}

typedef struct {
    int pc;
    wasmos_future_t* future;
    int status;
    uintptr_t value;
} waiter_state_t;

static int32_t waiter_task(void* user, uintptr_t* out_value) {
    waiter_state_t* state = user;
    if (state->pc == 0) {
        state->pc = 1;
        state->status = wasmos_future_await(state->future, &state->value);
        if (state->status == WASMOS_WASM_AWAIT_PENDING)
            return WASMOS_WASM_TASK_YIELDED;
    }
    if (state->pc == 1) {
        state->status = wasmos_future_await(state->future, &state->value);
        if (state->status == WASMOS_WASM_AWAIT_PENDING)
            return WASMOS_WASM_TASK_YIELDED;
        *out_value = state->value;
        return state->status;
    }
    return -1;
}

typedef struct {
    wasmos_promise_t* promise;
    uintptr_t value;
} resolver_state_t;

static int32_t resolver_task(void* user, uintptr_t* out_value) {
    resolver_state_t* state = user;
    if (!wasmos_promise_resolve(state->promise, state->value))
        return -1;
    *out_value = state->value;
    return 0;
}

static int test_yield_await_and_join(void) {
    wasmos_wasm_runtime_t runtime = {0};
    wasmos_wasm_coroutine_t first = {0}, waiter = {0}, resolver = {0};
    wasmos_future_t source;
    wasmos_promise_t promise;
    uint32_t events[4] = {0};
    size_t event_count = 0;
    yield_state_t first_state = {.events = events, .event_count = &event_count};
    waiter_state_t waiter_state = {.future = &source};
    resolver_state_t resolver_state = {.promise = &promise, .value = 42u};
    int32_t joined = 0;
    int32_t status = 0;
    uintptr_t value = 0;

    wasmos_wasm_runtime_init(&runtime);
    wasmos_future_init(&source, &promise);
    if (!wasmos_async_start(&runtime, &first, yield_task, &first_state) ||
        !wasmos_async_start(&runtime, &waiter, waiter_task, &waiter_state) ||
        !wasmos_async_start(&runtime, &resolver, resolver_task, &resolver_state) ||
        wasmos_wasm_coroutine_run(&runtime) != 5 || event_count != 2u || events[0] != 1u ||
        events[1] != 3u || waiter.state != WASMOS_WASM_COROUTINE_DEAD || waiter_state.status != 0 ||
        waiter_state.value != 42u || wasmos_wasm_coroutine_join(&first, &joined) != 0 ||
        joined != 7 || !wasmos_future_poll(&waiter.completion, &status, &value) || status != 0 ||
        value != 42u || wasmos_promise_resolve(&promise, 9u)) {
        return __LINE__;
    }
    return 0;
}

typedef struct {
    unsigned calls;
} callback_state_t;

static int32_t increment(void* user, uintptr_t value, uintptr_t* out_value) {
    callback_state_t* state = user;
    state->calls++;
    *out_value = value + 1u;
    return 0;
}

static int32_t recover(void* user, int32_t status, uintptr_t* out_value) {
    callback_state_t* state = user;
    state->calls++;
    if (status >= 0)
        return -1;
    *out_value = 55u;
    return 0;
}

static int32_t reject_callback(void* user, uintptr_t value, uintptr_t* out_value) {
    (void)user;
    (void)value;
    (void)out_value;
    return -41;
}

static int test_future_chains_and_deferred_callbacks(void) {
    wasmos_wasm_runtime_t runtime = {0};
    wasmos_future_t source, rejected;
    wasmos_promise_t source_promise, rejected_promise;
    wasmos_future_continuation_t plus_one = {0}, recover_continuation = {0},
                                 reject_continuation = {0};
    wasmos_future_t* child;
    callback_state_t state = {0};
    int32_t status = 0;
    uintptr_t value = 0;

    wasmos_wasm_runtime_init(&runtime);
    wasmos_future_init(&source, &source_promise);
    child = wasmos_future_then(&runtime, &source, &plus_one, increment, NULL, &state);
    if (!child || !wasmos_promise_resolve(&source_promise, 20u) || state.calls != 0u ||
        wasmos_wasm_coroutine_run(&runtime) != 0 || state.calls != 1u ||
        !wasmos_future_poll(child, &status, &value) || status != 0 || value != 21u) {
        return __LINE__;
    }
    wasmos_future_init(&rejected, &rejected_promise);
    child = wasmos_future_then(&runtime, &rejected, &recover_continuation, NULL, recover, &state);
    if (!child || !wasmos_promise_reject(&rejected_promise, -23) ||
        wasmos_promise_reject(&rejected_promise, -24) || wasmos_wasm_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(child, &status, &value) || status != 0 || value != 55u) {
        return __LINE__;
    }
    wasmos_future_init(&source, &source_promise);
    child =
        wasmos_future_then(&runtime, &source, &reject_continuation, reject_callback, NULL, NULL);
    if (!child || !wasmos_promise_resolve(&source_promise, 1u) ||
        wasmos_wasm_coroutine_run(&runtime) != 0 || !wasmos_future_poll(child, &status, &value) ||
        status != -41 || value != 0u) {
        return __LINE__;
    }
    return 0;
}

static int test_race_and_all(void) {
    wasmos_wasm_runtime_t runtime = {0};
    wasmos_future_t first, second, third;
    wasmos_promise_t first_promise, second_promise, third_promise;
    wasmos_future_group_t race_group = {0}, all_group = {0}, failed_group = {0};
    wasmos_future_continuation_t race_continuations[3] = {0};
    wasmos_future_continuation_t all_continuations[3] = {0};
    wasmos_future_continuation_t failed_continuations[2] = {0};
    uintptr_t values[3] = {0};
    wasmos_future_t* result;
    int32_t status = 0;
    uintptr_t value = 0;

    wasmos_wasm_runtime_init(&runtime);
    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    wasmos_future_init(&third, &third_promise);
    result = WASMOS_FUTURE_RACE(&runtime, &race_group, race_continuations, &first, &second, &third);
    if (!result || !wasmos_promise_resolve(&second_promise, 2u) ||
        !wasmos_promise_reject(&first_promise, -9) || !wasmos_promise_resolve(&third_promise, 3u) ||
        wasmos_wasm_coroutine_run(&runtime) != 0 || !wasmos_future_poll(result, &status, &value) ||
        status != 0 || value != 2u || race_group.active) {
        return __LINE__;
    }
    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    wasmos_future_init(&third, &third_promise);
    result =
        WASMOS_FUTURE_ALL(&runtime, &all_group, values, all_continuations, &first, &second, &third);
    if (!result || !wasmos_promise_resolve(&third_promise, 3u) ||
        !wasmos_promise_resolve(&first_promise, 1u) ||
        !wasmos_promise_resolve(&second_promise, 2u) || wasmos_wasm_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || status != 0 || value != (uintptr_t)values ||
        values[0] != 1u || values[1] != 2u || values[2] != 3u || all_group.active) {
        return __LINE__;
    }
    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    result =
        WASMOS_FUTURE_ALL(&runtime, &failed_group, values, failed_continuations, &first, &second);
    if (!result || !wasmos_promise_reject(&first_promise, -7) ||
        wasmos_wasm_coroutine_run(&runtime) != 0 || !wasmos_future_poll(result, &status, &value) ||
        status != -7 || !failed_group.active || !wasmos_promise_resolve(&second_promise, 2u) ||
        wasmos_wasm_coroutine_run(&runtime) != 0 || failed_group.active) {
        return __LINE__;
    }
    return 0;
}

static int test_contracts(void) {
    wasmos_wasm_runtime_t runtime = {0}, other = {0};
    wasmos_wasm_coroutine_t coroutine = {0};
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_future_continuation_t continuation = {0};

    wasmos_wasm_runtime_init(&runtime);
    wasmos_wasm_runtime_init(&other);
    wasmos_future_init(&future, &promise);
    if (wasmos_wasm_coroutine_run(NULL) != -1 || wasmos_future_await(&future, NULL) != -1 ||
        wasmos_promise_reject(&promise, 0) ||
        !wasmos_future_then(&runtime, &future, &continuation, NULL, NULL, NULL) ||
        wasmos_future_then(&other, &future, &(wasmos_future_continuation_t){0}, NULL, NULL, NULL) ||
        !wasmos_async_start(&runtime, &coroutine, yield_task, NULL) ||
        wasmos_async_start(&runtime, &coroutine, yield_task, NULL) ||
        wasmos_future_race(&runtime, NULL, NULL, 0, NULL) ||
        wasmos_future_all(&runtime, NULL, NULL, 0, NULL, NULL)) {
        return __LINE__;
    }
    return 0;
}

static int32_t reject_arg0(void* user, const wasmos_ipc_message_t* reply) {
    (void)user;
    return reply->arg0 < 0 ? reply->arg0 : 0;
}

static int test_ipc_future(void) {
    wasmos_sys_event_loop_t loop;
    wasmos_sys_wasm_ipc_future_t operation;
    wasmos_future_t* future;
    int32_t request_id = 0;
    int32_t status = 0;
    uintptr_t value = 0;

    send_status = 0;
    queued_reply = 0;
    wasmos_sys_event_loop_init(&loop, 55, 700);
    wasmos_sys_wasm_ipc_future_init(&operation, NULL, NULL);
    future =
        wasmos_sys_wasm_ipc_future_send(&loop, &operation, 44, 55, 0x123, 1, 2, 3, 4, &request_id);
    if (future != &operation.future || request_id != 700 || !operation.active ||
        sent_message.destination != 44 || sent_message.source != 55 ||
        sent_message.request_id != request_id) {
        return __LINE__;
    }
    queued_message = (wasmos_ipc_message_t){
        .type = 0x280, .request_id = request_id, .arg0 = 77, .source = 44, .destination = 55};
    queued_reply = 1;
    if (wasmos_sys_event_loop_poll(&loop, 1) != 1 || operation.active ||
        !wasmos_future_poll(future, &status, &value) || status != 0 ||
        value != (uintptr_t)&operation.reply || operation.reply.arg0 != 77) {
        return __LINE__;
    }
    wasmos_sys_wasm_ipc_future_init(&operation, reject_arg0, NULL);
    future = wasmos_sys_wasm_ipc_future_send(&loop, &operation, 44, 55, 1, 0, 0, 0, 0, NULL);
    queued_message = (wasmos_ipc_message_t){.request_id = sent_message.request_id, .arg0 = -29};
    queued_reply = 1;
    if (!future || wasmos_sys_event_loop_poll(&loop, 1) != 1 ||
        !wasmos_future_poll(future, &status, &value) || status != -29 || value != 0) {
        return __LINE__;
    }
    send_status = 1;
    wasmos_sys_wasm_ipc_future_init(&operation, NULL, NULL);
    future = wasmos_sys_wasm_ipc_future_send(&loop, &operation, 44, 55, 1, 0, 0, 0, 0, NULL);
    if (future != &operation.future || operation.active ||
        !wasmos_future_poll(future, &status, &value) || status != -1) {
        return __LINE__;
    }
    return 0;
}

static int test_fs_request_future(void) {
    wasmos_sys_event_loop_t loop;
    wasmos_sys_wasm_fs_request_t request;
    wasmos_future_t* future;
    int32_t status = 0;
    uintptr_t value = 0;

    send_status = 0;
    queued_reply = 0;
    wasmos_sys_event_loop_init(&loop, 55, 900);
    wasmos_sys_wasm_fs_request_init(&request);
    future =
        wasmos_sys_wasm_fs_request_send(&loop, &request, 44, 55, FS_IPC_OPEN_REQ, 1, 2, 3, 4, NULL);
    if (future != &request.ipc.future || sent_message.request_id != 900) {
        return __LINE__;
    }
    queued_message = (wasmos_ipc_message_t){
        .type = FS_IPC_RESP, .request_id = sent_message.request_id, .arg0 = 17};
    queued_reply = 1;
    if (wasmos_sys_event_loop_poll(&loop, 1) != 1 || !wasmos_future_poll(future, &status, &value) ||
        status != 0 || wasmos_sys_wasm_fs_request_reply(&request)->arg0 != 17) {
        return __LINE__;
    }
    wasmos_sys_wasm_fs_request_init(&request);
    future =
        wasmos_sys_wasm_fs_request_send(&loop, &request, 44, 55, FS_IPC_OPEN_REQ, 0, 0, 0, 0, NULL);
    queued_message = (wasmos_ipc_message_t){.type = 0x77, .request_id = sent_message.request_id};
    queued_reply = 1;
    if (!future || wasmos_sys_event_loop_poll(&loop, 1) != 1 ||
        !wasmos_future_poll(future, &status, &value) || status >= 0) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    int rc = test_yield_await_and_join();
    if (rc == 0)
        rc = test_future_chains_and_deferred_callbacks();
    if (rc == 0)
        rc = test_race_and_all();
    if (rc == 0)
        rc = test_contracts();
    if (rc == 0)
        rc = test_ipc_future();
    if (rc == 0)
        rc = test_fs_request_future();
    if (rc != 0) {
        return 1;
    }
    return 0;
}
