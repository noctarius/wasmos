#include "wasmos/coroutine_native.h"

#if (!defined(__x86_64__) && !defined(__aarch64__)) || defined(__wasm__)
#error "native coroutines require x86_64 or aarch64 native code"
#endif

extern void wasmos_native_coroutine_context_switch(wasmos_native_coroutine_context_t* from,
                                                   const wasmos_native_coroutine_context_t* to);

#define WASMOS_NATIVE_COROUTINE_MIN_STACK 1024u

#if defined(__x86_64__)
_Static_assert(offsetof(wasmos_native_coroutine_context_t, rsp) == 0u,
               "context-switch assembly offset for rsp changed");
_Static_assert(offsetof(wasmos_native_coroutine_context_t, rbx) == 8u,
               "context-switch assembly offset for rbx changed");
_Static_assert(offsetof(wasmos_native_coroutine_context_t, rip) == 56u,
               "context-switch assembly offset for rip changed");
_Static_assert(sizeof(wasmos_native_coroutine_context_t) == 64u,
               "context-switch assembly context size changed");
#elif defined(__aarch64__)
_Static_assert(offsetof(wasmos_native_coroutine_context_t, sp) == 0u,
               "context-switch assembly offset for sp changed");
_Static_assert(offsetof(wasmos_native_coroutine_context_t, x19) == 8u,
               "context-switch assembly offset for x19 changed");
_Static_assert(offsetof(wasmos_native_coroutine_context_t, x30) == 96u,
               "context-switch assembly offset for x30 changed");
_Static_assert(sizeof(wasmos_native_coroutine_context_t) == 104u,
               "context-switch assembly context size changed");
#endif

/* A native service links one copy of this single-worker runtime. Keeping the
 * active runtime in ordinary static storage is deliberate: r12/x19 are ABI
 * callee-saved registers, not reserved registers, and C code is free to reuse
 * them while a coroutine is running. */
static wasmos_native_coroutine_runtime_t* g_current_runtime;

static wasmos_native_coroutine_runtime_t* coroutine_current_runtime(void) {
    return g_current_runtime;
}

static void coroutine_enqueue(wasmos_native_coroutine_runtime_t* runtime,
                              wasmos_native_coroutine_t* coroutine) {
    coroutine->next = NULL;
    if (runtime->ready_tail) {
        runtime->ready_tail->next = coroutine;
    } else {
        runtime->ready_head = coroutine;
    }
    runtime->ready_tail = coroutine;
}

static wasmos_native_coroutine_t* coroutine_dequeue(wasmos_native_coroutine_runtime_t* runtime) {
    wasmos_native_coroutine_t* coroutine = runtime->ready_head;
    if (!coroutine) {
        return NULL;
    }
    runtime->ready_head = coroutine->next;
    if (!runtime->ready_head) {
        runtime->ready_tail = NULL;
    }
    coroutine->next = NULL;
    return coroutine;
}

static void coroutine_make_ready(wasmos_native_coroutine_t* coroutine) {
    if (!coroutine || coroutine->state != WASMOS_NATIVE_COROUTINE_WAITING) {
        return;
    }
    coroutine->state = WASMOS_NATIVE_COROUTINE_READY;
    coroutine_enqueue(coroutine->runtime, coroutine);
}

static void continuation_enqueue(wasmos_native_coroutine_runtime_t* runtime,
                                 wasmos_future_continuation_t* continuation) {
    continuation->next = NULL;
    if (runtime->continuation_tail) {
        runtime->continuation_tail->next = continuation;
    } else {
        runtime->continuation_head = continuation;
    }
    runtime->continuation_tail = continuation;
}

static wasmos_future_continuation_t*
continuation_dequeue(wasmos_native_coroutine_runtime_t* runtime) {
    wasmos_future_continuation_t* continuation = runtime->continuation_head;
    if (!continuation) {
        return NULL;
    }
    runtime->continuation_head = continuation->next;
    if (!runtime->continuation_head) {
        runtime->continuation_tail = NULL;
    }
    continuation->next = NULL;
    return continuation;
}

static void continuation_schedule(wasmos_native_coroutine_runtime_t* runtime,
                                  wasmos_future_continuation_t* continuation) {
    if (!runtime || !continuation || !continuation->active) {
        return;
    }
    continuation_enqueue(runtime, continuation);
}

static void coroutine_trampoline(void) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    /* The scheduler publishes current before entering this freshly prepared
     * stack, so no per-stack bootstrap object is needed. */
    __asm__ volatile("" : : : "memory");
    runtime = coroutine_current_runtime();
    /* The current coroutine is recoverable through the scheduler's active
     * runtime and entry record. */
    coroutine = runtime ? runtime->current : NULL;
    if (!coroutine || !coroutine->entry) {
        __builtin_trap();
    }
    coroutine->entry(coroutine->arg);
    wasmos_native_coroutine_exit(0);
}

void wasmos_native_coroutine_runtime_init(wasmos_native_coroutine_runtime_t* runtime) {
    if (!runtime) {
        return;
    }
    runtime->scheduler_context = (wasmos_native_coroutine_context_t){0};
    runtime->current = NULL;
    runtime->ready_head = NULL;
    runtime->ready_tail = NULL;
    runtime->continuation_head = NULL;
    runtime->continuation_tail = NULL;
    runtime->running = false;
}

void wasmos_future_init(wasmos_future_t* future, wasmos_promise_t* promise) {
    if (!future || !promise) {
        return;
    }
    future->state = WASMOS_FUTURE_PENDING;
    future->status = 0;
    future->value = 0;
    future->runtime = NULL;
    future->waiters = NULL;
    future->continuations = NULL;
    promise->future = future;
}

int wasmos_native_coroutine_spawn(wasmos_native_coroutine_runtime_t* runtime,
                                  wasmos_native_coroutine_t* coroutine, void* stack_base,
                                  size_t stack_size, wasmos_native_coroutine_entry_t entry,
                                  void* arg) {
    uintptr_t top;

    if (!runtime || !coroutine || !stack_base || stack_size < WASMOS_NATIVE_COROUTINE_MIN_STACK ||
        !entry) {
        return -1;
    }
    /* Match the WASM runtime: a record may only be (re)spawned when it is fresh
     * or has completed. Re-spawning a queued/running/waiting coroutine would
     * corrupt the ready list through its reused link fields. */
    if (coroutine->state != WASMOS_NATIVE_COROUTINE_NEW &&
        coroutine->state != WASMOS_NATIVE_COROUTINE_DEAD) {
        return -1;
    }
    top = ((uintptr_t)stack_base + stack_size) & ~(uintptr_t)0xFu;
#if defined(__x86_64__)
    if (top <= (uintptr_t)stack_base + sizeof(uintptr_t)) {
        return -1;
    }
    top -= sizeof(uintptr_t);
    *(uintptr_t*)top = 0;
#elif defined(__aarch64__)
    if (top <= (uintptr_t)stack_base) {
        return -1;
    }
#endif

    coroutine->context = (wasmos_native_coroutine_context_t){0};
#if defined(__x86_64__)
    coroutine->context.rsp = top;
    coroutine->context.rip = (uintptr_t)coroutine_trampoline;
#elif defined(__aarch64__)
    coroutine->context.sp = top;
    coroutine->context.x30 = (uintptr_t)coroutine_trampoline;
#endif
    coroutine->runtime = runtime;
    coroutine->entry = entry;
    coroutine->arg = arg;
    coroutine->state = WASMOS_NATIVE_COROUTINE_READY;
    coroutine->result = 0;
    coroutine->next = NULL;
    coroutine->wait_next = NULL;
    wasmos_future_init(&coroutine->completion, &coroutine->completion_promise);
    coroutine_enqueue(runtime, coroutine);
    return 0;
}

wasmos_future_t* wasmos_async_start(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_native_coroutine_t* coroutine, void* stack_base,
                                    size_t stack_size, wasmos_native_coroutine_entry_t entry,
                                    void* arg) {
    if (wasmos_native_coroutine_spawn(runtime, coroutine, stack_base, stack_size, entry, arg) !=
        0) {
        return NULL;
    }
    return &coroutine->completion;
}

int wasmos_native_coroutine_run_budget(wasmos_native_coroutine_runtime_t* runtime, size_t budget) {
    int resumed = 0;
    wasmos_native_coroutine_t* coroutine;

    if (!runtime || runtime->running) {
        return -1;
    }
    runtime->running = true;
    for (;;) {
        if (budget != 0u && (coroutine = coroutine_dequeue(runtime)) != NULL) {
            runtime->current = coroutine;
            coroutine->state = WASMOS_NATIVE_COROUTINE_RUNNING;
            resumed++;
            budget--;
            g_current_runtime = runtime;
            wasmos_native_coroutine_context_switch(&runtime->scheduler_context,
                                                   &coroutine->context);
            g_current_runtime = NULL;
            runtime->current = NULL;
            continue;
        }
        wasmos_future_continuation_t* continuation = continuation_dequeue(runtime);
        wasmos_future_t* future;
        if (!continuation) {
            break;
        }
        future = continuation->future;
        continuation->active = false;
        continuation->future = NULL;
        if (!future || future->state == WASMOS_FUTURE_PENDING) {
            continue;
        }
        if (future->status == 0) {
            if (continuation->on_success) {
                uintptr_t value = 0;
                int32_t status =
                    continuation->on_success(continuation->user, future->value, &value);
                if (status == 0) {
                    (void)wasmos_promise_resolve(&continuation->child_promise, value);
                } else {
                    (void)wasmos_promise_reject(&continuation->child_promise,
                                                status < 0 ? status : -1);
                }
            } else {
                (void)wasmos_promise_resolve(&continuation->child_promise, future->value);
            }
        } else if (continuation->on_error) {
            uintptr_t value = 0;
            int32_t status = continuation->on_error(continuation->user, future->status, &value);
            if (status == 0) {
                (void)wasmos_promise_resolve(&continuation->child_promise, value);
            } else {
                (void)wasmos_promise_reject(&continuation->child_promise, status < 0 ? status : -1);
            }
        } else {
            (void)wasmos_promise_reject(&continuation->child_promise, future->status);
        }
    }
    runtime->running = false;
    return resumed;
}

int wasmos_native_coroutine_run(wasmos_native_coroutine_runtime_t* runtime) {
    return wasmos_native_coroutine_run_budget(runtime, (size_t)-1);
}

bool wasmos_native_coroutine_runtime_has_ready(const wasmos_native_coroutine_runtime_t* runtime) {
    return runtime && (runtime->ready_head != NULL || runtime->continuation_head != NULL);
}

void wasmos_native_coroutine_yield(void) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    runtime = coroutine_current_runtime();
    coroutine = runtime ? runtime->current : NULL;
    if (!coroutine || coroutine->state != WASMOS_NATIVE_COROUTINE_RUNNING) {
        __builtin_trap();
    }
    coroutine->state = WASMOS_NATIVE_COROUTINE_READY;
    coroutine_enqueue(runtime, coroutine);
    wasmos_native_coroutine_context_switch(&coroutine->context, &runtime->scheduler_context);
}

void wasmos_native_coroutine_exit(int32_t result) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    runtime = coroutine_current_runtime();
    coroutine = runtime ? runtime->current : NULL;
    if (!coroutine || coroutine->state != WASMOS_NATIVE_COROUTINE_RUNNING) {
        __builtin_trap();
    }
    coroutine->result = result;
    coroutine->state = WASMOS_NATIVE_COROUTINE_DEAD;
    (void)wasmos_promise_resolve(&coroutine->completion_promise, (uintptr_t)(intptr_t)result);
    wasmos_native_coroutine_context_switch(&coroutine->context, &runtime->scheduler_context);
    __builtin_trap();
}

bool wasmos_future_poll(const wasmos_future_t* future, int32_t* out_status, uintptr_t* out_value) {
    if (!future || future->state == WASMOS_FUTURE_PENDING) {
        return false;
    }
    if (out_status) {
        *out_status = future->status;
    }
    if (out_value) {
        *out_value = future->value;
    }
    return true;
}

static bool promise_complete(wasmos_promise_t* promise, int32_t status, uintptr_t value) {
    wasmos_future_t* future;
    wasmos_native_coroutine_t* waiter;
    wasmos_future_continuation_t* continuation;

    if (!promise || !(future = promise->future) || future->state != WASMOS_FUTURE_PENDING) {
        return false;
    }
    future->status = status;
    future->value = value;
    future->state = status == 0 ? WASMOS_FUTURE_READY : WASMOS_FUTURE_FAILED;
    waiter = future->waiters;
    future->waiters = NULL;
    while (waiter) {
        wasmos_native_coroutine_t* next = waiter->wait_next;
        waiter->wait_next = NULL;
        coroutine_make_ready(waiter);
        waiter = next;
    }
    continuation = future->continuations;
    future->continuations = NULL;
    while (continuation) {
        wasmos_future_continuation_t* next = continuation->next;
        continuation_schedule(future->runtime, continuation);
        continuation = next;
    }
    return true;
}

bool wasmos_promise_resolve(wasmos_promise_t* promise, uintptr_t value) {
    return promise_complete(promise, 0, value);
}

bool wasmos_promise_reject(wasmos_promise_t* promise, int32_t status) {
    if (status >= 0) {
        return false;
    }
    return promise_complete(promise, status, 0);
}

int wasmos_future_await(wasmos_future_t* future, uintptr_t* out_value) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    if (!future) {
        return -1;
    }
    if (future->state == WASMOS_FUTURE_PENDING) {
        runtime = coroutine_current_runtime();
        coroutine = runtime ? runtime->current : NULL;
        if (!coroutine || coroutine->state != WASMOS_NATIVE_COROUTINE_RUNNING) {
            return -1;
        }
        if (future->runtime && future->runtime != runtime) {
            return -1;
        }
        future->runtime = runtime;
        coroutine->wait_next = future->waiters;
        future->waiters = coroutine;
        coroutine->state = WASMOS_NATIVE_COROUTINE_WAITING;
        wasmos_native_coroutine_context_switch(&coroutine->context, &runtime->scheduler_context);
    }
    if (future->state == WASMOS_FUTURE_PENDING) {
        return -1;
    }
    if (out_value) {
        *out_value = future->value;
    }
    return future->status;
}

wasmos_future_t* wasmos_future_then(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_future_t* future,
                                    wasmos_future_continuation_t* continuation,
                                    wasmos_future_success_fn_t on_success,
                                    wasmos_future_error_fn_t on_error, void* user) {
    if (!runtime || !future || !continuation || continuation->active) {
        return NULL;
    }
    if (future->runtime && future->runtime != runtime) {
        return NULL;
    }
    future->runtime = runtime;
    wasmos_future_init(&continuation->child, &continuation->child_promise);
    continuation->child.runtime = runtime;
    continuation->next = NULL;
    continuation->future = future;
    continuation->on_success = on_success;
    continuation->on_error = on_error;
    continuation->user = user;
    continuation->active = true;
    if (future->state == WASMOS_FUTURE_PENDING) {
        continuation->next = future->continuations;
        future->continuations = continuation;
    } else {
        continuation_schedule(runtime, continuation);
    }
    return &continuation->child;
}

static void future_group_callback_complete(wasmos_future_group_t* group) {
    group->completed++;
    if (group->completed == group->count) {
        group->active = false;
    }
}

static void continuation_list_remove(wasmos_future_continuation_t** head,
                                     wasmos_future_continuation_t** tail,
                                     wasmos_future_continuation_t* target) {
    wasmos_future_continuation_t* prev = NULL;
    wasmos_future_continuation_t* cur = *head;

    while (cur) {
        if (cur == target) {
            if (prev) {
                prev->next = cur->next;
            } else {
                *head = cur->next;
            }
            if (tail && *tail == target) {
                *tail = prev;
            }
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/* Detach a still-registered continuation from wherever it currently sits: the
 * pending source future's continuation list, or the runtime's dispatch queue if
 * its source already settled. A dispatched continuation has active == false and
 * is left untouched. */
static void continuation_cancel(wasmos_native_coroutine_runtime_t* runtime,
                                wasmos_future_continuation_t* continuation) {
    wasmos_future_t* future;

    if (!continuation || !continuation->active) {
        return;
    }
    future = continuation->future;
    if (future && future->state == WASMOS_FUTURE_PENDING) {
        continuation_list_remove(&future->continuations, NULL, continuation);
    } else if (runtime) {
        continuation_list_remove(&runtime->continuation_head, &runtime->continuation_tail,
                                 continuation);
    }
    continuation->next = NULL;
    continuation->future = NULL;
    continuation->active = false;
}

/* Called once a group has settled to release its remaining source
 * continuations so caller-owned group storage need not outlive slow sources. */
static void future_group_abandon(wasmos_future_group_t* group) {
    if (!group || !group->continuations) {
        return;
    }
    for (size_t i = 0; i < group->count; ++i) {
        continuation_cancel(group->runtime, &group->continuations[i]);
    }
    group->active = false;
}

static int32_t future_group_success(void* user, uintptr_t value, uintptr_t* out_value) {
    wasmos_future_continuation_t* continuation = user;
    wasmos_future_group_t* group = continuation ? continuation->group : NULL;

    if (!group || !group->active) {
        return -1;
    }
    if (group->kind == WASMOS_FUTURE_GROUP_ALL) {
        group->values[continuation->group_index] = value;
        future_group_callback_complete(group);
        if (!group->settled && group->completed == group->count) {
            group->settled = true;
            (void)wasmos_promise_resolve(&group->promise, (uintptr_t)group->values);
            future_group_abandon(group);
        }
    } else {
        if (!group->settled) {
            group->settled = true;
            (void)wasmos_promise_resolve(&group->promise, value);
            future_group_abandon(group);
        }
        future_group_callback_complete(group);
    }
    *out_value = value;
    return 0;
}

static int32_t future_group_error(void* user, int32_t status, uintptr_t* out_value) {
    wasmos_future_continuation_t* continuation = user;
    wasmos_future_group_t* group = continuation ? continuation->group : NULL;
    int32_t error = status < 0 ? status : -1;

    if (!group || !group->active) {
        return -1;
    }
    if (!group->settled) {
        group->settled = true;
        (void)wasmos_promise_reject(&group->promise, error);
        future_group_abandon(group);
    }
    future_group_callback_complete(group);
    *out_value = 0;
    return error;
}

static bool future_group_inputs_valid(wasmos_native_coroutine_runtime_t* runtime,
                                      wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                      size_t count, uintptr_t* values,
                                      wasmos_future_continuation_t* continuations,
                                      wasmos_future_group_kind_t kind) {
    if (!runtime || !group || !inputs || !continuations || count == 0 || group->active ||
        (kind == WASMOS_FUTURE_GROUP_ALL && !values)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!inputs[i] || continuations[i].active ||
            (inputs[i]->runtime && inputs[i]->runtime != runtime)) {
            return false;
        }
    }
    return true;
}

static wasmos_future_t*
future_group_start(wasmos_native_coroutine_runtime_t* runtime, wasmos_future_group_t* group,
                   wasmos_future_t* const* inputs, size_t count, uintptr_t* values,
                   wasmos_future_continuation_t* continuations, wasmos_future_group_kind_t kind) {
    /* Once the group settles, future_group_abandon() unlinks the remaining
     * source continuations, so caller group storage only needs to outlive the
     * group future, not every source. */
    if (!future_group_inputs_valid(runtime, group, inputs, count, values, continuations, kind)) {
        return NULL;
    }
    group->runtime = runtime;
    group->continuations = continuations;
    group->values = values;
    group->count = count;
    group->completed = 0;
    group->kind = kind;
    group->settled = false;
    group->active = true;
    wasmos_future_init(&group->future, &group->promise);
    group->future.runtime = runtime;

    for (size_t i = 0; i < count; ++i) {
        continuations[i].group = group;
        continuations[i].group_index = i;
        if (!wasmos_future_then(runtime, inputs[i], &continuations[i], future_group_success,
                                future_group_error, &continuations[i])) {
            __builtin_trap();
        }
    }
    return &group->future;
}

wasmos_future_t* wasmos_future_race(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                    size_t count, wasmos_future_continuation_t* continuations) {
    return future_group_start(runtime, group, inputs, count, NULL, continuations,
                              WASMOS_FUTURE_GROUP_RACE);
}

wasmos_future_t* wasmos_future_all(wasmos_native_coroutine_runtime_t* runtime,
                                   wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                   size_t count, uintptr_t* values,
                                   wasmos_future_continuation_t* continuations) {
    return future_group_start(runtime, group, inputs, count, values, continuations,
                              WASMOS_FUTURE_GROUP_ALL);
}

int wasmos_native_coroutine_join(wasmos_native_coroutine_t* coroutine, int32_t* out_result) {
    uintptr_t value = 0;
    int status;

    if (!coroutine) {
        return -1;
    }
    status = wasmos_future_await(&coroutine->completion, &value);
    if (status == 0 && out_result) {
        *out_result = (int32_t)(intptr_t)value;
    }
    return status;
}
