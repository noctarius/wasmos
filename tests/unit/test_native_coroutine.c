/* Runtime-behaviour tests for the single-worker native coroutine/future core. */
#include <stdint.h>

#include "test_shuffle.h"
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
    wasmos_future_t* first_completion;
    wasmos_future_t* late_child;
    int32_t late_status = -1;
    uintptr_t late_value = 0;
    int run_count;

    wasmos_native_coroutine_runtime_init(&state.runtime);
    wasmos_future_init(&state.future, &state.promise);
    if (!wasmos_future_then(&state.runtime, &state.future, &state.first_continuation,
                            success_callback, error_callback, &state)) {
        return __LINE__;
    }
    first_completion = wasmos_async_start(&state.runtime, &state.first, state.first_stack,
                                          sizeof(state.first_stack), first_entry, &state);
    if (first_completion != &state.first.completion ||
        wasmos_native_coroutine_spawn(&state.runtime, &state.second, state.second_stack,
                                      sizeof(state.second_stack), second_entry, &state) != 0 ||
        wasmos_native_coroutine_spawn(&state.runtime, &state.joiner, state.joiner_stack,
                                      sizeof(state.joiner_stack), joiner_entry, &state) != 0) {
        return __LINE__;
    }
    run_count = wasmos_native_coroutine_run(&state.runtime);
    if (run_count != 7 || state.event_count != sizeof(expected) / sizeof(expected[0]) - 1u ||
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
    late_child = wasmos_future_then(&state.runtime, &state.future, &state.late_continuation,
                                    success_callback, error_callback, &state);
    if (!late_child || wasmos_native_coroutine_run(&state.runtime) != 0 ||
        state.event_count != sizeof(expected) / sizeof(expected[0]) ||
        !wasmos_future_poll(late_child, &late_status, &late_value) || late_status != 0 ||
        late_value != 42u) {
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
    wasmos_future_t forwarded;
    wasmos_promise_t forwarded_promise;
    wasmos_future_t forwarded_error;
    wasmos_promise_t forwarded_error_promise;
    wasmos_future_continuation_t increment = {0};
    wasmos_future_continuation_t double_value = {0};
    wasmos_future_continuation_t recover = {0};
    wasmos_future_continuation_t reject = {0};
    wasmos_future_continuation_t forward = {0};
    wasmos_future_continuation_t forward_error = {0};
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

    wasmos_future_init(&forwarded, &forwarded_promise);
    child = wasmos_future_then(&runtime, &forwarded, &forward, NULL, NULL, NULL);
    if (!child || !wasmos_promise_resolve(&forwarded_promise, 88u) ||
        wasmos_native_coroutine_run(&runtime) != 0 || !wasmos_future_poll(child, &status, &value) ||
        status != 0 || value != 88u) {
        return __LINE__;
    }

    wasmos_future_init(&forwarded_error, &forwarded_error_promise);
    child = wasmos_future_then(&runtime, &forwarded_error, &forward_error, NULL, NULL, NULL);
    if (!child || !wasmos_promise_reject(&forwarded_error_promise, -77) ||
        wasmos_native_coroutine_run(&runtime) != 0 || !wasmos_future_poll(child, &status, &value) ||
        status != -77 || value != 0u) {
        return __LINE__;
    }
    return 0;
}

enum {
    TEST_WAITER_COUNT = 3,
    TEST_STRESS_COUNT = 12,
    TEST_STRESS_YIELDS = 24,
};

typedef struct waiter_state waiter_state_t;

typedef struct {
    waiter_state_t* state;
    size_t index;
} waiter_arg_t;

struct waiter_state {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_native_coroutine_t coroutines[TEST_WAITER_COUNT];
    waiter_arg_t args[TEST_WAITER_COUNT];
    uint8_t stacks[TEST_WAITER_COUNT][4096];
    int32_t statuses[TEST_WAITER_COUNT];
    uintptr_t values[TEST_WAITER_COUNT];
};

static void waiter_entry(void* arg) {
    waiter_arg_t* waiter = arg;
    waiter_state_t* state = waiter->state;
    size_t index = waiter->index;

    state->statuses[index] = wasmos_future_await(&state->future, &state->values[index]);
}

static int test_multiple_waiters(void) {
    waiter_state_t success = {0};
    waiter_state_t failure = {0};

    wasmos_native_coroutine_runtime_init(&success.runtime);
    wasmos_future_init(&success.future, &success.promise);
    for (size_t i = 0; i < TEST_WAITER_COUNT; ++i) {
        success.args[i] = (waiter_arg_t){.state = &success, .index = i};
        if (!wasmos_async_start(&success.runtime, &success.coroutines[i], success.stacks[i],
                                sizeof(success.stacks[i]), waiter_entry, &success.args[i])) {
            return __LINE__;
        }
    }
    if (wasmos_native_coroutine_run(&success.runtime) != TEST_WAITER_COUNT ||
        !wasmos_promise_resolve(&success.promise, 77u) ||
        wasmos_native_coroutine_run(&success.runtime) != TEST_WAITER_COUNT) {
        return __LINE__;
    }
    for (size_t i = 0; i < TEST_WAITER_COUNT; ++i) {
        if (success.statuses[i] != 0 || success.values[i] != 77u ||
            success.coroutines[i].state != WASMOS_NATIVE_COROUTINE_DEAD) {
            return __LINE__;
        }
    }

    wasmos_native_coroutine_runtime_init(&failure.runtime);
    wasmos_future_init(&failure.future, &failure.promise);
    for (size_t i = 0; i < TEST_WAITER_COUNT; ++i) {
        failure.args[i] = (waiter_arg_t){.state = &failure, .index = i};
        if (!wasmos_async_start(&failure.runtime, &failure.coroutines[i], failure.stacks[i],
                                sizeof(failure.stacks[i]), waiter_entry, &failure.args[i])) {
            return __LINE__;
        }
    }
    if (wasmos_native_coroutine_run(&failure.runtime) != TEST_WAITER_COUNT ||
        !wasmos_promise_reject(&failure.promise, -31) ||
        wasmos_native_coroutine_run(&failure.runtime) != TEST_WAITER_COUNT) {
        return __LINE__;
    }
    for (size_t i = 0; i < TEST_WAITER_COUNT; ++i) {
        if (failure.statuses[i] != -31 || failure.values[i] != 0u ||
            failure.coroutines[i].state != WASMOS_NATIVE_COROUTINE_DEAD) {
            return __LINE__;
        }
    }
    return 0;
}

typedef struct join_many_state join_many_state_t;

typedef struct {
    join_many_state_t* state;
    size_t index;
} join_many_arg_t;

struct join_many_state {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_native_coroutine_t target;
    wasmos_native_coroutine_t joiners[TEST_WAITER_COUNT];
    join_many_arg_t args[TEST_WAITER_COUNT];
    uint8_t target_stack[4096];
    uint8_t joiner_stacks[TEST_WAITER_COUNT][4096];
    int32_t statuses[TEST_WAITER_COUNT];
    int32_t results[TEST_WAITER_COUNT];
};

static void join_many_target(void* arg) {
    (void)arg;
    wasmos_native_coroutine_yield();
    wasmos_native_coroutine_exit(23);
}

static void join_many_entry(void* arg) {
    join_many_arg_t* joiner = arg;
    join_many_state_t* state = joiner->state;
    state->statuses[joiner->index] =
        wasmos_native_coroutine_join(&state->target, &state->results[joiner->index]);
}

static int test_multiple_joiners_and_return(void) {
    join_many_state_t state = {0};
    wasmos_future_t* completion;
    int32_t returned = 1;

    wasmos_native_coroutine_runtime_init(&state.runtime);
    completion = wasmos_async_start(&state.runtime, &state.target, state.target_stack,
                                    sizeof(state.target_stack), join_many_target, NULL);
    if (!completion) {
        return __LINE__;
    }
    for (size_t i = 0; i < TEST_WAITER_COUNT; ++i) {
        state.args[i] = (join_many_arg_t){.state = &state, .index = i};
        if (!wasmos_async_start(&state.runtime, &state.joiners[i], state.joiner_stacks[i],
                                sizeof(state.joiner_stacks[i]), join_many_entry, &state.args[i])) {
            return __LINE__;
        }
    }
    if (wasmos_native_coroutine_run(&state.runtime) != 8 ||
        wasmos_native_coroutine_join(&state.target, &returned) != 0 || returned != 23) {
        return __LINE__;
    }
    for (size_t i = 0; i < TEST_WAITER_COUNT; ++i) {
        if (state.statuses[i] != 0 || state.results[i] != 23) {
            return __LINE__;
        }
    }
    if (!wasmos_future_poll(completion, NULL, NULL)) {
        return __LINE__;
    }
    return 0;
}

typedef struct {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_native_coroutine_t coroutines[TEST_STRESS_COUNT];
    uint8_t stacks[TEST_STRESS_COUNT][4096];
    uint32_t completed;
} stress_state_t;

static void stress_entry(void* arg) {
    stress_state_t* state = arg;
    for (uint32_t i = 0; i < TEST_STRESS_YIELDS; ++i) {
        wasmos_native_coroutine_yield();
    }
    state->completed++;
}

static int test_scheduler_stress(void) {
    stress_state_t state = {0};

    wasmos_native_coroutine_runtime_init(&state.runtime);
    for (size_t i = 0; i < TEST_STRESS_COUNT; ++i) {
        if (!wasmos_async_start(&state.runtime, &state.coroutines[i], state.stacks[i],
                                sizeof(state.stacks[i]), stress_entry, &state)) {
            return __LINE__;
        }
    }
    if (wasmos_native_coroutine_run(&state.runtime) !=
            TEST_STRESS_COUNT * (TEST_STRESS_YIELDS + 1) ||
        state.completed != TEST_STRESS_COUNT) {
        return __LINE__;
    }
    for (size_t i = 0; i < TEST_STRESS_COUNT; ++i) {
        int32_t status = -1;
        uintptr_t value = 1u;
        if (!wasmos_future_poll(&state.coroutines[i].completion, &status, &value) || status != 0 ||
            value != 0u) {
            return __LINE__;
        }
    }
    return 0;
}

typedef struct {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_future_t* settled;
    wasmos_future_continuation_t nested;
    wasmos_future_t* nested_child;
    int32_t reentrant_run_result;
} reentrant_state_t;

static int32_t reentrant_nested(void* user, uintptr_t value, uintptr_t* out_value) {
    (void)user;
    *out_value = value + 1u;
    return 0;
}

static int32_t reentrant_callback(void* user, uintptr_t value, uintptr_t* out_value) {
    reentrant_state_t* state = user;
    state->reentrant_run_result = wasmos_native_coroutine_run(state->runtime);
    state->nested_child = wasmos_future_then(state->runtime, state->settled, &state->nested,
                                             reentrant_nested, NULL, NULL);
    *out_value = value;
    return state->nested_child ? 0 : -1;
}

static int test_reentrant_callbacks_and_contracts(void) {
    wasmos_native_coroutine_runtime_t runtime = {0};
    wasmos_native_coroutine_runtime_t other_runtime = {0};
    wasmos_future_t source;
    wasmos_promise_t source_promise;
    wasmos_future_t settled;
    wasmos_promise_t settled_promise;
    wasmos_future_continuation_t continuation = {0};
    wasmos_future_continuation_t other_continuation = {0};
    reentrant_state_t state = {.runtime = &runtime, .settled = &settled};
    wasmos_native_coroutine_t coroutine = {0};
    uint8_t stack[1024];

    wasmos_native_coroutine_runtime_init(&runtime);
    wasmos_native_coroutine_runtime_init(&other_runtime);
    wasmos_future_init(&source, &source_promise);
    wasmos_future_init(&settled, &settled_promise);
    if (wasmos_native_coroutine_run(NULL) != -1 || wasmos_future_await(&source, NULL) != -1 ||
        wasmos_promise_reject(&source_promise, 0) ||
        wasmos_async_start(NULL, &coroutine, stack, sizeof(stack), stress_entry, NULL) ||
        wasmos_native_coroutine_spawn(&runtime, &coroutine, stack, sizeof(stack) - 1u, stress_entry,
                                      NULL) != -1 ||
        !wasmos_promise_resolve(&settled_promise, 9u) ||
        !wasmos_future_then(&runtime, &source, &continuation, reentrant_callback, NULL, &state) ||
        wasmos_future_then(&other_runtime, &source, &other_continuation, NULL, NULL, NULL) ||
        !wasmos_promise_resolve(&source_promise, 7u) ||
        wasmos_native_coroutine_run(&runtime) != 0 || state.reentrant_run_result != -1 ||
        !state.nested_child) {
        return __LINE__;
    }
    uintptr_t value = 0;
    if (!wasmos_future_poll(state.nested_child, NULL, &value) || value != 10u) {
        return __LINE__;
    }
    return 0;
}

static int test_future_race_and_all(void) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t first;
    wasmos_future_t second;
    wasmos_future_t third;
    wasmos_promise_t first_promise;
    wasmos_promise_t second_promise;
    wasmos_promise_t third_promise;
    wasmos_future_group_t race_group = {0};
    wasmos_future_group_t failed_race_group = {0};
    wasmos_future_group_t all_group = {0};
    wasmos_future_group_t failed_all_group = {0};
    wasmos_future_continuation_t race_continuations[3] = {0};
    wasmos_future_continuation_t failed_race_continuations[2] = {0};
    wasmos_future_continuation_t all_continuations[3] = {0};
    wasmos_future_continuation_t failed_all_continuations[3] = {0};
    uintptr_t values[3] = {0};
    uintptr_t failed_values[3] = {0};
    wasmos_future_t* result;
    int32_t status = 0;
    uintptr_t value = 0;

    wasmos_native_coroutine_runtime_init(&runtime);
    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    wasmos_future_init(&third, &third_promise);
    result = WASMOS_FUTURE_RACE(&runtime, &race_group, race_continuations, &first, &second, &third);
    if (!result || !wasmos_promise_resolve(&second_promise, 22u) ||
        !wasmos_promise_reject(&first_promise, -9) ||
        !wasmos_promise_resolve(&third_promise, 33u) ||
        wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || status != 0 || value != 22u ||
        race_group.active) {
        return __LINE__;
    }

    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    result = WASMOS_FUTURE_RACE(&runtime, &failed_race_group, failed_race_continuations, &first,
                                &second);
    /* A fail-fast settles the group immediately, marks it inactive, and unlinks
     * the still-pending source so its later completion is a no-op. */
    if (!result || !wasmos_promise_reject(&first_promise, -13) ||
        wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || status != -13 ||
        !failed_race_group.settled || failed_race_group.active ||
        failed_race_continuations[1].active || second.continuations != NULL ||
        !wasmos_promise_resolve(&second_promise, 2u) ||
        wasmos_native_coroutine_run(&runtime) != 0 || failed_race_group.active) {
        return __LINE__;
    }

    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    wasmos_future_init(&third, &third_promise);
    result =
        WASMOS_FUTURE_ALL(&runtime, &all_group, values, all_continuations, &first, &second, &third);
    if (!result || !wasmos_promise_resolve(&third_promise, 3u) ||
        !wasmos_promise_resolve(&first_promise, 1u) ||
        !wasmos_promise_resolve(&second_promise, 2u) ||
        wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || status != 0 || value != (uintptr_t)values ||
        values[0] != 1u || values[1] != 2u || values[2] != 3u || all_group.active) {
        return __LINE__;
    }

    wasmos_future_init(&first, &first_promise);
    wasmos_future_init(&second, &second_promise);
    wasmos_future_init(&third, &third_promise);
    result = WASMOS_FUTURE_ALL(&runtime, &failed_all_group, failed_values, failed_all_continuations,
                               &first, &second, &third);
    if (!result || !wasmos_promise_reject(&second_promise, -44) ||
        wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || status != -44 ||
        !failed_all_group.settled || failed_all_group.active ||
        failed_all_continuations[0].active || failed_all_continuations[2].active ||
        first.continuations != NULL || third.continuations != NULL ||
        !wasmos_promise_resolve(&first_promise, 1u) ||
        !wasmos_promise_resolve(&third_promise, 3u) || wasmos_native_coroutine_run(&runtime) != 0 ||
        failed_all_group.active || wasmos_future_race(&runtime, NULL, NULL, 0, NULL) ||
        wasmos_future_all(&runtime, NULL, NULL, 0, NULL, NULL)) {
        return __LINE__;
    }
    return 0;
}

static void respawn_entry(void* arg) {
    uint32_t* runs = arg;
    if (runs) {
        (*runs)++;
    }
}

static int test_respawn_guard(void) {
    wasmos_native_coroutine_runtime_t runtime = {0};
    wasmos_native_coroutine_t coroutine = {0};
    uint32_t runs = 0;
    uint8_t stack[1024];

    wasmos_native_coroutine_runtime_init(&runtime);

    /* A fresh (NEW) record spawns. */
    if (wasmos_native_coroutine_spawn(&runtime, &coroutine, stack, sizeof(stack), respawn_entry,
                                      &runs) != 0) {
        return __LINE__;
    }
    /* Re-spawning a queued record must be rejected so it cannot be linked into
     * the ready list twice. */
    if (wasmos_native_coroutine_spawn(&runtime, &coroutine, stack, sizeof(stack), respawn_entry,
                                      &runs) != -1) {
        return __LINE__;
    }
    /* Draining runs it exactly once and leaves it DEAD. */
    if (wasmos_native_coroutine_run(&runtime) != 1 || runs != 1u ||
        coroutine.state != WASMOS_NATIVE_COROUTINE_DEAD) {
        return __LINE__;
    }
    /* A DEAD record may be reused. */
    if (wasmos_native_coroutine_spawn(&runtime, &coroutine, stack, sizeof(stack), respawn_entry,
                                      &runs) != 0 ||
        wasmos_native_coroutine_run(&runtime) != 1 || runs != 2u) {
        return __LINE__;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Parity with tests/unit/test_wasm_coroutine.c and test_as_coroutine.ts.
 * Future/promise semantics are shared across the runtimes by design (see
 * docs/architecture/32-coroutines-futures-promises.md), so the group scenarios
 * below apply to all three and are kept identical.
 * ---------------------------------------------------------------------- */

#define RACE_MAX 4u

/* Race with N candidates that ALL settle, run once per winning position, which
 * pins the outcome as independent of WHICH position wins.
 *
 * Scope: this does NOT distinguish the head/middle/tail branches of the
 * dispatch-queue removal. Removing only the head passes as well, because
 * continuation_cancel nulls the node's next regardless -- truncating the list
 * anyway -- and nulls its future, so a surviving stale entry dispatches as a
 * no-op. */
static int native_race_winner_case(size_t winner, size_t count) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t futures[RACE_MAX];
    wasmos_promise_t promises[RACE_MAX];
    wasmos_future_continuation_t continuations[RACE_MAX] = {0};
    wasmos_future_t* inputs[RACE_MAX];
    wasmos_future_group_t group = {0};
    wasmos_future_t* result;
    int32_t status = 0;
    uintptr_t value = 0;
    const uintptr_t expected = (uintptr_t)(100u + winner);

    wasmos_native_coroutine_runtime_init(&runtime);
    for (size_t i = 0; i < count; ++i) {
        wasmos_future_init(&futures[i], &promises[i]);
        inputs[i] = &futures[i];
    }
    result = wasmos_future_race(&runtime, &group, inputs, count, continuations);
    if (!result)
        return __LINE__;

    /* Winner settles first, so it dispatches first and wins; the rest settle
     * before the drain and queue behind it, making them losers that were
     * already enqueued rather than merely pending. */
    if (!wasmos_promise_resolve(&promises[winner], expected))
        return __LINE__;
    for (size_t i = count; i-- > 0;) {
        if (i == winner)
            continue;
        if (!wasmos_promise_resolve(&promises[i], (uintptr_t)(900u + i)))
            return __LINE__;
    }
    if (!runtime.continuation_head || wasmos_native_coroutine_run(&runtime) != 0)
        return __LINE__;
    if (!wasmos_future_poll(result, &status, &value) || status != 0 || value != expected ||
        !group.settled || group.active) {
        return __LINE__;
    }
    /* The losers were DISCARDED, not merely outvoted: exactly one group
     * callback ran. Without this a runtime that leaves them queued and lets
     * them run -- their resolve refused because the group already settled --
     * is indistinguishable from one that cancels them. */
    if (group.completed != 1u)
        return __LINE__;
    for (size_t i = 0; i < count; ++i) {
        if (continuations[i].active || futures[i].continuations != NULL)
            return __LINE__;
    }
    if (runtime.continuation_head || runtime.continuation_tail)
        return __LINE__;
    if (wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || value != expected) {
        return __LINE__;
    }
    return 0;
}

static int native_race_reject_case(size_t loser, size_t count) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t futures[RACE_MAX];
    wasmos_promise_t promises[RACE_MAX];
    wasmos_future_continuation_t continuations[RACE_MAX] = {0};
    wasmos_future_t* inputs[RACE_MAX];
    wasmos_future_group_t group = {0};
    wasmos_future_t* result;
    int32_t status = 0;
    uintptr_t value = 0;
    const int32_t expected = -40 - (int32_t)loser;

    wasmos_native_coroutine_runtime_init(&runtime);
    for (size_t i = 0; i < count; ++i) {
        wasmos_future_init(&futures[i], &promises[i]);
        inputs[i] = &futures[i];
    }
    result = wasmos_future_race(&runtime, &group, inputs, count, continuations);
    if (!result || !wasmos_promise_reject(&promises[loser], expected))
        return __LINE__;
    for (size_t i = count; i-- > 0;) {
        if (i == loser)
            continue;
        if (!wasmos_promise_resolve(&promises[i], (uintptr_t)(900u + i)))
            return __LINE__;
    }
    if (wasmos_native_coroutine_run(&runtime) != 0)
        return __LINE__;
    if (!wasmos_future_poll(result, &status, &value) || status != expected || !group.settled ||
        group.active || group.completed != 1u) {
        return __LINE__;
    }
    for (size_t i = 0; i < count; ++i) {
        if (continuations[i].active || futures[i].continuations != NULL)
            return __LINE__;
    }
    if (runtime.continuation_head || runtime.continuation_tail)
        return __LINE__;
    return 0;
}

static int native_all_reject_case(size_t rejecter, size_t count) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t futures[RACE_MAX];
    wasmos_promise_t promises[RACE_MAX];
    wasmos_future_continuation_t continuations[RACE_MAX] = {0};
    wasmos_future_t* inputs[RACE_MAX];
    uintptr_t values[RACE_MAX] = {0};
    wasmos_future_group_t group = {0};
    wasmos_future_t* result;
    int32_t status = 0;
    uintptr_t value = 0;
    const int32_t expected = -60 - (int32_t)rejecter;

    wasmos_native_coroutine_runtime_init(&runtime);
    for (size_t i = 0; i < count; ++i) {
        wasmos_future_init(&futures[i], &promises[i]);
        inputs[i] = &futures[i];
    }
    result = wasmos_future_all(&runtime, &group, inputs, count, values, continuations);
    if (!result || !wasmos_promise_reject(&promises[rejecter], expected))
        return __LINE__;
    if (wasmos_native_coroutine_run(&runtime) != 0)
        return __LINE__;
    if (!wasmos_future_poll(result, &status, &value) || status != expected || !group.settled ||
        group.active || group.completed != 1u) {
        return __LINE__;
    }
    for (size_t i = 0; i < count; ++i) {
        if (continuations[i].active)
            return __LINE__;
        if (i != rejecter && futures[i].continuations != NULL)
            return __LINE__;
    }
    for (size_t i = 0; i < count; ++i) {
        if (i == rejecter)
            continue;
        if (!wasmos_promise_resolve(&promises[i], 999u))
            return __LINE__;
    }
    if (wasmos_native_coroutine_run(&runtime) != 0 ||
        !wasmos_future_poll(result, &status, &value) || status != expected) {
        return __LINE__;
    }
    return 0;
}

static int test_race_and_all_every_position(void) {
    int rc = 0;
    for (size_t winner = 0; winner < 3u; ++winner) {
        rc = native_race_winner_case(winner, 3u);
        if (rc != 0)
            return rc;
    }
    /* A fourth candidate, so more than two losers are abandoned at once. */
    rc = native_race_winner_case(1u, 4u);
    if (rc != 0)
        return rc;
    for (size_t loser = 0; loser < 3u; ++loser) {
        rc = native_race_reject_case(loser, 3u);
        if (rc != 0)
            return rc;
    }
    for (size_t rejecter = 0; rejecter < 3u; ++rejecter) {
        rc = native_all_reject_case(rejecter, 3u);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* Registering on an ALREADY-SETTLED future must still defer to the runtime
 * rather than dispatch inline from wasmos_future_then. Every other case here
 * registers BEFORE settling, so this is the only one covering that branch. */
static int test_then_on_settled_future_defers(void) {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_future_t settled;
    wasmos_promise_t settled_promise;
    wasmos_future_continuation_t continuation = {0};
    wasmos_future_t* child;
    test_state_t state = {0};
    int32_t status = 0;
    uintptr_t value = 0;

    wasmos_native_coroutine_runtime_init(&runtime);
    wasmos_future_init(&settled, &settled_promise);
    if (!wasmos_promise_resolve(&settled_promise, 70u))
        return __LINE__;
    child = wasmos_future_then(&runtime, &settled, &continuation, success_callback, NULL, &state);
    if (!child || state.event_count != 0u)
        return __LINE__;
    if (wasmos_native_coroutine_run(&runtime) != 0 || state.event_count != 1u)
        return __LINE__;
    if (!wasmos_future_poll(child, &status, &value) || status != 0)
        return __LINE__;
    return 0;
}

/* Randomized order: a case that leaks state must not be able to make its
 * neighbour pass. The seed is printed before the first case runs and repeated
 * on failure; WASMOS_TEST_SEED replays that order. See test_shuffle.h. */
static const wasmos_test_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_yield_await_and_join),
    WASMOS_TEST_CASE(test_rejection_and_poll),
    WASMOS_TEST_CASE(test_future_chains),
    WASMOS_TEST_CASE(test_multiple_waiters),
    WASMOS_TEST_CASE(test_multiple_joiners_and_return),
    WASMOS_TEST_CASE(test_scheduler_stress),
    WASMOS_TEST_CASE(test_reentrant_callbacks_and_contracts),
    WASMOS_TEST_CASE(test_future_race_and_all),
    WASMOS_TEST_CASE(test_respawn_guard),
    WASMOS_TEST_CASE(test_race_and_all_every_position),
    WASMOS_TEST_CASE(test_then_on_settled_future_defers),
};

int main(void) {
    return wasmos_test_run_all(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));
}
