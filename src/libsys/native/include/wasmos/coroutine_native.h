/* coroutine_native.h - cooperative stackful coroutines for native WASMOS code.
 *
 * This is deliberately a single-worker, caller-storage runtime. A native
 * service supplies coroutine records and stacks; no allocator, kernel thread,
 * or WASM runtime support is required. The public surface is C ABI so native
 * C and Zig services can use the same runtime. */
#ifndef WASMOS_COROUTINE_NATIVE_H
#define WASMOS_COROUTINE_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wasmos_native_coroutine_state {
    WASMOS_NATIVE_COROUTINE_NEW = 0,
    WASMOS_NATIVE_COROUTINE_READY,
    WASMOS_NATIVE_COROUTINE_RUNNING,
    WASMOS_NATIVE_COROUTINE_WAITING,
    WASMOS_NATIVE_COROUTINE_DEAD,
} wasmos_native_coroutine_state_t;

typedef enum wasmos_future_state {
    WASMOS_FUTURE_PENDING = 0,
    WASMOS_FUTURE_READY,
    WASMOS_FUTURE_FAILED,
} wasmos_future_state_t;

#if defined(__x86_64__) && !defined(__wasm__)
typedef struct wasmos_native_coroutine_context {
    uintptr_t rsp;
    uintptr_t rbx;
    uintptr_t rbp;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
    uintptr_t rip;
} wasmos_native_coroutine_context_t;
#elif defined(__aarch64__)
typedef struct wasmos_native_coroutine_context {
    uintptr_t sp;
    uintptr_t x19;
    uintptr_t x20;
    uintptr_t x21;
    uintptr_t x22;
    uintptr_t x23;
    uintptr_t x24;
    uintptr_t x25;
    uintptr_t x26;
    uintptr_t x27;
    uintptr_t x28;
    uintptr_t x29;
    uintptr_t x30;
} wasmos_native_coroutine_context_t;
#else
#error "native coroutines require x86_64 or aarch64 native code"
#endif

typedef struct wasmos_native_coroutine wasmos_native_coroutine_t;
typedef struct wasmos_native_coroutine_runtime wasmos_native_coroutine_runtime_t;
typedef struct wasmos_future_continuation wasmos_future_continuation_t;
typedef struct wasmos_future_group wasmos_future_group_t;

/* Return zero to resolve the chained future with out_value, or a negative
 * status to reject it. */
typedef int32_t (*wasmos_future_success_fn_t)(void* user, uintptr_t value, uintptr_t* out_value);
typedef int32_t (*wasmos_future_error_fn_t)(void* user, int32_t status, uintptr_t* out_value);

typedef struct wasmos_future {
    wasmos_future_state_t state;
    int32_t status;
    uintptr_t value;
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* waiters;
    wasmos_future_continuation_t* continuations;
} wasmos_future_t;

typedef struct wasmos_promise {
    wasmos_future_t* future;
} wasmos_promise_t;

typedef enum wasmos_future_group_kind {
    WASMOS_FUTURE_GROUP_RACE = 0,
    WASMOS_FUTURE_GROUP_ALL,
} wasmos_future_group_kind_t;

/* Caller-owned state for race/all. Once the group future settles (a race
 * result, or a fail-fast all failure/complete all success) the runtime unlinks
 * every still-pending source continuation, so the group, its continuations, and
 * its values array only need to remain live until the group future settles -
 * not until every source future settles. */
struct wasmos_future_group {
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_future_continuation_t* continuations;
    uintptr_t* values;
    size_t count;
    size_t completed;
    wasmos_future_group_kind_t kind;
    bool settled;
    bool active;
};

typedef void (*wasmos_native_coroutine_entry_t)(void* arg);

/* Caller-owned continuation registration. A registration may be active on one
 * future at a time and must outlive the callback or cancellation of the
 * runtime. The record owns the child future returned by future_then(); do not
 * reuse it while that child or a future chained from it is still referenced.
 * Callbacks run from runtime_run(), never inline from resolve/reject. */
struct wasmos_future_continuation {
    wasmos_future_continuation_t* next;
    wasmos_future_t* future;
    wasmos_future_success_fn_t on_success;
    wasmos_future_error_fn_t on_error;
    void* user;
    wasmos_future_group_t* group;
    size_t group_index;
    wasmos_future_t child;
    wasmos_promise_t child_promise;
    bool active;
};

struct wasmos_native_coroutine {
    wasmos_native_coroutine_context_t context;
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_entry_t entry;
    void* arg;
    wasmos_native_coroutine_state_t state;
    int32_t result;
    wasmos_native_coroutine_t* next;
    wasmos_native_coroutine_t* wait_next;
    wasmos_future_t completion;
    wasmos_promise_t completion_promise;
};

struct wasmos_native_coroutine_runtime {
    wasmos_native_coroutine_context_t scheduler_context;
    wasmos_native_coroutine_t* current;
    wasmos_native_coroutine_t* ready_head;
    wasmos_native_coroutine_t* ready_tail;
    wasmos_future_continuation_t* continuation_head;
    wasmos_future_continuation_t* continuation_tail;
    bool running;
};

/* All state and stack memory remain caller-owned for their full lifetime. */
void wasmos_native_coroutine_runtime_init(wasmos_native_coroutine_runtime_t* runtime);
int wasmos_native_coroutine_spawn(wasmos_native_coroutine_runtime_t* runtime,
                                  wasmos_native_coroutine_t* coroutine, void* stack_base,
                                  size_t stack_size, wasmos_native_coroutine_entry_t entry,
                                  void* arg);
/* Starts a caller-owned native async task and returns its completion future,
 * or NULL if the coroutine cannot be started. */
wasmos_future_t* wasmos_async_start(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_native_coroutine_t* coroutine, void* stack_base,
                                    size_t stack_size, wasmos_native_coroutine_entry_t entry,
                                    void* arg);

/* Run at most budget ready coroutines, while still dispatching queued future
 * continuations. Returns the number resumed, or -1 for invalid/reentrant use. */
int wasmos_native_coroutine_run_budget(wasmos_native_coroutine_runtime_t* runtime, size_t budget);
/* Run ready work until no coroutine is runnable. Returns the number resumed,
 * or -1 for invalid/reentrant use. */
int wasmos_native_coroutine_run(wasmos_native_coroutine_runtime_t* runtime);
/* Returns true when the runtime has a ready coroutine or a queued continuation,
 * i.e. run_budget still has work to do. A running coroutine can use this to tell
 * "other coroutines are runnable" (keep yielding) from "idle" (safe to block). */
bool wasmos_native_coroutine_runtime_has_ready(const wasmos_native_coroutine_runtime_t* runtime);
void wasmos_native_coroutine_yield(void);
void wasmos_native_coroutine_exit(int32_t result) __attribute__((noreturn));
int wasmos_native_coroutine_join(wasmos_native_coroutine_t* coroutine, int32_t* out_result);

void wasmos_future_init(wasmos_future_t* future, wasmos_promise_t* promise);
bool wasmos_future_poll(const wasmos_future_t* future, int32_t* out_status, uintptr_t* out_value);
int wasmos_future_await(wasmos_future_t* future, uintptr_t* out_value);
/* Registers a scheduled transformation and returns its child future, or NULL
 * on invalid input. A missing success/error callback forwards that outcome. */
wasmos_future_t* wasmos_future_then(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_future_t* future,
                                    wasmos_future_continuation_t* continuation,
                                    wasmos_future_success_fn_t on_success,
                                    wasmos_future_error_fn_t on_error, void* user);

/* Register every source before returning. The inputs array is consumed during
 * this call, but group and continuations[count] must remain live until the
 * group future settles (the runtime unlinks unsettled sources at that point).
 * race resolves/rejects from the first source outcome; all resolves with values
 * on complete success or rejects on the first failure. */
wasmos_future_t* wasmos_future_race(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                    size_t count, wasmos_future_continuation_t* continuations);
wasmos_future_t* wasmos_future_all(wasmos_native_coroutine_runtime_t* runtime,
                                   wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                   size_t count, uintptr_t* values,
                                   wasmos_future_continuation_t* continuations);

#define WASMOS_FUTURE_RACE(runtime, group, continuations, ...)                                     \
    wasmos_future_race((runtime), (group), (wasmos_future_t*[]){__VA_ARGS__},                      \
                       sizeof((wasmos_future_t*[]){__VA_ARGS__}) / sizeof(wasmos_future_t*),       \
                       (continuations))

#define WASMOS_FUTURE_ALL(runtime, group, values, continuations, ...)                              \
    wasmos_future_all((runtime), (group), (wasmos_future_t*[]){__VA_ARGS__},                       \
                      sizeof((wasmos_future_t*[]){__VA_ARGS__}) / sizeof(wasmos_future_t*),        \
                      (values), (continuations))

bool wasmos_promise_resolve(wasmos_promise_t* promise, uintptr_t value);
bool wasmos_promise_reject(wasmos_promise_t* promise, int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_COROUTINE_NATIVE_H */
