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

/* Lifecycle of a coroutine record. NEW and DEAD are the only states from which
 * spawn() accepts a record; READY means queued on the runtime's ready list,
 * RUNNING means it owns the CPU, and WAITING means it is parked on a future's
 * waiter list with its register context saved. */
typedef enum wasmos_native_coroutine_state {
    WASMOS_NATIVE_COROUTINE_NEW = 0,
    WASMOS_NATIVE_COROUTINE_READY,
    WASMOS_NATIVE_COROUTINE_RUNNING,
    WASMOS_NATIVE_COROUTINE_WAITING,
    WASMOS_NATIVE_COROUTINE_DEAD,
} wasmos_native_coroutine_state_t;

/* Settlement state of a future. PENDING is the only state a promise may still
 * settle from; READY carries `value`, FAILED carries a negative `status`. */
typedef enum wasmos_future_state {
    WASMOS_FUTURE_PENDING = 0,
    WASMOS_FUTURE_READY,
    WASMOS_FUTURE_FAILED,
} wasmos_future_state_t;

/* Saved machine state of a suspended coroutine: the stack pointer, the
 * platform ABI's callee-saved registers, and the address to resume at. The
 * field order is part of the contract with coroutine_native_<arch>.S, which
 * addresses these by byte offset; coroutine_native.c static-asserts the
 * offsets and the struct size. */
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

/* Caller-owned single-settlement cell. `runtime` is bound lazily by the first
 * await/then/group that touches the future and pins it to that runtime. */
typedef struct wasmos_future {
    wasmos_future_state_t state;
    int32_t status; /* 0 while pending or ready; negative once FAILED */
    uintptr_t value;
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_t* waiters;          /* wait_next-linked suspended coroutines */
    wasmos_future_continuation_t* continuations; /* next-linked, queued on settle */
} wasmos_future_t;

/* The settle side of a future. Held by whoever completes the operation; the
 * future side is held by whoever waits on it. */
typedef struct wasmos_promise {
    wasmos_future_t* future;
} wasmos_promise_t;

/* RACE settles from the first source outcome; ALL resolves with the values
 * array once every source resolves, and rejects on the first failure. */
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
    wasmos_future_t future; /* the group's own result future */
    wasmos_promise_t promise;
    wasmos_future_continuation_t* continuations; /* caller array of `count` records */
    uintptr_t* values;                           /* ALL only: caller array of `count` slots */
    size_t count;
    size_t completed; /* source callbacks run so far */
    wasmos_future_group_kind_t kind;
    bool settled; /* the group future has been resolved/rejected */
    bool active;  /* callbacks are still accepted; also blocks reuse of this record */
};

/* Body of a coroutine. Returning from it exits the coroutine with result 0,
 * exactly as if it had called wasmos_native_coroutine_exit(0). */
typedef void (*wasmos_native_coroutine_entry_t)(void* arg);

/* Caller-owned continuation registration. A registration may be active on one
 * future at a time and must outlive the callback or cancellation of the
 * runtime. The record owns the child future returned by future_then(); do not
 * reuse it while that child or a future chained from it is still referenced.
 * Callbacks run from wasmos_native_coroutine_run[_budget](), never inline from
 * resolve/reject. */
struct wasmos_future_continuation {
    wasmos_future_continuation_t* next;
    wasmos_future_t* future; /* source future; NULL once dispatched */
    wasmos_future_success_fn_t on_success;
    wasmos_future_error_fn_t on_error;
    void* user;
    wasmos_future_group_t* group; /* set only for a race/all member */
    size_t group_index;
    wasmos_future_t child;
    wasmos_promise_t child_promise;
    bool active;
};

/* Caller-owned coroutine record. `next` links the ready queue and `wait_next`
 * the waiter list of the future it is parked on, so a record may only be
 * re-spawned once it is DEAD. The stack it was spawned with is not referenced
 * here; the caller keeps it alive for the coroutine's whole life. */
struct wasmos_native_coroutine {
    wasmos_native_coroutine_context_t context;
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_entry_t entry;
    void* arg;
    wasmos_native_coroutine_state_t state;
    int32_t result; /* value passed to exit(), or 0 when the entry returned */
    wasmos_native_coroutine_t* next;
    wasmos_native_coroutine_t* wait_next;
    wasmos_future_t completion;
    wasmos_promise_t completion_promise;
};

/* Caller-owned scheduler state: the context to switch back to when a coroutine
 * suspends, one FIFO of runnable coroutines, and one FIFO of continuations
 * whose source future has settled. `running` guards against re-entering
 * run()/run_budget() from inside a coroutine or callback. */
struct wasmos_native_coroutine_runtime {
    wasmos_native_coroutine_context_t scheduler_context;
    wasmos_native_coroutine_t* current; /* coroutine on the CPU, else NULL */
    wasmos_native_coroutine_t* ready_head;
    wasmos_native_coroutine_t* ready_tail;
    wasmos_future_continuation_t* continuation_head;
    wasmos_future_continuation_t* continuation_tail;
    bool running;
};

/* All state and stack memory remain caller-owned for their full lifetime. */
void wasmos_native_coroutine_runtime_init(wasmos_native_coroutine_runtime_t* runtime);
/* Prepare `coroutine` to run `entry(arg)` on [stack_base, stack_base+stack_size)
 * and queue it READY. The stack is borrowed, must be at least 1024 bytes, and
 * must stay mapped and unused by anything else until the coroutine is DEAD; it
 * is used downwards from its 16-byte-aligned top. Returns 0 on
 * success, -1 for a NULL argument, an undersized stack, or a record that is
 * neither NEW nor DEAD (re-spawning a queued/running/waiting record would
 * corrupt the ready list through its reused link fields). Nothing runs until
 * wasmos_native_coroutine_run[_budget](). */
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
/* Requeue the running coroutine at the tail of the ready list and switch back
 * to the scheduler; returns to the caller when the coroutine is resumed. Traps
 * when there is no running coroutine, so it must not be called from the
 * scheduler stack (a service idle hook) or from a continuation callback. */
void wasmos_native_coroutine_yield(void);
/* Terminate the running coroutine, marking it DEAD and RESOLVING its completion
 * future with `result` - a negative result is still a resolve, not a reject, so
 * join() reports it through out_result and not as a status. Traps when there is
 * no running coroutine. Its stack may be reused once it is DEAD. */
void wasmos_native_coroutine_exit(int32_t result) __attribute__((noreturn));
/* Await a coroutine's completion future. Stackful, so from another coroutine
 * this suspends until the target finishes and then returns 0, writing the exit
 * result to out_result. Returns -1 without suspending when called outside a
 * running coroutine on a coroutine that has not finished yet. */
int wasmos_native_coroutine_join(wasmos_native_coroutine_t* coroutine, int32_t* out_result);

/* Reset `future` to PENDING and bind `promise` to it. Both pointers are
 * required; a NULL argument leaves both untouched. This drops any waiter and
 * continuation links the future held, so only call it on an unused future. */
void wasmos_future_init(wasmos_future_t* future, wasmos_promise_t* promise);
/* Non-destructive test for settlement: returns false (and writes nothing) while
 * the future is pending or NULL, otherwise true with the status and value
 * copied out. Never blocks and never consumes the result. */
bool wasmos_future_poll(const wasmos_future_t* future, int32_t* out_status, uintptr_t* out_value);
/* Suspends the calling coroutine until the future settles, then returns its
 * status (0 or negative). Stackful, so unlike the WASM variant it returns only
 * once the result is available and the caller keeps its locals. Returns -1
 * without suspending when there is no future, no running coroutine, or the
 * future belongs to another runtime. */
int wasmos_future_await(wasmos_future_t* future, uintptr_t* out_value);
/* Registers a scheduled transformation and returns its child future, or NULL
 * on invalid input. A missing success/error callback forwards that outcome.
 * A callback's non-zero return rejects the child, with a non-negative status
 * normalised to -1. There is no native then_flat: the WASM-only
 * WASMOS_FUTURE_CHAIN_NEXT protocol has no counterpart here, and a stackful
 * coroutine chains by awaiting the next future directly. */
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
/* Both return NULL without touching the group when an argument is NULL, count
 * is 0, the group is still active from an earlier unsettled call, all() has no
 * values array, or an input is NULL, pinned to another runtime, or paired with
 * an already-active continuation. */
wasmos_future_t* wasmos_future_race(wasmos_native_coroutine_runtime_t* runtime,
                                    wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                    size_t count, wasmos_future_continuation_t* continuations);
wasmos_future_t* wasmos_future_all(wasmos_native_coroutine_runtime_t* runtime,
                                   wasmos_future_group_t* group, wasmos_future_t* const* inputs,
                                   size_t count, uintptr_t* values,
                                   wasmos_future_continuation_t* continuations);

/* Variadic sugar over race()/all(): the futures are passed as trailing
 * arguments and `count` is derived from them, so `continuations` (and `values`)
 * must have at least that many elements. */
#define WASMOS_FUTURE_RACE(runtime, group, continuations, ...)                                     \
    wasmos_future_race((runtime),                                                                  \
                       (group),                                                                    \
                       (wasmos_future_t*[]){__VA_ARGS__},                                          \
                       sizeof((wasmos_future_t*[]){__VA_ARGS__}) / sizeof(wasmos_future_t*),       \
                       (continuations))

#define WASMOS_FUTURE_ALL(runtime, group, values, continuations, ...)                              \
    wasmos_future_all((runtime),                                                                   \
                      (group),                                                                     \
                      (wasmos_future_t*[]){__VA_ARGS__},                                           \
                      sizeof((wasmos_future_t*[]){__VA_ARGS__}) / sizeof(wasmos_future_t*),        \
                      (values),                                                                    \
                      (continuations))

/* Settle the promise's future READY with `value`, waking its suspended waiters
 * and queueing its continuations on the runtime (callbacks run later, from
 * run[_budget](), never inline). Returns false when the promise is NULL or
 * unbound, or the future has already settled. */
bool wasmos_promise_resolve(wasmos_promise_t* promise, uintptr_t value);
/* Settle the promise's future FAILED with `status`, which MUST be negative: a
 * zero or positive status settles nothing and returns false, as does an
 * already-settled future. */
bool wasmos_promise_reject(wasmos_promise_t* promise, int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_COROUTINE_NATIVE_H */
