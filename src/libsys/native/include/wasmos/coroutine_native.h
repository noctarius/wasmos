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

typedef enum wasmos_native_future_state {
    WASMOS_NATIVE_FUTURE_PENDING = 0,
    WASMOS_NATIVE_FUTURE_READY,
    WASMOS_NATIVE_FUTURE_FAILED,
} wasmos_native_future_state_t;

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

typedef struct wasmos_native_coroutine wasmos_native_coroutine_t;
typedef struct wasmos_native_coroutine_runtime wasmos_native_coroutine_runtime_t;

typedef struct wasmos_native_future {
    wasmos_native_future_state_t state;
    int32_t status;
    uintptr_t value;
    wasmos_native_coroutine_t* waiters;
} wasmos_native_future_t;

typedef struct wasmos_native_promise {
    wasmos_native_future_t* future;
} wasmos_native_promise_t;

typedef void (*wasmos_native_coroutine_entry_t)(void* arg);

struct wasmos_native_coroutine {
    wasmos_native_coroutine_context_t context;
    wasmos_native_coroutine_runtime_t* runtime;
    wasmos_native_coroutine_entry_t entry;
    void* arg;
    wasmos_native_coroutine_state_t state;
    int32_t result;
    wasmos_native_coroutine_t* next;
    wasmos_native_coroutine_t* wait_next;
    wasmos_native_future_t completion;
    wasmos_native_promise_t completion_promise;
};

struct wasmos_native_coroutine_runtime {
    wasmos_native_coroutine_context_t scheduler_context;
    wasmos_native_coroutine_t* current;
    wasmos_native_coroutine_t* ready_head;
    wasmos_native_coroutine_t* ready_tail;
};

/* All state and stack memory remain caller-owned for their full lifetime. */
void wasmos_native_coroutine_runtime_init(wasmos_native_coroutine_runtime_t* runtime);
int wasmos_native_coroutine_spawn(wasmos_native_coroutine_runtime_t* runtime,
                                  wasmos_native_coroutine_t* coroutine, void* stack_base,
                                  size_t stack_size, wasmos_native_coroutine_entry_t entry,
                                  void* arg);

/* Run ready work until no coroutine is runnable. Returns the number resumed,
 * or -1 for invalid/reentrant use. */
int wasmos_native_coroutine_run(wasmos_native_coroutine_runtime_t* runtime);
void wasmos_native_coroutine_yield(void);
void wasmos_native_coroutine_exit(int32_t result) __attribute__((noreturn));
int wasmos_native_coroutine_join(wasmos_native_coroutine_t* coroutine, int32_t* out_result);

void wasmos_native_future_init(wasmos_native_future_t* future, wasmos_native_promise_t* promise);
bool wasmos_native_future_poll(const wasmos_native_future_t* future, int32_t* out_status,
                               uintptr_t* out_value);
int wasmos_native_future_await(wasmos_native_future_t* future, uintptr_t* out_value);
bool wasmos_native_promise_resolve(wasmos_native_promise_t* promise, uintptr_t value);
bool wasmos_native_promise_reject(wasmos_native_promise_t* promise, int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_COROUTINE_NATIVE_H */
