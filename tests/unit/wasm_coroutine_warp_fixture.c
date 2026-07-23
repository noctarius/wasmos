/* Compiled to wasm32 and executed by test_warp_wasm_coroutine.cpp. */
#include <stdint.h>

#include "wasmos/coroutine_wasm.h"

typedef struct {
    int pc;
    wasmos_future_t* future;
    int status;
    uintptr_t value;
} waiter_state_t;

static int32_t waiter(void* user, uintptr_t* out_value) {
    waiter_state_t* state = user;
    if (state->pc == 0) {
        state->pc = 1;
        state->status = wasmos_future_await(state->future, &state->value);
        if (state->status == WASMOS_WASM_AWAIT_PENDING) return WASMOS_WASM_TASK_YIELDED;
    }
    state->status = wasmos_future_await(state->future, &state->value);
    if (state->status == WASMOS_WASM_AWAIT_PENDING) return WASMOS_WASM_TASK_YIELDED;
    *out_value = state->value;
    return state->status;
}

typedef struct {
    wasmos_promise_t* promise;
} resolver_state_t;

static int32_t resolver(void* user, uintptr_t* out_value) {
    resolver_state_t* state = user;
    if (!wasmos_promise_resolve(state->promise, 42u)) return -1;
    *out_value = 42u;
    return 0;
}

int32_t wasmos_coroutine_wasm_test(void) {
    wasmos_wasm_runtime_t runtime = {0};
    wasmos_wasm_coroutine_t waiting = {0}, resolving = {0};
    wasmos_future_t future;
    wasmos_promise_t promise;
    waiter_state_t waiting_state = {.future = &future};
    resolver_state_t resolving_state = {.promise = &promise};
    int32_t status = -1;
    uintptr_t value = 0;

    wasmos_wasm_runtime_init(&runtime);
    wasmos_future_init(&future, &promise);
    if (!wasmos_async_start(&runtime, &waiting, waiter, &waiting_state) ||
        !wasmos_async_start(&runtime, &resolving, resolver, &resolving_state) ||
        wasmos_wasm_coroutine_run(&runtime) != 3 || waiting_state.status != 0 ||
        waiting_state.value != 42u || !wasmos_future_poll(&waiting.completion, &status, &value) ||
        status != 0 || value != 42u) {
        return 1;
    }
    return 0;
}
