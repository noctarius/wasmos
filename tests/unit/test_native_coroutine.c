/* Runtime-behaviour tests for the single-worker native coroutine/future core. */
#include <stdint.h>
#include <stdio.h>

#include "wasmos/coroutine_native.h"

typedef struct {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_native_coroutine_t first;
    wasmos_native_coroutine_t second;
    wasmos_native_coroutine_t joiner;
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_future_continuation_t first_continuation;
    wasmos_future_continuation_t late_continuation;
    uint8_t first_stack[4096];
    uint8_t second_stack[4096];
    uint8_t joiner_stack[4096];
    uint32_t events[10];
    uint32_t event_count;
    int32_t join_result;
} test_state_t;

static void record(test_state_t* state, uint32_t event) {
    if (state->event_count < sizeof(state->events) / sizeof(state->events[0])) {
        state->events[state->event_count++] = event;
    }
}

static void first_entry(void* arg) {
    test_state_t* state = arg;
    uintptr_t value = 0;

    record(state, 1u);
    wasmos_native_coroutine_yield();
    record(state, 3u);
    if (wasmos_future_await(&state->future, &value) != 0 || value != 42u) {
        wasmos_native_coroutine_exit(-1);
    }
    record(state, 5u);
    wasmos_native_coroutine_exit(7);
}

static void second_entry(void* arg) {
    test_state_t* state = arg;

    record(state, 2u);
    wasmos_native_coroutine_yield();
    record(state, 4u);
    if (!wasmos_promise_resolve(&state->promise, 42u) ||
        wasmos_promise_resolve(&state->promise, 9u)) {
        wasmos_native_coroutine_exit(-1);
    }
}

static void joiner_entry(void* arg) {
    test_state_t* state = arg;

    if (wasmos_native_coroutine_join(&state->first, &state->join_result) != 0 ||
        state->join_result != 7) {
        wasmos_native_coroutine_exit(-1);
    }
    record(state, 6u);
}

static int32_t success_callback(void* user, uintptr_t value, uintptr_t* out_value) {
    test_state_t* state = user;
    record(state, value == 42u ? 7u : 99u);
    *out_value = value;
    return 0;
}

static int32_t error_callback(void* user, int32_t status, uintptr_t* out_value) {
    test_state_t* state = user;
    record(state, status < 0 ? 98u : 99u);
    *out_value = 0;
    return status;
}

static int32_t increment_callback(void* user, uintptr_t value, uintptr_t* out_value) {
    (void)user;
    *out_value = value + 1u;
    return 0;
}

static int32_t double_callback(void* user, uintptr_t value, uintptr_t* out_value) {
    (void)user;
    *out_value = value * 2u;
    return 0;
}

static int32_t recover_callback(void* user, int32_t status, uintptr_t* out_value) {
    (void)user;
    if (status >= 0) {
        return -1;
    }
    *out_value = 55u;
    return 0;
}

static int32_t reject_callback(void* user, uintptr_t value, uintptr_t* out_value) {
    (void)user;
    (void)value;
    (void)out_value;
    return -41;
}

static int test_yield_await_and_join(void) {
    static const uint32_t expected[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 7u};
    test_state_t state = {0};
    int run_count;

    wasmos_native_coroutine_runtime_init(&state.runtime);
    wasmos_future_init(&state.future, &state.promise);
    if (!wasmos_future_then(&state.runtime, &state.future, &state.first_continuation,
                            success_callback, error_callback, &state)) {
        return __LINE__;
    }
    if (wasmos_native_coroutine_spawn(&state.runtime, &state.first, state.first_stack,
                                      sizeof(state.first_stack), first_entry, &state) != 0 ||
        wasmos_native_coroutine_spawn(&state.runtime, &state.second, state.second_stack,
                                      sizeof(state.second_stack), second_entry, &state) != 0 ||
        wasmos_native_coroutine_spawn(&state.runtime, &state.joiner, state.joiner_stack,
                                      sizeof(state.joiner_stack), joiner_entry, &state) != 0) {
        return __LINE__;
    }
    run_count = wasmos_native_coroutine_run(&state.runtime);
    if (run_count != 8 || state.event_count != sizeof(expected) / sizeof(expected[0]) - 1u ||
        state.first.state != WASMOS_NATIVE_COROUTINE_DEAD ||
        state.second.state != WASMOS_NATIVE_COROUTINE_DEAD ||
        state.joiner.state != WASMOS_NATIVE_COROUTINE_DEAD) {
        return __LINE__;
    }
    for (uint32_t i = 0; i < state.event_count; ++i) {
        if (state.events[i] != expected[i]) {
            return __LINE__;
        }
    }
    if (!wasmos_future_then(&state.runtime, &state.future, &state.late_continuation,
                            success_callback, error_callback, &state) ||
        wasmos_native_coroutine_run(&state.runtime) != 0 ||
        state.event_count != sizeof(expected) / sizeof(expected[0])) {
        return __LINE__;
    }
    for (uint32_t i = 0; i < state.event_count; ++i) {
        if (state.events[i] != expected[i]) {
            return __LINE__;
        }
    }
    return 0;
}

static int test_rejection_and_poll(void) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_future_continuation_t continuation = {0};
    int32_t status = 0;
    uintptr_t value = 1u;
    test_state_t state = {0};

    wasmos_native_coroutine_runtime_init(&runtime);
    wasmos_future_init(&future, &promise);
    if (!wasmos_future_then(&runtime, &future, &continuation, success_callback, error_callback,
                            &state) ||
        wasmos_future_poll(&future, &status, &value) || !wasmos_promise_reject(&promise, -23) ||
        wasmos_promise_reject(&promise, -24) || !wasmos_future_poll(&future, &status, &value) ||
        status != -23 || value != 0u) {
        return __LINE__;
    }
    if (wasmos_native_coroutine_run(&runtime) != 0 || state.event_count != 1u ||
        state.events[0] != 98u) {
        return __LINE__;
    }
    return 0;
}

static int test_future_chains(void) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t source;
    wasmos_promise_t source_promise;
    wasmos_future_t rejected;
    wasmos_promise_t rejected_promise;
    wasmos_future_t failed_callback;
    wasmos_promise_t failed_callback_promise;
    wasmos_future_continuation_t increment = {0};
    wasmos_future_continuation_t double_value = {0};
    wasmos_future_continuation_t recover = {0};
    wasmos_future_continuation_t reject = {0};
    wasmos_future_t* child;
    wasmos_future_t* grandchild;
    wasmos_future_t* recovered;
    wasmos_future_t* callback_failed;
    int32_t status = 0;
    uintptr_t value = 0;

    wasmos_native_coroutine_runtime_init(&runtime);
    wasmos_future_init(&source, &source_promise);
    child = wasmos_future_then(&runtime, &source, &increment, increment_callback, NULL, NULL);
    grandchild =
        child ? wasmos_future_then(&runtime, child, &double_value, double_callback, NULL, NULL)
              : NULL;
    if (!child || !grandchild || !wasmos_promise_resolve(&source_promise, 20u) ||
        wasmos_native_coroutine_run(&runtime) != 0 || !wasmos_future_poll(child, &status, &value) ||
        status != 0 || value != 21u || !wasmos_future_poll(grandchild, &status, &value) ||
        status != 0 || value != 42u) {
        return __LINE__;
    }

    wasmos_future_init(&rejected, &rejected_promise);
    recovered = wasmos_future_then(&runtime, &rejected, &recover, NULL, recover_callback, NULL);
    if (!recovered || !wasmos_promise_reject(&rejected_promise, -23) ||
        wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(recovered, &status, &value) || status != 0 || value != 55u) {
        return __LINE__;
    }

    wasmos_future_init(&failed_callback, &failed_callback_promise);
    callback_failed =
        wasmos_future_then(&runtime, &failed_callback, &reject, reject_callback, NULL, NULL);
    if (!callback_failed || !wasmos_promise_resolve(&failed_callback_promise, 1u) ||
        wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(callback_failed, &status, &value) || status != -41 || value != 0u) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    int rc = test_yield_await_and_join();
    if (rc == 0) {
        rc = test_rejection_and_poll();
    }
    if (rc == 0) {
        rc = test_future_chains();
    }
    if (rc != 0) {
        fprintf(stderr, "native coroutine test failed at line %d\n", rc);
        return 1;
    }
    return 0;
}
