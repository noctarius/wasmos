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

/* Settlement state of a future. PENDING is the only state a promise may still
 * settle from; READY carries `value`, FAILED carries a negative `status`. */
typedef enum {
    WASMOS_FUTURE_PENDING = 0,
    WASMOS_FUTURE_READY,
    WASMOS_FUTURE_FAILED,
} wasmos_future_state_t;

/* Lifecycle of a stackless task record.  NEW and DEAD are the only states from
 * which wasmos_async_start() accepts a record; READY means queued on the
 * runtime's ready list, RUNNING means its resume function is on the stack, and
 * WAITING means it is parked on a future's waiter list. */
typedef enum {
    WASMOS_WASM_COROUTINE_NEW = 0,
    WASMOS_WASM_COROUTINE_READY,
    WASMOS_WASM_COROUTINE_RUNNING,
    WASMOS_WASM_COROUTINE_WAITING,
    WASMOS_WASM_COROUTINE_DEAD,
} wasmos_wasm_coroutine_state_t;

/* Return protocol of a wasmos_wasm_task_resume_fn.  TASK_COMPLETE (0) resolves
 * the task's completion future with *out_value; TASK_YIELDED asks to be resumed
 * again; any other value is a negative status that rejects the completion
 * future.  CHAIN_NEXT is not a task return: it is the then_flat() success-
 * callback signal described at wasmos_future_then_flat(). */
enum {
    WASMOS_WASM_TASK_COMPLETE = 0,
    WASMOS_WASM_TASK_YIELDED = 1,
    /* Deliberately shares TASK_YIELDED's value: a resume function may return
     * wasmos_future_await()'s result straight to the runtime. */
    WASMOS_WASM_AWAIT_PENDING = 1,
    WASMOS_FUTURE_CHAIN_NEXT = 2,
};

typedef struct wasmos_wasm_runtime wasmos_wasm_runtime_t;
typedef struct wasmos_wasm_coroutine wasmos_wasm_coroutine_t;
typedef struct wasmos_future wasmos_future_t;
typedef struct wasmos_promise wasmos_promise_t;
typedef struct wasmos_future_continuation wasmos_future_continuation_t;
typedef struct wasmos_future_group wasmos_future_group_t;

/* One step of a stackless task.  `user` is the pointer handed to
 * wasmos_async_start(); the task keeps its own resume point there because no
 * stack is preserved across calls.  Returns a WASMOS_WASM_TASK_* code and, on
 * TASK_COMPLETE, the completion value through out_value. */
typedef int32_t (*wasmos_wasm_task_resume_fn)(void* user, uintptr_t* out_value);
/* Continuation callbacks.  Both return 0 to resolve the chained child future
 * with *out_value, or a negative status to reject it; a non-negative non-zero
 * return is normalised to -1 (except WASMOS_FUTURE_CHAIN_NEXT from a
 * then_flat() success callback). */
typedef int32_t (*wasmos_future_success_fn_t)(void* user, uintptr_t value, uintptr_t* out_value);
typedef int32_t (*wasmos_future_error_fn_t)(void* user, int32_t status, uintptr_t* out_value);

/* Caller-owned single-settlement cell.  `runtime` is bound lazily by the first
 * await/then/group that touches the future and pins it to that runtime. */
struct wasmos_future {
    wasmos_future_state_t state;
    int32_t status; /* 0 while pending or ready; negative once FAILED */
    uintptr_t value;
    wasmos_wasm_runtime_t* runtime;
    wasmos_wasm_coroutine_t* waiters;            /* wait_next-linked parked tasks */
    wasmos_future_continuation_t* continuations; /* next-linked, dispatched on settle */
};

/* The settle side of a future.  Held by whoever completes the operation; the
 * future side is held by whoever waits on it. */
struct wasmos_promise {
    wasmos_future_t* future;
};

/* Caller-owned stackless task record.  `next` links the ready queue and
 * `wait_next` the waiter list of the future it is parked on, so a record may
 * only be re-started once it is DEAD. */
struct wasmos_wasm_coroutine {
    wasmos_wasm_runtime_t* runtime;
    wasmos_wasm_coroutine_t* next;
    wasmos_wasm_coroutine_t* wait_next;
    wasmos_wasm_task_resume_fn resume;
    void* user;
    wasmos_wasm_coroutine_state_t state;
    int32_t result; /* the resume function's final return */
    wasmos_future_t completion;
    wasmos_promise_t completion_promise;
};

/* Caller-owned registration of one then()/group callback pair.  It owns the
 * `child` future returned by wasmos_future_then(), so it must outlive both the
 * callback and any future chained off that child.  `active` is true from
 * registration until the callback is dispatched or the record is unlinked. */
struct wasmos_future_continuation {
    wasmos_future_continuation_t* next;
    wasmos_future_t* future; /* source future; NULL once dispatched */
    wasmos_future_success_fn_t on_success;
    wasmos_future_error_fn_t on_error;
    void* user;
    /* Owning group for a race/all member. wasmos_future_then_flat() reuses this
     * slot to carry the adoption continuation instead, so it is only a
     * wasmos_future_group_t* when the continuation was registered by a group. */
    wasmos_future_group_t* group;
    size_t group_index;
    wasmos_future_t child;
    wasmos_promise_t child_promise;
    bool active;
};

/* RACE settles from the first source outcome; ALL resolves with the values
 * array once every source resolves, and rejects on the first failure. */
typedef enum { WASMOS_FUTURE_GROUP_RACE = 0, WASMOS_FUTURE_GROUP_ALL } wasmos_future_group_kind_t;
/* Once the group future settles the runtime unlinks every still-pending source
 * continuation, so group, continuations, and values only need to stay live
 * until the group future settles - not until every source settles. */
struct wasmos_future_group {
    wasmos_wasm_runtime_t* runtime;
    wasmos_future_t future; /* the group's own result future */
    wasmos_promise_t promise;
    wasmos_future_continuation_t* continuations; /* caller array of `count` records */
    uintptr_t* values;                           /* ALL only: caller array of `count` slots */
    size_t count;
    size_t completed; /* source callbacks run so far */
    wasmos_future_group_kind_t kind;
    bool settled; /* the group future has been resolved/rejected */
    bool active;  /* callbacks are still accepted */
};

/* Caller-owned scheduler state: one FIFO of runnable tasks and one FIFO of
 * continuations whose source future has settled.  `running` guards against
 * re-entering run()/run_budget() from inside a task or callback. */
struct wasmos_wasm_runtime {
    wasmos_wasm_coroutine_t* current; /* task being resumed, else NULL */
    wasmos_wasm_coroutine_t* ready_head;
    wasmos_wasm_coroutine_t* ready_tail;
    wasmos_future_continuation_t* continuation_head;
    wasmos_future_continuation_t* continuation_tail;
    bool running;
};

/* Zero a caller-owned runtime.  All task, future, continuation and group
 * storage stays caller-owned for its full lifetime; the runtime allocates
 * nothing.  A NULL runtime is ignored. */
void wasmos_wasm_runtime_init(wasmos_wasm_runtime_t* runtime);
/* Overwrite `coroutine` with a task driven by `resume`, queue it READY on the
 * runtime, and return its completion future (a pointer into the record, valid
 * as long as the record is).  Returns NULL for a NULL argument or a record that
 * is neither NEW nor DEAD - a queued, running or waiting record would corrupt
 * the ready list through its reused link fields.  Nothing runs until
 * wasmos_wasm_coroutine_run[_budget](). */
wasmos_future_t* wasmos_async_start(wasmos_wasm_runtime_t* runtime,
                                    wasmos_wasm_coroutine_t* coroutine,
                                    wasmos_wasm_task_resume_fn resume, void* user);
/* Resume at most `budget` ready tasks, then dispatch queued continuations until
 * that queue is empty (continuation dispatch is not bounded by `budget`).
 * Returns the number of task resumes performed, or -1 when runtime is NULL or a
 * run is already in progress on this runtime.  A task that returns anything but
 * TASK_YIELDED becomes DEAD and settles its completion future. */
int wasmos_wasm_coroutine_run_budget(wasmos_wasm_runtime_t* runtime, size_t budget);
/* run_budget() with an unbounded budget: returns once no task is runnable and
 * no continuation is queued.  A task that yields unconditionally never lets
 * this return. */
int wasmos_wasm_coroutine_run(wasmos_wasm_runtime_t* runtime);
/* Returns WASMOS_WASM_TASK_YIELDED.  It does not suspend anything by itself -
 * the calling resume function must return this value for the yield to happen. */
int wasmos_wasm_coroutine_yield(void);
/* Await a task's completion future.  Inherits wasmos_future_await()'s stackless
 * contract: from inside a running task on an unfinished coroutine it parks the
 * caller and returns WASMOS_WASM_AWAIT_PENDING, so the caller must return
 * TASK_YIELDED; called outside a task it returns -1 unless the coroutine has
 * already finished.  On a zero status it writes the task's completion value to
 * out_result. */
int wasmos_wasm_coroutine_join(wasmos_wasm_coroutine_t* coroutine, int32_t* out_result);

/* Reset `future` to PENDING and bind `promise` to it.  Both pointers are
 * required; a NULL argument leaves both untouched.  This drops any waiter and
 * continuation links the future held, so only call it on an unused future. */
void wasmos_future_init(wasmos_future_t* future, wasmos_promise_t* promise);
/* Non-destructive test for settlement: returns false (and writes nothing) while
 * the future is pending or NULL, otherwise true with the status and value
 * copied out.  Never blocks and never consumes the result. */
bool wasmos_future_poll(const wasmos_future_t* future, int32_t* out_status, uintptr_t* out_value);
/* Returns the settled future's status (0 or negative), or
 * WASMOS_WASM_AWAIT_PENDING after parking the current stackless task, in which
 * case the caller must return TASK_YIELDED immediately without touching
 * out_value. Returns -1 without parking when there is no future, no running
 * coroutine, or the future belongs to another runtime. */
int wasmos_future_await(wasmos_future_t* future, uintptr_t* out_value);
/* Settle the promise's future READY with `value`, waking its parked tasks and
 * queueing its continuations on the runtime (callbacks run later, from
 * run[_budget](), never inline).  Returns false when the promise is NULL or
 * unbound, or the future has already settled. */
bool wasmos_promise_resolve(wasmos_promise_t* promise, uintptr_t value);
/* Settle the promise's future FAILED with `status`, which MUST be negative:
 * a zero or positive status settles nothing and returns false, as does an
 * already-settled future. */
bool wasmos_promise_reject(wasmos_promise_t* promise, int32_t status);
/* Register `continuation` on `future` and return the child future it owns, or
 * NULL when an argument is NULL, the continuation is already active, or the
 * future is pinned to a different runtime.  Either callback may be NULL: a
 * missing on_success forwards the source value, a missing on_error forwards the
 * source status.  The callback is dispatched from run[_budget](), including
 * when the source has already settled at registration time.  The returned
 * pointer stays valid as long as the continuation record does. */
wasmos_future_t* wasmos_future_then(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                    wasmos_future_continuation_t* continuation,
                                    wasmos_future_success_fn_t on_success,
                                    wasmos_future_error_fn_t on_error, void* user);
/* A success callback may return WASMOS_FUTURE_CHAIN_NEXT with the next future
 * in out_value. The returned child adopts that future's eventual result. */
/* `adopt_continuation` is a second caller-owned record, used only on that
 * CHAIN_NEXT path to register on the next future; it must be distinct from
 * `continuation`, inactive, and live until the child settles.  Returns NULL on
 * the same conditions as wasmos_future_then(), or when adopt_continuation is
 * NULL.  If the callback signals CHAIN_NEXT with a NULL or unregisterable next
 * future, the child is rejected with -1.  Has no native counterpart. */
wasmos_future_t* wasmos_future_then_flat(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                         wasmos_future_continuation_t* continuation,
                                         wasmos_future_continuation_t* adopt_continuation,
                                         wasmos_future_success_fn_t on_success,
                                         wasmos_future_error_fn_t on_error, void* user);
/* Combine `count` futures into one.  `inputs` is read during the call only;
 * `group` and `continuations[count]` must stay live until the group future
 * settles.  race() settles from the first source outcome; all() additionally
 * needs `values[count]` and resolves with that array (as a uintptr_t) once every
 * source resolves, or rejects with the first failure status.  Both return the
 * group's future, or NULL when an argument is NULL, count is 0, all() has no
 * values array, an input is NULL or pinned to another runtime, or a
 * continuation is already active. */
wasmos_future_t* wasmos_future_race(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                                    wasmos_future_t* const* inputs, size_t count,
                                    wasmos_future_continuation_t* continuations);
wasmos_future_t* wasmos_future_all(wasmos_wasm_runtime_t* runtime, wasmos_future_group_t* group,
                                   wasmos_future_t* const* inputs, size_t count, uintptr_t* values,
                                   wasmos_future_continuation_t* continuations);

/* Variadic sugar over race()/all(): the futures are passed as trailing
 * arguments and `count` is derived from them, so `continuations` (and `values`)
 * must have at least that many elements. */
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
