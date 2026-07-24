/* coroutine_wasm.h - cooperative stackless coroutines for WASMOS guests.
 *
 * Unlike native coroutines this API never saves a C/WASM call stack. A task
 * resume function returns WASMOS_WASM_TASK_YIELDED after recording its own
 * program counter in caller-owned state, then is invoked again by the runtime.
 * Future/promise semantics intentionally match the native runtime. */
#ifndef WASMOS_COROUTINE_WASM_H
#define WASMOS_COROUTINE_WASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WASMOS_FUTURE_PENDING = 0,
    WASMOS_FUTURE_READY,
    WASMOS_FUTURE_FAILED,
} wasmos_future_state_t;

typedef enum {
    WASMOS_WASM_COROUTINE_NEW = 0,
    WASMOS_WASM_COROUTINE_READY,
    WASMOS_WASM_COROUTINE_RUNNING,
    WASMOS_WASM_COROUTINE_WAITING,
    WASMOS_WASM_COROUTINE_DEAD,
} wasmos_wasm_coroutine_state_t;

enum {
    WASMOS_WASM_TASK_COMPLETE = 0,
    WASMOS_WASM_TASK_YIELDED = 1,
    WASMOS_WASM_AWAIT_PENDING = 1,
    WASMOS_FUTURE_CHAIN_NEXT = 2,
};

typedef struct wasmos_wasm_runtime wasmos_wasm_runtime_t;
typedef struct wasmos_wasm_coroutine wasmos_wasm_coroutine_t;
typedef struct wasmos_future wasmos_future_t;
typedef struct wasmos_promise wasmos_promise_t;
typedef struct wasmos_future_continuation wasmos_future_continuation_t;
typedef struct wasmos_future_group wasmos_future_group_t;

typedef int32_t (*wasmos_wasm_task_resume_fn)(void* user, uintptr_t* out_value);
typedef int32_t (*wasmos_future_success_fn_t)(void* user, uintptr_t value, uintptr_t* out_value);
typedef int32_t (*wasmos_future_error_fn_t)(void* user, int32_t status, uintptr_t* out_value);

struct wasmos_future {
    wasmos_future_state_t state;
    int32_t status;
    uintptr_t value;
    wasmos_wasm_runtime_t* runtime;
    wasmos_wasm_coroutine_t* waiters;
    wasmos_future_continuation_t* continuations;
};

struct wasmos_promise {
    wasmos_future_t* future;
};

struct wasmos_wasm_coroutine {
    wasmos_wasm_runtime_t* runtime;
    wasmos_wasm_coroutine_t* next;
    wasmos_wasm_coroutine_t* wait_next;
    wasmos_wasm_task_resume_fn resume;
    void* user;
    wasmos_wasm_coroutine_state_t state;
    int32_t result;
    wasmos_future_t completion;
    wasmos_promise_t completion_promise;
};

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

typedef enum { WASMOS_FUTURE_GROUP_RACE = 0, WASMOS_FUTURE_GROUP_ALL } wasmos_future_group_kind_t;
/* Once the group future settles the runtime unlinks every still-pending source
 * continuation, so group, continuations, and values only need to stay live
 * until the group future settles - not until every source settles. */
struct wasmos_future_group {
    wasmos_wasm_runtime_t* runtime;
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

struct wasmos_wasm_runtime {
    wasmos_wasm_coroutine_t* current;
    wasmos_wasm_coroutine_t* ready_head;
    wasmos_wasm_coroutine_t* ready_tail;
    wasmos_future_continuation_t* continuation_head;
    wasmos_future_continuation_t* continuation_tail;
    bool running;
};

void wasmos_wasm_runtime_init(wasmos_wasm_runtime_t* runtime);
wasmos_future_t* wasmos_async_start(wasmos_wasm_runtime_t* runtime,
                                    wasmos_wasm_coroutine_t* coroutine,
                                    wasmos_wasm_task_resume_fn resume, void* user);
int wasmos_wasm_coroutine_run_budget(wasmos_wasm_runtime_t* runtime, size_t budget);
int wasmos_wasm_coroutine_run(wasmos_wasm_runtime_t* runtime);
int wasmos_wasm_coroutine_yield(void);
int wasmos_wasm_coroutine_join(wasmos_wasm_coroutine_t* coroutine, int32_t* out_result);

void wasmos_future_init(wasmos_future_t* future, wasmos_promise_t* promise);
bool wasmos_future_poll(const wasmos_future_t* future, int32_t* out_status, uintptr_t* out_value);
/* Returns 0/negative for a settled future, or WASMOS_WASM_AWAIT_PENDING after
 * parking the current stackless task. The caller must return TASK_YIELDED. */
int wasmos_future_await(wasmos_future_t* future, uintptr_t* out_value);
bool wasmos_promise_resolve(wasmos_promise_t* promise, uintptr_t value);
bool wasmos_promise_reject(wasmos_promise_t* promise, int32_t status);
wasmos_future_t* wasmos_future_then(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                    wasmos_future_continuation_t* continuation,
                                    wasmos_future_success_fn_t on_success,
                                    wasmos_future_error_fn_t on_error, void* user);
/* A success callback may return WASMOS_FUTURE_CHAIN_NEXT with the next future
 * in out_value. The returned child adopts that future's eventual result. */
wasmos_future_t* wasmos_future_then_flat(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                         wasmos_future_continuation_t* continuation,
                                         wasmos_future_continuation_t* adopt_continuation,
                                         wasmos_future_success_fn_t on_success,
                                         wasmos_future_error_fn_t on_error, void* user);
wasmos_future_t* wasmos_future_race(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                                    wasmos_future_t* const* inputs, size_t count,
                                    wasmos_future_continuation_t* continuations);
wasmos_future_t* wasmos_future_all(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                                   wasmos_future_t* const* inputs, size_t count, uintptr_t* values,
                                   wasmos_future_continuation_t* continuations);

#define WASMOS_FUTURE_RACE(runtime, group, continuations, ...)                                     \
    wasmos_future_race((runtime), (group), (wasmos_future_t*[]){__VA_ARGS__},                      \
                       sizeof((wasmos_future_t*[]){__VA_ARGS__}) / sizeof(wasmos_future_t*),       \
                       (continuations))
#define WASMOS_FUTURE_ALL(runtime, group, values, continuations, ...)                              \
    wasmos_future_all((runtime), (group), (wasmos_future_t*[]){__VA_ARGS__},                       \
                      sizeof((wasmos_future_t*[]){__VA_ARGS__}) / sizeof(wasmos_future_t*),        \
                      (values), (continuations))

#ifdef __cplusplus
}
#endif
#endif
