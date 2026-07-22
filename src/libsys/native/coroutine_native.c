#include "wasmos/coroutine_native.h"

#if !defined(__x86_64__) || defined(__wasm__)
#error "native coroutines require x86_64 native code"
#endif

extern void wasmos_native_coroutine_context_switch(wasmos_native_coroutine_context_t* from,
                                                   const wasmos_native_coroutine_context_t* to);

#define WASMOS_NATIVE_COROUTINE_MIN_STACK 1024u

_Static_assert(offsetof(wasmos_native_coroutine_context_t, rsp) == 0u,
               "context-switch assembly offset for rsp changed");
_Static_assert(offsetof(wasmos_native_coroutine_context_t, rbx) == 8u,
               "context-switch assembly offset for rbx changed");
_Static_assert(offsetof(wasmos_native_coroutine_context_t, rip) == 56u,
               "context-switch assembly offset for rip changed");
_Static_assert(sizeof(wasmos_native_coroutine_context_t) == 64u,
               "context-switch assembly context size changed");

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

static void coroutine_trampoline(void) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    /* The scheduler publishes current before entering this freshly prepared
     * stack, so no per-stack bootstrap object is needed. */
    __asm__ volatile("" : : : "memory");
    runtime = NULL;
    /* The current coroutine is recoverable through the entry's record. The
     * trampoline receives it in r12, a callee-saved register restored by the
     * context switch before control reaches this function. */
    __asm__ volatile("mov %%r12, %0" : "=r"(runtime));
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
}

void wasmos_native_future_init(wasmos_native_future_t* future, wasmos_native_promise_t* promise) {
    if (!future || !promise) {
        return;
    }
    future->state = WASMOS_NATIVE_FUTURE_PENDING;
    future->status = 0;
    future->value = 0;
    future->waiters = NULL;
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
    top = ((uintptr_t)stack_base + stack_size) & ~(uintptr_t)0xFu;
    if (top <= (uintptr_t)stack_base + sizeof(uintptr_t)) {
        return -1;
    }
    top -= sizeof(uintptr_t);
    *(uintptr_t*)top = 0;

    coroutine->context = (wasmos_native_coroutine_context_t){0};
    coroutine->context.rsp = top;
    coroutine->context.r12 = (uintptr_t)runtime;
    coroutine->context.rip = (uintptr_t)coroutine_trampoline;
    coroutine->runtime = runtime;
    coroutine->entry = entry;
    coroutine->arg = arg;
    coroutine->state = WASMOS_NATIVE_COROUTINE_READY;
    coroutine->result = 0;
    coroutine->next = NULL;
    coroutine->wait_next = NULL;
    wasmos_native_future_init(&coroutine->completion, &coroutine->completion_promise);
    coroutine_enqueue(runtime, coroutine);
    return 0;
}

int wasmos_native_coroutine_run(wasmos_native_coroutine_runtime_t* runtime) {
    int resumed = 0;
    wasmos_native_coroutine_t* coroutine;

    if (!runtime || runtime->current) {
        return -1;
    }
    while ((coroutine = coroutine_dequeue(runtime)) != NULL) {
        runtime->current = coroutine;
        coroutine->state = WASMOS_NATIVE_COROUTINE_RUNNING;
        resumed++;
        wasmos_native_coroutine_context_switch(&runtime->scheduler_context, &coroutine->context);
        runtime->current = NULL;
    }
    return resumed;
}

void wasmos_native_coroutine_yield(void) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    __asm__ volatile("mov %%r12, %0" : "=r"(runtime));
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

    __asm__ volatile("mov %%r12, %0" : "=r"(runtime));
    coroutine = runtime ? runtime->current : NULL;
    if (!coroutine || coroutine->state != WASMOS_NATIVE_COROUTINE_RUNNING) {
        __builtin_trap();
    }
    coroutine->result = result;
    coroutine->state = WASMOS_NATIVE_COROUTINE_DEAD;
    (void)wasmos_native_promise_resolve(&coroutine->completion_promise,
                                        (uintptr_t)(intptr_t)result);
    wasmos_native_coroutine_context_switch(&coroutine->context, &runtime->scheduler_context);
    __builtin_trap();
}

bool wasmos_native_future_poll(const wasmos_native_future_t* future, int32_t* out_status,
                               uintptr_t* out_value) {
    if (!future || future->state == WASMOS_NATIVE_FUTURE_PENDING) {
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

static bool promise_complete(wasmos_native_promise_t* promise, int32_t status, uintptr_t value) {
    wasmos_native_future_t* future;
    wasmos_native_coroutine_t* waiter;

    if (!promise || !(future = promise->future) || future->state != WASMOS_NATIVE_FUTURE_PENDING) {
        return false;
    }
    future->status = status;
    future->value = value;
    future->state = status == 0 ? WASMOS_NATIVE_FUTURE_READY : WASMOS_NATIVE_FUTURE_FAILED;
    waiter = future->waiters;
    future->waiters = NULL;
    while (waiter) {
        wasmos_native_coroutine_t* next = waiter->wait_next;
        waiter->wait_next = NULL;
        coroutine_make_ready(waiter);
        waiter = next;
    }
    return true;
}

bool wasmos_native_promise_resolve(wasmos_native_promise_t* promise, uintptr_t value) {
    return promise_complete(promise, 0, value);
}

bool wasmos_native_promise_reject(wasmos_native_promise_t* promise, int32_t status) {
    if (status >= 0) {
        return false;
    }
    return promise_complete(promise, status, 0);
}

int wasmos_native_future_await(wasmos_native_future_t* future, uintptr_t* out_value) {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* coroutine;

    if (!future) {
        return -1;
    }
    if (future->state == WASMOS_NATIVE_FUTURE_PENDING) {
        __asm__ volatile("mov %%r12, %0" : "=r"(runtime));
        coroutine = runtime ? runtime->current : NULL;
        if (!coroutine || coroutine->state != WASMOS_NATIVE_COROUTINE_RUNNING) {
            return -1;
        }
        coroutine->wait_next = future->waiters;
        future->waiters = coroutine;
        coroutine->state = WASMOS_NATIVE_COROUTINE_WAITING;
        wasmos_native_coroutine_context_switch(&coroutine->context, &runtime->scheduler_context);
    }
    if (future->state == WASMOS_NATIVE_FUTURE_PENDING) {
        return -1;
    }
    if (out_value) {
        *out_value = future->value;
    }
    return future->status;
}

int wasmos_native_coroutine_join(wasmos_native_coroutine_t* coroutine, int32_t* out_result) {
    uintptr_t value = 0;
    int status;

    if (!coroutine) {
        return -1;
    }
    status = wasmos_native_future_await(&coroutine->completion, &value);
    if (status == 0 && out_result) {
        *out_result = (int32_t)(intptr_t)value;
    }
    return status;
}
