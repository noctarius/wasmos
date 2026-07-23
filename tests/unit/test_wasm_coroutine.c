/* Runtime-behaviour tests for the WASM stackless coroutine/future core. */
#include <stdint.h>
#include <stdio.h>

#include "wasmos/coroutine_wasm.h"

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
        if (state->status == WASMOS_WASM_AWAIT_PENDING) return WASMOS_WASM_TASK_YIELDED;
    }
    if (state->pc == 1) {
        state->status = wasmos_future_await(state->future, &state->value);
        if (state->status == WASMOS_WASM_AWAIT_PENDING) return WASMOS_WASM_TASK_YIELDED;
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
    if (!wasmos_promise_resolve(state->promise, state->value)) return -1;
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
        events[1] != 3u || waiter.state != WASMOS_WASM_COROUTINE_DEAD ||
        waiter_state.status != 0 || waiter_state.value != 42u ||
        wasmos_wasm_coroutine_join(&first, &joined) != 0 || joined != 7 ||
        !wasmos_future_poll(&waiter.completion, &status, &value) || status != 0 || value != 42u ||
        wasmos_promise_resolve(&promise, 9u)) {
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
    if (status >= 0) return -1;
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
    wasmos_future_continuation_t plus_one = {0}, recover_continuation = {0}, reject_continuation = {0};
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
    child = wasmos_future_then(&runtime, &source, &reject_continuation, reject_callback, NULL, NULL);
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
    result = WASMOS_FUTURE_ALL(&runtime, &all_group, values, all_continuations, &first, &second, &third);
    if (!result || !wasmos_promise_resolve(&third_promise, 3u) ||
        !wasmos_promise_resolve(&first_promise, 1u) || !wasmos_promise_resolve(&second_promise, 2u) ||
        wasmos_wasm_coroutine_run(&runtime) != 0 || !wasmos_future_poll(result, &status, &value) ||
        status != 0 || value != (uintptr_t)values || values[0] != 1u || values[1] != 2u ||
        values[2] != 3u || all_group.active) {
        return __LINE__;
    }
    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    result = WASMOS_FUTURE_ALL(&runtime, &failed_group, values, failed_continuations, &first, &second);
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
        wasmos_promise_reject(&promise, 0) || !wasmos_future_then(&runtime, &future, &continuation,
                                                                   NULL, NULL, NULL) ||
        wasmos_future_then(&other, &future, &(wasmos_future_continuation_t){0}, NULL, NULL, NULL) ||
        !wasmos_async_start(&runtime, &coroutine, yield_task, NULL) ||
        wasmos_async_start(&runtime, &coroutine, yield_task, NULL) ||
        wasmos_future_race(&runtime, NULL, NULL, 0, NULL) ||
        wasmos_future_all(&runtime, NULL, NULL, 0, NULL, NULL)) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    int rc = test_yield_await_and_join();
    if (rc == 0) rc = test_future_chains_and_deferred_callbacks();
    if (rc == 0) rc = test_race_and_all();
    if (rc == 0) rc = test_contracts();
    if (rc != 0) {
        fprintf(stderr, "wasm coroutine test failed at line %d\n", rc);
        return 1;
    }
    return 0;
}
