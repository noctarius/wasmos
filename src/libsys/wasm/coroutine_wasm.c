#include "wasmos/coroutine_wasm.h"

/* A WASM guest has one cooperative runtime per module.  Keep the active
 * runtime in ordinary module-static storage while a task resume function is
 * executing; a pending future is allowed to acquire that runtime on first
 * await. */
static wasmos_wasm_runtime_t* g_current_runtime;

static void coroutine_enqueue(wasmos_wasm_runtime_t* runtime, wasmos_wasm_coroutine_t* coroutine) {
    coroutine->next = NULL;
    if (runtime->ready_tail)
        runtime->ready_tail->next = coroutine;
    else
        runtime->ready_head = coroutine;
    runtime->ready_tail = coroutine;
}

static wasmos_wasm_coroutine_t* coroutine_dequeue(wasmos_wasm_runtime_t* runtime) {
    wasmos_wasm_coroutine_t* coroutine = runtime->ready_head;
    if (!coroutine)
        return NULL;
    runtime->ready_head = coroutine->next;
    if (!runtime->ready_head)
        runtime->ready_tail = NULL;
    coroutine->next = NULL;
    return coroutine;
}

static void continuation_enqueue(wasmos_wasm_runtime_t* runtime,
                                 wasmos_future_continuation_t* continuation) {
    continuation->next = NULL;
    if (runtime->continuation_tail)
        runtime->continuation_tail->next = continuation;
    else
        runtime->continuation_head = continuation;
    runtime->continuation_tail = continuation;
}

static wasmos_future_continuation_t* continuation_dequeue(wasmos_wasm_runtime_t* runtime) {
    wasmos_future_continuation_t* continuation = runtime->continuation_head;
    if (!continuation)
        return NULL;
    runtime->continuation_head = continuation->next;
    if (!runtime->continuation_head)
        runtime->continuation_tail = NULL;
    continuation->next = NULL;
    return continuation;
}

void wasmos_wasm_runtime_init(wasmos_wasm_runtime_t* runtime) {
    if (runtime)
        *runtime = (wasmos_wasm_runtime_t){0};
}

void wasmos_future_init(wasmos_future_t* future, wasmos_promise_t* promise) {
    if (!future || !promise)
        return;
    *future = (wasmos_future_t){.state = WASMOS_FUTURE_PENDING};
    promise->future = future;
}

wasmos_future_t* wasmos_async_start(wasmos_wasm_runtime_t* runtime,
                                    wasmos_wasm_coroutine_t* coroutine,
                                    wasmos_wasm_task_resume_fn resume, void* user) {
    if (!runtime || !coroutine || !resume ||
        (coroutine->state != WASMOS_WASM_COROUTINE_NEW &&
         coroutine->state != WASMOS_WASM_COROUTINE_DEAD))
        return NULL;
    *coroutine = (wasmos_wasm_coroutine_t){
        .runtime = runtime, .resume = resume, .user = user, .state = WASMOS_WASM_COROUTINE_READY};
    wasmos_future_init(&coroutine->completion, &coroutine->completion_promise);
    coroutine->completion.runtime = runtime;
    coroutine_enqueue(runtime, coroutine);
    return &coroutine->completion;
}

static void coroutine_ready(wasmos_wasm_coroutine_t* coroutine) {
    if (coroutine && coroutine->state == WASMOS_WASM_COROUTINE_WAITING) {
        coroutine->state = WASMOS_WASM_COROUTINE_READY;
        coroutine_enqueue(coroutine->runtime, coroutine);
    }
}

static bool promise_complete(wasmos_promise_t* promise, int32_t status, uintptr_t value) {
    wasmos_future_t* future;
    wasmos_wasm_coroutine_t* waiter;
    wasmos_future_continuation_t* continuation;
    if (!promise || !(future = promise->future) || future->state != WASMOS_FUTURE_PENDING)
        return false;
    future->state = status == 0 ? WASMOS_FUTURE_READY : WASMOS_FUTURE_FAILED;
    future->status = status;
    future->value = value;
    waiter = future->waiters;
    future->waiters = NULL;
    while (waiter) {
        wasmos_wasm_coroutine_t* next = waiter->wait_next;
        waiter->wait_next = NULL;
        coroutine_ready(waiter);
        waiter = next;
    }
    continuation = future->continuations;
    future->continuations = NULL;
    while (continuation) {
        wasmos_future_continuation_t* next = continuation->next;
        if (continuation->active)
            continuation_enqueue(future->runtime, continuation);
        continuation = next;
    }
    return true;
}

bool wasmos_promise_resolve(wasmos_promise_t* promise, uintptr_t value) {
    return promise_complete(promise, 0, value);
}

bool wasmos_promise_reject(wasmos_promise_t* promise, int32_t status) {
    return status < 0 && promise_complete(promise, status, 0);
}

bool wasmos_future_poll(const wasmos_future_t* future, int32_t* out_status, uintptr_t* out_value) {
    if (!future || future->state == WASMOS_FUTURE_PENDING)
        return false;
    if (out_status)
        *out_status = future->status;
    if (out_value)
        *out_value = future->value;
    return true;
}

int wasmos_future_await(wasmos_future_t* future, uintptr_t* out_value) {
    wasmos_wasm_runtime_t* runtime;
    wasmos_wasm_coroutine_t* coroutine;
    if (!future)
        return -1;
    if (future->state != WASMOS_FUTURE_PENDING) {
        if (out_value)
            *out_value = future->value;
        return future->status;
    }
    runtime = future->runtime ? future->runtime : g_current_runtime;
    coroutine = runtime ? runtime->current : NULL;
    if (!coroutine || coroutine->state != WASMOS_WASM_COROUTINE_RUNNING ||
        (future->runtime && future->runtime != runtime)) {
        return -1;
    }
    future->runtime = runtime;
    coroutine->wait_next = future->waiters;
    future->waiters = coroutine;
    coroutine->state = WASMOS_WASM_COROUTINE_WAITING;
    return WASMOS_WASM_AWAIT_PENDING;
}

int wasmos_wasm_coroutine_yield(void) {
    return WASMOS_WASM_TASK_YIELDED;
}

int wasmos_wasm_coroutine_join(wasmos_wasm_coroutine_t* coroutine, int32_t* out_result) {
    uintptr_t value = 0;
    int status = coroutine ? wasmos_future_await(&coroutine->completion, &value) : -1;
    if (status == 0 && out_result)
        *out_result = (int32_t)(intptr_t)value;
    return status;
}

static int32_t flat_forward_success(void* user, uintptr_t value, uintptr_t* out_value);
static int32_t flat_forward_error(void* user, int32_t status, uintptr_t* out_value);

static void continuation_dispatch(wasmos_future_continuation_t* continuation) {
    wasmos_future_t* future = continuation->future;
    uintptr_t value = 0;
    int32_t status;
    continuation->active = false;
    continuation->future = NULL;
    if (!future || future->state == WASMOS_FUTURE_PENDING)
        return;
    if (future->status == 0) {
        status = continuation->on_success
                     ? continuation->on_success(continuation->user, future->value, &value)
                     : 0;
        if (!continuation->on_success)
            value = future->value;
    } else {
        status = continuation->on_error
                     ? continuation->on_error(continuation->user, future->status, &value)
                     : future->status;
    }
    if (status == WASMOS_FUTURE_CHAIN_NEXT && continuation->group) {
        wasmos_future_t* next = (wasmos_future_t*)value;
        wasmos_future_continuation_t* adopt = (wasmos_future_continuation_t*)continuation->group;
        continuation->group = NULL;
        if (!next ||
            !wasmos_future_then(continuation->child.runtime, next, adopt, flat_forward_success,
                                flat_forward_error, &continuation->child_promise))
            (void)wasmos_promise_reject(&continuation->child_promise, -1);
    } else if (status == 0)
        (void)wasmos_promise_resolve(&continuation->child_promise, value);
    else
        (void)wasmos_promise_reject(&continuation->child_promise, status < 0 ? status : -1);
}

int wasmos_wasm_coroutine_run_budget(wasmos_wasm_runtime_t* runtime, size_t budget) {
    int resumed = 0;
    if (!runtime || runtime->running)
        return -1;
    runtime->running = true;
    for (;;) {
        wasmos_wasm_coroutine_t* coroutine;
        if (budget != 0 && (coroutine = coroutine_dequeue(runtime)) != NULL) {
            uintptr_t value = 0;
            int32_t status;
            runtime->current = coroutine;
            coroutine->state = WASMOS_WASM_COROUTINE_RUNNING;
            resumed++;
            budget--;
            g_current_runtime = runtime;
            status = coroutine->resume(coroutine->user, &value);
            g_current_runtime = NULL;
            runtime->current = NULL;
            if (status == WASMOS_WASM_TASK_YIELDED) {
                if (coroutine->state == WASMOS_WASM_COROUTINE_RUNNING) {
                    coroutine->state = WASMOS_WASM_COROUTINE_READY;
                    coroutine_enqueue(runtime, coroutine);
                }
            } else {
                coroutine->result = status;
                coroutine->state = WASMOS_WASM_COROUTINE_DEAD;
                if (status == 0)
                    (void)wasmos_promise_resolve(&coroutine->completion_promise, value);
                else
                    (void)wasmos_promise_reject(&coroutine->completion_promise, status);
            }
            continue;
        }
        wasmos_future_continuation_t* continuation = continuation_dequeue(runtime);
        if (!continuation)
            break;
        continuation_dispatch(continuation);
    }
    runtime->running = false;
    return resumed;
}

int wasmos_wasm_coroutine_run(wasmos_wasm_runtime_t* runtime) {
    return wasmos_wasm_coroutine_run_budget(runtime, (size_t)-1);
}

wasmos_future_t* wasmos_future_then(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                    wasmos_future_continuation_t* continuation,
                                    wasmos_future_success_fn_t on_success,
                                    wasmos_future_error_fn_t on_error, void* user) {
    if (!runtime || !future || !continuation || continuation->active ||
        (future->runtime && future->runtime != runtime))
        return NULL;
    future->runtime = runtime;
    *continuation = (wasmos_future_continuation_t){.future = future,
                                                   .on_success = on_success,
                                                   .on_error = on_error,
                                                   .user = user,
                                                   .active = true};
    wasmos_future_init(&continuation->child, &continuation->child_promise);
    continuation->child.runtime = runtime;
    if (future->state == WASMOS_FUTURE_PENDING) {
        continuation->next = future->continuations;
        future->continuations = continuation;
    } else
        continuation_enqueue(runtime, continuation);
    return &continuation->child;
}

static int32_t flat_forward_success(void* user, uintptr_t value, uintptr_t* out_value) {
    wasmos_promise_t* promise = user;
    if (!promise || !wasmos_promise_resolve(promise, value))
        return -1;
    if (out_value)
        *out_value = value;
    return 0;
}
static int32_t flat_forward_error(void* user, int32_t status, uintptr_t* out_value) {
    wasmos_promise_t* promise = user;
    if (!promise || !wasmos_promise_reject(promise, status))
        return -1;
    if (out_value)
        *out_value = 0;
    return status;
}

wasmos_future_t* wasmos_future_then_flat(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                         wasmos_future_continuation_t* continuation,
                                         wasmos_future_continuation_t* adopt_continuation,
                                         wasmos_future_success_fn_t on_success,
                                         wasmos_future_error_fn_t on_error, void* user) {
    wasmos_future_t* child;
    if (!adopt_continuation)
        return NULL;
    child = wasmos_future_then(runtime, future, continuation, on_success, on_error, user);
    if (!child)
        return NULL;
    continuation->group = (wasmos_future_group_t*)adopt_continuation;
    return child;
}

static void group_callback_complete(wasmos_future_group_t* group) {
    group->completed++;
    if (group->completed == group->count)
        group->active = false;
}

static int32_t group_success(void* user, uintptr_t value, uintptr_t* out_value) {
    wasmos_future_continuation_t* continuation = user;
    wasmos_future_group_t* group = continuation ? continuation->group : NULL;
    if (!group || !group->active)
        return -1;
    if (group->kind == WASMOS_FUTURE_GROUP_RACE) {
        if (!group->settled) {
            group->settled = true;
            (void)wasmos_promise_resolve(&group->promise, value);
        }
    } else {
        group->values[continuation->group_index] = value;
        if (!group->settled && group->completed + 1u == group->count) {
            group->settled = true;
            (void)wasmos_promise_resolve(&group->promise, (uintptr_t)group->values);
        }
    }
    group_callback_complete(group);
    if (out_value)
        *out_value = value;
    return 0;
}

static int32_t group_error(void* user, int32_t status, uintptr_t* out_value) {
    wasmos_future_continuation_t* continuation = user;
    wasmos_future_group_t* group = continuation ? continuation->group : NULL;
    if (!group || !group->active || status >= 0)
        return -1;
    if (!group->settled) {
        group->settled = true;
        (void)wasmos_promise_reject(&group->promise, status);
    }
    group_callback_complete(group);
    if (out_value)
        *out_value = 0;
    return status;
}

static wasmos_future_t*
future_group_start(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                   wasmos_future_t* const* inputs, size_t count, uintptr_t* values,
                   wasmos_future_continuation_t* continuations, wasmos_future_group_kind_t kind) {
    if (!runtime || !group || !inputs || !continuations || count == 0 ||
        (kind == WASMOS_FUTURE_GROUP_ALL && !values))
        return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (!inputs[i] || continuations[i].active ||
            (inputs[i]->runtime && inputs[i]->runtime != runtime)) {
            return NULL;
        }
    }
    *group = (wasmos_future_group_t){
        .runtime = runtime, .values = values, .count = count, .kind = kind, .active = true};
    wasmos_future_init(&group->future, &group->promise);
    group->future.runtime = runtime;
    for (size_t i = 0; i < count; ++i) {
        if (!inputs[i] || !wasmos_future_then(runtime, inputs[i], &continuations[i], group_success,
                                              group_error, &continuations[i])) {
            group->active = false;
            return NULL;
        }
        continuations[i].group = group;
        continuations[i].group_index = i;
    }
    return &group->future;
}

wasmos_future_t* wasmos_future_race(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                                    wasmos_future_t* const* inputs, size_t count,
                                    wasmos_future_continuation_t* continuations) {
    return future_group_start(runtime, group, inputs, count, NULL, continuations,
                              WASMOS_FUTURE_GROUP_RACE);
}

wasmos_future_t* wasmos_future_all(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                                   wasmos_future_t* const* inputs, size_t count, uintptr_t* values,
                                   wasmos_future_continuation_t* continuations) {
    return future_group_start(runtime, group, inputs, count, values, continuations,
                              WASMOS_FUTURE_GROUP_ALL);
}
