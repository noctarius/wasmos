# Coroutine, Future, and Promise Runtime Design for WASMOS

> **Documentation status: Mixed reference and proposal.** A native,
> single-worker cooperative coroutine and future/promise cores are implemented
> for native and C WASM guests; native non-blocking IPC-to-future adaptation is
> also implemented. Multi-worker scheduling, timers, CQ integration, WASM IPC
> adaptation, and language-specific WASM wrappers remain proposed. Section 48
> maps the broader design onto existing kernel and libsys primitives.

**Status:** Proposed — verified against the implementation on 2026-07-18  
**Target:** WASMOS user-space runtime on an SMP, timer-preemptive microkernel  
**Primary language model:** C-compatible runtime API  
**Execution model:** M:N stackful coroutines over kernel-scheduled worker threads (native); single-worker cooperative / stackless for WASM guests  

> **Verification note (2026-07-18).** Sections 1–47 are the original aspirational
> design. They have been checked against the current WASMOS kernel and user-space
> ABI. The reconciliation lives in two new sections at the end:
>
> - **[§48 Implementation Reality Baseline](#48-implementation-reality-baseline-verified-2026-07-18)** — a primitive-by-primitive
>   mapping of what this design assumes to what actually exists, plus the required
>   adjustments (the biggest: synchronous request/response IPC is being **removed**
>   and the future/promise model *replaces* it — nothing new is layered on top of
>   synchronous IPC, and the `sys_ipc_submit`/completion-token async-IPC syscalls
>   are neither present nor required; the clock is coarse; `futex` is the real
>   wait/wake primitive but is not yet surfaced to libc).
> - **[§49 Wrapper Architecture](#49-wrapper-architecture-one-contract-native-and-wasm-cores)** — how a *single* future/promise
>   contract is wrapped across both execution substrates (native and WASM) and all
>   five language shims (C, Rust, Go, Zig, AssemblyScript). The key finding is that
>   the **coroutine context-switch layer cannot be one shared implementation**: the
>   native stackful core (`coroutine_switch`) works for ring-3 native code but not
>   for wasm3-interpreted or WARP-JIT guest stacks, and the per-process
>   `runtime_lock` serializes WASM runtime execution. Coroutines are still
>   available to both, via two distinct L2 implementations over one L1 future core.
>
> Inline `> Verified 2026-07-18:` call-outs flag the sections whose specifics were
> corrected.

---

## 1. Summary

This document defines a generic coroutine runtime for WASMOS and describes how futures and promises integrate with it.

The design separates three concerns:

1. The **kernel scheduler** schedules kernel threads onto CPUs.
2. The **coroutine scheduler** schedules user-space coroutines onto worker threads.
3. The **future/promise subsystem** represents asynchronous completion and wakes suspended coroutines when operations finish.

The proposed runtime uses an M:N model:

```text
M coroutines
    ↓ user-space scheduling
N worker threads
    ↓ kernel scheduling
P logical CPUs
```

The kernel does not schedule individual coroutines. It schedules worker threads. Coroutines are scheduled entirely by the runtime except where kernel facilities are needed for IPC, timers, waiting, waking, and optional preemption notifications.

The central runtime rule is:

> Blocking a coroutine must not block its worker thread unless the worker has no other runnable work.

IPC, timers, joins, synchronization operations, and device requests are therefore exposed to coroutines as asynchronous operations represented by futures. Awaiting a future suspends only the current coroutine.

---

## 2. Goals

The runtime shall:

- Support many lightweight stackful coroutines.
- Run coroutines concurrently across multiple CPUs.
- Use one or more kernel worker threads per process.
- Support coroutine migration between workers.
- Preserve cache locality through per-worker queues.
- Support cooperative yielding.
- Support timer-driven scheduling through safe-point preemption.
- Integrate naturally with asynchronous microkernel IPC.
- Provide generic futures, promises, timers, joins, and cancellation.
- Avoid lost wakeups and double execution under SMP.
- Permit incremental implementation, beginning with a simple lock-based design.
- Permit later replacement of internal queues with lock-free implementations.
- Keep the kernel independent of coroutine-specific data structures.

---

## 3. Non-goals

The initial implementation does not attempt to provide:

- Transparent asynchronous preemption at arbitrary machine instructions.
- Scheduler activations or arbitrary kernel-to-user upcalls.
- Distributed futures across process boundaries.
- Automatic deadlock detection.
- Priority inheritance between arbitrary futures.
- Fully lock-free runtime data structures.
- Automatic segmented or copying stacks.
- Language-level compiler transformation for stackless `async` functions.

These features may be added later without changing the core model.

---

## 4. Terminology

### 4.1 Kernel thread

A schedulable execution context known to the kernel. A kernel thread has kernel-managed CPU state and can be preempted by the kernel scheduler.

### 4.2 Worker

A kernel thread participating in the coroutine runtime. A worker executes at most one coroutine at a time and owns a local coroutine run queue.

### 4.3 Coroutine

A user-space execution context with its own stack, saved register state, lifecycle state, and scheduling metadata.

### 4.4 Future

A consumer-facing handle to a value or error that may become available later.

### 4.5 Promise

A producer-facing handle that may complete, reject, or cancel the shared state observed through a future.

### 4.6 Await

An operation that either returns an already available future result or suspends the current coroutine until completion.

### 4.7 Completion

A transition of a future from pending to a terminal state such as ready, failed, or cancelled.

### 4.8 Safe point

A location where coroutine code may yield without exposing partially modified runtime state or violating calling conventions.

---

## 5. Architectural Overview

```text
+--------------------------------------------------------------+
| Application / service code                                   |
|                                                              |
| coroutine_spawn()   future_await()   ipc_call_async()        |
+----------------------------+---------------------------------+
                             |
+----------------------------v---------------------------------+
| Coroutine runtime                                            |
|                                                              |
| - coroutine lifecycle                                        |
| - per-worker run queues                                      |
| - work stealing                                              |
| - timer queues                                               |
| - future/promise shared state                                |
| - cancellation                                               |
| - completion dispatch                                        |
+----------------------------+---------------------------------+
                             |
+----------------------------v---------------------------------+
| WASMOS syscall interface                                     |
|                                                              |
| - yield                                                      |
| - wait/wake                                                  |
| - asynchronous IPC submission                                |
| - completion/event waiting                                   |
| - monotonic clock                                            |
| - optional timer notification                                |
+----------------------------+---------------------------------+
                             |
+----------------------------v---------------------------------+
| WASMOS microkernel                                           |
|                                                              |
| - SMP kernel scheduler                                       |
| - timer preemption                                           |
| - IPC routing                                                |
| - event delivery                                             |
| - CPU wakeup                                                 |
+--------------------------------------------------------------+
```

The runtime is responsible for coroutine fairness within a process. The kernel remains responsible for fairness between kernel threads and processes.

A kernel preemption of a worker does not change the coroutine's runtime state. The coroutine remains `CORO_RUNNING`; its worker is merely not currently executing on a CPU.

---

## 6. Scheduling Model

### 6.1 M:N scheduling

The default model is M:N scheduling:

```text
coroutine 0 ─┐
coroutine 1 ─┼──> worker 0 ──> CPU 0
coroutine 2 ─┘

coroutine 3 ─┐
coroutine 4 ─┼──> worker 1 ──> CPU 1
coroutine 5 ─┘
```

The runtime normally creates one worker per CPU granted to the process. It may use fewer workers when CPU allocation is restricted.

### 6.2 Worker count

An initial policy may use:

```text
worker_count = min(online_cpu_count, configured_worker_limit)
```

A process may later support dynamic worker allocation.

### 6.3 Locality

A coroutine should normally resume on the same worker on which it last executed. This improves:

- stack cache locality;
- runtime metadata locality;
- JIT code and data locality;
- CPU-local allocator behavior.

Migration is allowed when another worker steals the coroutine.

---

## 7. Coroutine Data Model

### 7.1 States

```c
typedef enum coroutine_state {
    CORO_NEW = 0,
    CORO_RUNNABLE,
    CORO_RUNNING,
    CORO_WAIT_FUTURE,
    CORO_WAIT_TIMER,
    CORO_WAIT_IPC,
    CORO_WAIT_JOIN,
    CORO_CANCELLED,
    CORO_DEAD,
} coroutine_state_t;
```

`CORO_WAIT_TIMER`, `CORO_WAIT_IPC`, and `CORO_WAIT_JOIN` may be represented internally as specialized future waits. Keeping separate states during initial development improves diagnostics.

### 7.2 State machine

```text
                   +----------------+
                   |      NEW       |
                   +--------+-------+
                            |
                            v
                   +----------------+
          +------->|    RUNNABLE    |<--------------------+
          |        +--------+-------+                     |
          |                 |                             |
          |                 v                             |
          |        +----------------+                     |
          |        |    RUNNING     |                     |
          |        +--+---+---+--+--+                     |
          |           |   |   |  |                        |
          | yield     |   |   |  | return/exit            |
          +-----------+   |   |  +---------> DEAD         |
                          |   |                           |
                          |   +-------> WAIT_TIMER -------+
                          |                               |
                          +-----------> WAIT_FUTURE ------+
```

Cancellation may move a waiting coroutine toward `RUNNABLE` with a cancellation result or directly toward termination depending on API policy.

### 7.3 Core structure

```c
typedef struct cpu_context {
    uintptr_t rsp;
    uintptr_t rbx;
    uintptr_t rbp;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
} cpu_context_t;

typedef struct coroutine coroutine_t;
typedef struct future_waiter future_waiter_t;

typedef struct coroutine {
    uint64_t id;

    cpu_context_t context;

    void *stack_mapping;
    size_t stack_mapping_size;
    void *stack_base;
    size_t stack_size;

    void (*entry)(void *arg);
    void *arg;
    void *result;

    _Atomic coroutine_state_t state;
    _Atomic uint32_t owner_worker;
    _Atomic uint32_t ref_count;

    uint32_t flags;
    int32_t pinned_worker;

    struct coroutine *run_next;
    struct coroutine *reap_next;

    future_waiter_t *active_waiter;

    int completion_status;
    bool detached;
} coroutine_t;
```

### 7.4 Ownership invariant

Every live coroutine must be in exactly one scheduling location:

- `RUNNING`: owned by exactly one worker and present in no queue;
- `RUNNABLE`: present in exactly one run queue;
- waiting: registered in exactly one logical wait operation;
- `DEAD`: present only in join, reference, or reap structures.

A coroutine must never be runnable and registered as a waiter simultaneously.

### 7.5 Owner field

```c
#define CORO_NO_OWNER UINT32_MAX
```

A worker claims a runnable coroutine before executing it:

```c
bool coroutine_claim(coroutine_t *coro, uint32_t worker_id)
{
    uint32_t expected = CORO_NO_OWNER;

    return atomic_compare_exchange_strong_explicit(
        &coro->owner_worker,
        &expected,
        worker_id,
        memory_order_acquire,
        memory_order_relaxed);
}
```

The owner field is a defensive mechanism. Correct queue ownership should already prevent double execution, but the atomic claim detects scheduler corruption.

---

## 8. Stack Management

### 8.1 Initial policy

Each coroutine receives:

- a reserved virtual-memory range;
- a fixed committed stack region;
- at least one unmapped guard page.

```text
high address
+----------------------------+
| usable coroutine stack     |
|                            |
+----------------------------+
| guard page                 |
+----------------------------+
low address
```

Suggested initial defaults:

```text
virtual reservation: 1 MiB
committed stack:     64 KiB
minimum guard:        4 KiB or architecture page size
```

If virtual-memory reservation and lazy commitment are not yet available, allocate a fixed stack and guard page.

### 8.2 Alignment

On x86-64 System V, the initial stack must preserve 16-byte alignment at function-call boundaries.

The initial stack is prepared so that the first context restore returns into `coroutine_trampoline()`.

### 8.3 Red zone

If asynchronous user-space preemption is introduced, runtime and coroutine code should be built with:

```text
-mno-red-zone
```

Cooperative switching alone does not require disabling the red zone because switches occur at explicit call boundaries.

### 8.4 Stack growth

Automatic stack growth is deferred. It may later be implemented with guard-page faults, provided page-fault resolution cannot deadlock the process when workers are suspended.

---

## 9. Context Switching

### 9.1 Cooperative context

A cooperative switch on x86-64 SysV needs to preserve callee-saved general-purpose registers:

- `RBX`;
- `RBP`;
- `R12` through `R15`;
- `RSP`.

The return address stored on the stack acts as the saved instruction pointer.

### 9.2 Interface

```c
void coroutine_switch(cpu_context_t *from, const cpu_context_t *to);
```

### 9.3 Conceptual x86-64 implementation

```asm
coroutine_switch:
    mov [rdi + CTX_RSP], rsp
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R15], r15

    mov rsp, [rsi + CTX_RSP]
    mov rbx, [rsi + CTX_RBX]
    mov rbp, [rsi + CTX_RBP]
    mov r12, [rsi + CTX_R12]
    mov r13, [rsi + CTX_R13]
    mov r14, [rsi + CTX_R14]
    mov r15, [rsi + CTX_R15]

    ret
```

Exact offsets and unwind annotations are architecture-specific.

### 9.4 SIMD and FPU state

The initial cooperative runtime treats SIMD/FPU registers according to the normal ABI: caller-saved SIMD state need not survive an ordinary function call to `coroutine_yield()`.

Hard asynchronous preemption would require preserving the complete interrupted extended state, likely through `XSAVE`/`XRSTOR` or an equivalent kernel-provided context frame.

---

## 10. Coroutine Creation and Exit

### 10.1 Creation API

```c
typedef struct coroutine_options {
    size_t stack_size;
    uint32_t flags;
    int32_t pinned_worker;
    bool detached;
} coroutine_options_t;

int coroutine_spawn(
    coroutine_t **out,
    void (*entry)(void *),
    void *arg,
    const coroutine_options_t *options);
```

### 10.2 Trampoline

```c
static _Noreturn void coroutine_trampoline(void)
{
    worker_t *worker = worker_current();
    coroutine_t *coro = worker->current;

    coro->entry(coro->arg);
    coroutine_exit(NULL);
}
```

### 10.3 Exit

A coroutine may not free its own stack while executing on it.

```c
_Noreturn void coroutine_exit(void *result)
{
    worker_t *worker = worker_current();
    coroutine_t *coro = worker->current;

    coro->result = result;
    coro->completion_status = 0;

    coroutine_complete_join_future(coro);

    atomic_store_explicit(
        &coro->state,
        CORO_DEAD,
        memory_order_release);

    worker->current = NULL;

    coroutine_switch(
        &coro->context,
        &worker->scheduler_context);

    __builtin_unreachable();
}
```

The scheduler later queues the coroutine for reaping when no references remain.

---

## 11. Worker Runtime

### 11.1 Worker structure

```c
typedef struct worker {
    uint32_t id;
    uint32_t preferred_cpu;

    coroutine_t *current;
    cpu_context_t scheduler_context;

    run_queue_t run_queue;
    timer_queue_t timer_queue;

    _Atomic uint32_t sleep_word;
    _Atomic bool sleeping;

    uint32_t preemption_disable_count;
    bool preemption_pending;
    uint64_t slice_deadline;

    completion_buffer_t completions;
} worker_t;
```

### 11.2 Main loop

```c
static _Noreturn void worker_main(worker_t *worker)
{
    worker_set_current(worker);

    for (;;) {
        dispatch_pending_completions(worker);
        expire_local_timers(worker);
        reap_dead_coroutines(worker);

        coroutine_t *coro = run_queue_pop_local(&worker->run_queue);

        if (coro == NULL) {
            coro = worker_steal(worker);
        }

        if (coro == NULL) {
            worker_park(worker);
            continue;
        }

        if (!coroutine_claim(coro, worker->id)) {
            runtime_panic("coroutine double ownership");
        }

        coroutine_state_t expected = CORO_RUNNABLE;
        if (!atomic_compare_exchange_strong_explicit(
                &coro->state,
                &expected,
                CORO_RUNNING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            runtime_panic("invalid runnable coroutine state");
        }

        worker->current = coro;
        worker->slice_deadline = runtime_now() + runtime_quantum();

        coroutine_switch(
            &worker->scheduler_context,
            &coro->context);

        worker->current = NULL;
        atomic_store_explicit(
            &coro->owner_worker,
            CORO_NO_OWNER,
            memory_order_release);
    }
}
```

---

## 12. Yielding

### 12.1 Coroutine yield

`coroutine_yield()` is a user-space scheduler operation. It does not normally invoke the kernel's `yield` syscall.

```c
void coroutine_yield(void)
{
    worker_t *worker = worker_current();
    coroutine_t *coro = worker->current;

    runtime_preempt_disable();

    atomic_store_explicit(
        &coro->state,
        CORO_RUNNABLE,
        memory_order_release);

    run_queue_push_local(&worker->run_queue, coro);

    coroutine_switch(
        &coro->context,
        &worker->scheduler_context);

    runtime_preempt_enable();
}
```

The exact placement of preemption enable/disable depends on the switch implementation. The scheduler must never observe the coroutine simultaneously as running and runnable.

### 12.2 Kernel yield

The kernel `yield` syscall is used only as a scheduling hint when a worker has no runnable coroutines and does not have a better blocking primitive available.

A proper wait/wake syscall is preferred over repeated yielding.

---

## 13. Run Queues and Work Stealing

### 13.1 Initial implementation

Use one lock-protected deque per worker.

```c
typedef struct run_queue {
    spinlock_t lock;
    coroutine_t **items;
    size_t capacity;
    size_t head;
    size_t tail;
} run_queue_t;
```

The owner pushes and pops from one end. Thieves remove work from the opposite end.

### 13.2 Future optimization

The queue may later be replaced with a Chase-Lev work-stealing deque.

### 13.3 Scheduling policy

The scheduler should:

1. prefer the local worker queue;
2. process a bounded batch of local completions;
3. expire local timers;
4. steal from another worker if local work is absent;
5. park only after a lost-wakeup-safe idle transition.

### 13.4 Pinned coroutines

```c
enum {
    CORO_FLAG_PINNED = 1u << 0,
    CORO_FLAG_NO_PREEMPT = 1u << 1,
};
```

Pinned coroutines may only run on `pinned_worker`. Thieves skip pinned entries they cannot execute.

---

## 14. Worker Parking and Lost Wakeups

### 14.1 Race to avoid

The following sequence is invalid:

```text
worker A observes no work
worker B enqueues work
worker B observes A as active and sends no wake
worker A enters kernel sleep
```

### 14.2 Correct parking protocol

```c
static void worker_park(worker_t *worker)
{
    atomic_store_explicit(
        &worker->sleeping,
        true,
        memory_order_release);

    if (worker_has_work(worker) || runtime_has_stealable_work()) {
        atomic_store_explicit(
            &worker->sleeping,
            false,
            memory_order_release);
        return;
    }

    uint32_t observed = atomic_load_explicit(
        &worker->sleep_word,
        memory_order_acquire);

    uint64_t deadline = timer_queue_next_deadline(&worker->timer_queue);

    sys_wait_u32(&worker->sleep_word, observed, deadline);

    atomic_store_explicit(
        &worker->sleeping,
        false,
        memory_order_release);
}
```

The kernel must atomically verify that `sleep_word` still equals `observed` before blocking.

### 14.3 Wake protocol

```c
static void worker_wake(worker_t *worker)
{
    atomic_fetch_add_explicit(
        &worker->sleep_word,
        1,
        memory_order_release);

    if (atomic_load_explicit(
            &worker->sleeping,
            memory_order_acquire)) {
        sys_wake_u32(&worker->sleep_word, 1);
    }
}
```

Spurious wakeups are permitted. Lost wakeups are not.

---

## 15. Preemption Model

### 15.1 Kernel preemption

The kernel may preempt a worker thread at any instruction. This does not alter the coroutine runtime state.

```text
worker running coroutine A
    ↓ kernel timer interrupt
worker descheduled
    ↓ later
same worker resumes coroutine A
```

`A` remains `CORO_RUNNING` throughout.

### 15.2 Coroutine preemption

Coroutine-level fairness is initially implemented through cooperative safe points driven by timer deadlines.

Each worker has a quantum deadline:

```c
worker->slice_deadline = runtime_now() + configured_quantum;
```

Coroutine or generated code checks:

```c
static inline void coroutine_preemption_point(void)
{
    worker_t *worker = worker_current();

    if (runtime_preemption_disabled()) {
        return;
    }

    if (runtime_now() >= worker->slice_deadline) {
        coroutine_yield();
    }
}
```

### 15.3 Safe-point locations

WARP or wasm3 should insert checks at:

- loop back edges;
- selected function prologues;
- long-running runtime calls;
- allocation slow paths;
- IPC boundaries;
- explicit user yield instructions.

### 15.4 Preemption disabling

Runtime internals use a per-worker nesting counter:

```c
void runtime_preempt_disable(void)
{
    worker_current()->preemption_disable_count++;
}

void runtime_preempt_enable(void)
{
    worker_t *worker = worker_current();

    runtime_assert(worker->preemption_disable_count != 0);
    worker->preemption_disable_count--;

    if (worker->preemption_disable_count == 0 &&
        worker->preemption_pending) {
        worker->preemption_pending = false;
        coroutine_yield();
    }
}
```

This mechanism affects only coroutine scheduling. It does not disable kernel interrupts or kernel thread preemption.

### 15.5 Hard asynchronous preemption

Hard preemption is deferred. It would require:

- a kernel-delivered interrupted register frame;
- alternate notification stacks;
- full SIMD/FPU context preservation;
- signal-safe runtime internals;
- red-zone handling;
- deferred preemption inside critical sections;
- careful syscall-return semantics.

Safe-point preemption is strongly preferred for WASM workloads.

---

## 16. Future and Promise Model

### 16.1 Shared state

A future and promise refer to the same shared completion object.

```text
producer owns Promise<T>
          |
          v
   shared future state
          ^
          |
consumers own Future<T>
```

The future cannot complete the state. The promise cannot await it through the public API, although internal code may retain both handles.

### 16.2 Future states

```c
typedef enum future_status {
    FUTURE_PENDING = 0,
    FUTURE_READY,
    FUTURE_FAILED,
    FUTURE_CANCELLED,
} future_status_t;
```

Allowed terminal transitions:

```text
PENDING -> READY
PENDING -> FAILED
PENDING -> CANCELLED
```

A future completes exactly once.

### 16.3 Shared state structure

```c
typedef struct future_value {
    void *data;
    size_t size;
    void (*destroy)(void *data);
} future_value_t;

typedef struct future_waiter future_waiter_t;

typedef struct future_state {
    _Atomic uint32_t ref_count;
    _Atomic future_status_t status;

    spinlock_t lock;
    future_waiter_t *waiters;

    future_value_t value;
    int error;

    void (*cancel_operation)(void *context);
    void *cancel_context;
} future_state_t;

typedef struct future {
    future_state_t *state;
} future_t;

typedef struct promise {
    future_state_t *state;
} promise_t;
```

The initial implementation uses a lock to synchronize waiter registration and completion. This is simpler and safer than beginning with a lock-free linked list.

---

## 17. Generic Waiters

### 17.1 Waiter abstraction

The future subsystem should not be permanently tied to stackful coroutines.

```c
typedef void (*future_wake_fn)(future_waiter_t *waiter);

typedef struct future_waiter {
    struct future_waiter *next;
    future_wake_fn wake;
    _Atomic bool active;
    void *context;
} future_waiter_t;
```

### 17.2 Coroutine waiter

```c
typedef struct coroutine_future_waiter {
    future_waiter_t base;
    coroutine_t *coroutine;
} coroutine_future_waiter_t;
```

### 17.3 Continuation waiter

A later stackless async implementation may register:

```c
typedef struct continuation_waiter {
    future_waiter_t base;
    void (*continuation)(void *context);
    void *continuation_context;
} continuation_waiter_t;
```

This permits stackful coroutines, callbacks, and compiler-generated async tasks to share one future implementation.

---

## 18. Creating Futures and Promises

```c
int future_create(future_t *future, promise_t *promise)
{
    future_state_t *state = runtime_alloc(sizeof(*state));
    if (state == NULL) {
        return ERROR_NO_MEMORY;
    }

    memset(state, 0, sizeof(*state));
    atomic_init(&state->ref_count, 2);
    atomic_init(&state->status, FUTURE_PENDING);
    spinlock_init(&state->lock);

    future->state = state;
    promise->state = state;
    return 0;
}
```

Both handles retain one reference. Copies must explicitly retain the state.

---

## 19. Awaiting a Future

### 19.1 Required behavior

`future_await()` shall:

- return immediately if the future is terminal;
- otherwise register the current coroutine as a waiter;
- transition the coroutine to `CORO_WAIT_FUTURE`;
- switch to the worker scheduler;
- return the value or error after wakeup.

### 19.2 Lost-wakeup prevention

The readiness check and waiter registration must be serialized with promise completion.

The invalid sequence is:

```text
awaiter checks PENDING
completer changes to READY and sees no waiter
awaiter registers waiter and sleeps forever
```

The future lock prevents this race.

### 19.3 Conceptual implementation

```c
int future_await(future_t *future, future_value_t *out)
{
    future_state_t *state = future->state;
    worker_t *worker = worker_current();
    coroutine_t *coro = worker->current;
    coroutine_future_waiter_t waiter;

    coroutine_waiter_init(&waiter, coro);

    runtime_preempt_disable();
    spin_lock(&state->lock);

    future_status_t status = atomic_load_explicit(
        &state->status,
        memory_order_relaxed);

    if (status != FUTURE_PENDING) {
        int result = future_copy_terminal_result_locked(state, out);
        spin_unlock(&state->lock);
        runtime_preempt_enable();
        return result;
    }

    waiter.base.next = state->waiters;
    state->waiters = &waiter.base;
    coro->active_waiter = &waiter.base;

    atomic_store_explicit(
        &coro->state,
        CORO_WAIT_FUTURE,
        memory_order_release);

    spin_unlock(&state->lock);

    coroutine_suspend_current();

    coro->active_waiter = NULL;
    runtime_preempt_enable();

    spin_lock(&state->lock);
    int result = future_copy_terminal_result_locked(state, out);
    spin_unlock(&state->lock);

    return result;
}
```

The waiter is stack-allocated on the coroutine stack and remains valid because that stack remains allocated while the coroutine is suspended.

### 19.4 Suspension boundary

`coroutine_suspend_current()` must switch directly to the scheduler without exposing a window in which the coroutine is waiting but continues executing application code.

---

## 20. Promise Completion

### 20.1 Completion procedure

Promise completion must:

1. acquire the state lock;
2. verify that the state is pending;
3. store the value or error;
4. publish the terminal state;
5. detach the waiter list;
6. release the lock;
7. wake waiters outside the lock.

### 20.2 Resolve

```c
bool promise_resolve(promise_t *promise, future_value_t value)
{
    future_state_t *state = promise->state;
    future_waiter_t *waiters;

    spin_lock(&state->lock);

    if (atomic_load_explicit(
            &state->status,
            memory_order_relaxed) != FUTURE_PENDING) {
        spin_unlock(&state->lock);
        return false;
    }

    state->value = value;

    atomic_store_explicit(
        &state->status,
        FUTURE_READY,
        memory_order_release);

    waiters = state->waiters;
    state->waiters = NULL;

    spin_unlock(&state->lock);

    future_wake_waiter_list(waiters);
    return true;
}
```

### 20.3 Reject

```c
bool promise_reject(promise_t *promise, int error);
```

This performs `PENDING -> FAILED` and wakes all waiters.

### 20.4 Cancel

```c
bool promise_cancel(promise_t *promise, int reason);
```

This performs `PENDING -> CANCELLED` and wakes all waiters.

### 20.5 Wake outside the lock

Waiters must be scheduled after releasing the future lock. Scheduling can involve run-queue locks, worker wake syscalls, allocator activity, or callbacks. Executing these while holding the future lock risks lock inversion and long critical sections.

---

## 21. Waking Coroutine Waiters

```c
static void coroutine_future_wake(future_waiter_t *base)
{
    coroutine_future_waiter_t *waiter =
        container_of(base, coroutine_future_waiter_t, base);

    coroutine_t *coro = waiter->coroutine;

    if (!atomic_exchange_explicit(
            &base->active,
            false,
            memory_order_acq_rel)) {
        return;
    }

    coroutine_state_t expected = CORO_WAIT_FUTURE;

    if (!atomic_compare_exchange_strong_explicit(
            &coro->state,
            &expected,
            CORO_RUNNABLE,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }

    runtime_schedule(coro);
}
```

Exactly one completion, timeout, or cancellation path may win the transition to `CORO_RUNNABLE`.

---

## 22. Future Result Semantics

### 22.1 Shared versus moved values

The generic future supports a stored value with an optional destructor.

For multiple waiters, the value must either:

- be immutable and shared;
- be copied for each waiter;
- contain a reference-counted object.

Move-only result types require a single-consumer future variant.

### 22.2 Typed wrappers

Public APIs should prefer typed wrappers over raw `void *` values.

```c
typedef struct ipc_future {
    future_t base;
} ipc_future_t;

int ipc_future_await(
    ipc_future_t *future,
    ipc_result_t *result);
```

Potential wrappers include:

- `ipc_future_t`;
- `timer_future_t`;
- `process_future_t`;
- `io_future_t`;
- `coroutine_join_future_t`.

---

## 23. IPC Integration

> **Verified 2026-07-18 (updated):** Do not conflate two separate things.
> **(a) The synchronous request/response *pattern*** — `wasmos_ipc_call`, the
> native `IPC_CALL` syscall, and any use of a blocking `ipc_select_one` as a
> *nested reply-wait inside a handler* — **is being removed.** The future/promise
> model is its **replacement**, not a layer on top of it; **nothing new is built
> over synchronous IPC.** **(b) The non-blocking transport** — `ipc_send` (post a
> message) plus **one per-endpoint receive pump** (`ipc_select_wait`) — remains,
> because a reply is simply an ordinary message delivered to the caller's own
> endpoint, correlated by `request_id`. An IPC future is built directly on (b):
> send the request, record `request_id → promise`, keep pumping the single
> receiver, and resolve the promise when the correlated reply arrives — never
> parking in a nested receive. This is the existing `wasmos_sys_event_loop`
> **intent** table promoted to a first-class promise (`intent.on_resolve` is the
> continuation). The `sys_ipc_submit`/`sys_ipc_cancel`/completion-token syscalls
> below **do not exist and are not required**; were a dedicated async
> submit/completion syscall ever added it would be a *new transport primitive* —
> still not layered on the removed synchronous path. See
> [§48.2](#482-required-adjustments) and [§23.3](#233-runtime-request-table). The
> deadlock that motivates removing synchronous IPC is documented in
> [`09-process-and-ipc.md`](09-process-and-ipc.md)
> ("Synchronous request/response IPC — deadlock hazard").

### 23.1 Requirement

A synchronous blocking IPC syscall must not be used directly from a coroutine worker when other coroutines may need to run.

Otherwise:

```text
coroutine blocks in IPC
    ↓
worker kernel thread blocks
    ↓
all coroutines assigned to that worker stop
```

### 23.2 Asynchronous IPC API

Suggested syscall:

```c
int sys_ipc_submit(
    endpoint_t endpoint,
    const ipc_request_t *request,
    uint64_t completion_token);
```

Suggested completion event:

```c
typedef struct ipc_completion {
    uint64_t token;
    int status;
    size_t result_size;
    uint64_t result_handle;
} ipc_completion_t;
```

### 23.3 Runtime request table

The runtime maintains:

```text
request ID -> promise
```

Use monotonically increasing 64-bit IDs or generation-tagged slots. Never rely solely on recycled pointers because late completions may target a newly allocated operation.

### 23.4 IPC submission

```c
ipc_future_t ipc_call_async(
    endpoint_t endpoint,
    const ipc_request_t *request)
{
    ipc_future_t result;
    promise_t promise;

    future_create(&result.base, &promise);

    uint64_t id = request_id_allocate();
    request_table_insert(id, promise);

    int error = sys_ipc_submit(endpoint, request, id);
    if (error != 0) {
        request_table_remove(id, NULL);
        promise_reject(&promise, error);
    }

    promise_release(&promise);
    return result;
}
```

The request table retains the promise state until completion or cancellation.

### 23.5 Completion dispatch

```c
static void dispatch_ipc_completion(const ipc_completion_t *completion)
{
    promise_t promise;

    if (!request_table_remove(completion->token, &promise)) {
        return; /* cancelled, stale, or duplicate completion */
    }

    if (completion->status == 0) {
        future_value_t value = ipc_completion_to_value(completion);
        promise_resolve(&promise, value);
    } else {
        promise_reject(&promise, completion->status);
    }

    promise_release(&promise);
}
```

### 23.6 Incoming message streams

A future represents one completion. Repeated incoming IPC messages should use a channel or stream abstraction:

```text
future: one result
channel: repeated values
```

A channel receive operation may itself return a future.

---

## 24. Kernel Event Waiting

### 24.1 Unified event syscall

A useful kernel primitive is:

```c
int sys_wait_events(
    event_t *events,
    size_t capacity,
    uint64_t absolute_deadline);
```

Possible events include:

- asynchronous IPC completion;
- endpoint readability;
- endpoint writability;
- timer expiration;
- process death;
- IRQ notification;
- worker wake event;
- service lifecycle notification.

### 24.2 Event token

```c
typedef struct event {
    uint64_t token;
    uint32_t type;
    int32_t status;
    uint64_t value;
} event_t;
```

Tokens should identify runtime requests, not raw coroutine pointers.

### 24.3 Dispatcher model

Two valid runtime models are supported.

#### Per-worker event wait

Each worker waits for events when idle. This is simple and naturally parallel.

#### Dedicated event worker

One or more workers primarily consume kernel completions and enqueue resumed coroutines onto normal workers. This can reduce contention in kernels whose event queue has one consumer.

The initial implementation should use per-worker waits unless the kernel completion API requires centralized consumption.

---

## 25. Timers as Futures

### 25.1 API

```c
timer_future_t timer_at(uint64_t absolute_deadline);
timer_future_t timer_after(uint64_t duration);
```

### 25.2 Implementation

A timer object owns a promise. On expiration it resolves the promise.

```text
timer queue entry
    ↓ deadline expires
promise_resolve()
    ↓
waiting coroutine becomes runnable
```

### 25.3 Timer queues

The initial runtime may use one binary min-heap per worker.

```c
typedef struct timer_entry {
    uint64_t deadline;
    promise_t promise;
    _Atomic bool active;
} timer_entry_t;
```

For large timer counts, replace the heap with a hierarchical timing wheel.

### 25.4 Timer ownership

The worker owning the timer entry is responsible for resolving it. The resumed coroutine may be scheduled on any eligible worker.

### 25.5 Sleep API

```c
int coroutine_sleep_until(uint64_t deadline)
{
    timer_future_t timer = timer_at(deadline);
    int result = timer_future_await(&timer);
    timer_future_release(&timer);
    return result;
}
```

---

## 26. Timeouts and Multi-Wait

### 26.1 Await with deadline

```c
int future_await_until(
    future_t *future,
    uint64_t deadline,
    future_value_t *result);
```

This waits for either:

- the future completion;
- the deadline expiration;
- cancellation.

### 26.2 Shared wait operation

A multi-wait uses one atomic winner field:

```c
typedef enum wait_winner {
    WAIT_WINNER_NONE = 0,
    WAIT_WINNER_FUTURE,
    WAIT_WINNER_TIMEOUT,
    WAIT_WINNER_CANCEL,
} wait_winner_t;

typedef struct wait_operation {
    _Atomic wait_winner_t winner;
    coroutine_t *coroutine;
} wait_operation_t;
```

Each candidate attempts:

```c
wait_winner_t expected = WAIT_WINNER_NONE;
if (atomic_compare_exchange_strong(
        &operation->winner,
        &expected,
        WAIT_WINNER_FUTURE)) {
    runtime_make_runnable(operation->coroutine);
}
```

Losing registrations must be removed or marked inactive before their memory is reclaimed.

### 26.3 General select

A future version may support:

```c
int future_select(
    future_t **futures,
    size_t count,
    uint64_t deadline,
    size_t *completed_index,
    future_value_t *value);
```

The first implementation may limit select to one future plus one timeout to reduce cancellation complexity.

---

## 27. Cancellation

### 27.1 Cancellation meanings

Two operations must be distinguished:

1. stop waiting for a result;
2. attempt to cancel the underlying operation.

Stopping a wait does not guarantee that the IPC, I/O, or device operation has stopped.

### 27.2 Cancellation token

```c
typedef struct cancellation_token {
    _Atomic bool cancelled;
    spinlock_t lock;
    cancellation_waiter_t *waiters;
} cancellation_token_t;
```

### 27.3 Future cancellation

A future may register an operation-specific cancellation callback:

```c
state->cancel_operation = ipc_cancel_request;
state->cancel_context = request;
```

Cancellation attempts the state transition:

```text
PENDING -> CANCELLED
```

If cancellation wins:

- waiters wake with `ERROR_CANCELLED`;
- the runtime attempts to cancel the kernel operation;
- late completions are discarded using the request ID/generation;
- operation resources are released when the kernel confirms or completes.

If normal completion wins first, cancellation has no effect on the result.

### 27.4 Coroutine cancellation

Coroutine cancellation should initially be cooperative. Cancellation sets a flag that is observed at:

- future awaits;
- timer waits;
- explicit cancellation points;
- preemption safe points;
- runtime calls.

Asynchronously destroying a running coroutine is unsafe because it may hold locks or own partially initialized resources.

---

## 28. Joining Coroutines

### 28.1 Join as a future

Each joinable coroutine owns an internal completion promise and exposes a join future.

```c
typedef struct coroutine {
    ...
    promise_t join_promise;
    future_t join_future;
} coroutine_t;
```

On exit:

```c
promise_resolve(&coro->join_promise, completion_value);
```

### 28.2 Join API

```c
int coroutine_join(
    coroutine_t *coro,
    void **result);
```

Internally:

```c
return future_await(&coro->join_future, ...);
```

### 28.3 Detached coroutines

A detached coroutine has no required joiner and may be reaped after:

- it reaches `CORO_DEAD`;
- its scheduler reference is released;
- all external handles are released.

---

## 29. Channels and Streams

Futures are single-completion objects. Channels represent repeated communication.

```c
typedef struct channel channel_t;

future_t channel_send_async(channel_t *channel, value_t value);
future_t channel_receive_async(channel_t *channel);
```

A channel may internally match senders and receivers directly or buffer values.

Repeated microkernel endpoint messages are better represented as a stream or channel than as one shared future.

---

## 30. Synchronization Primitives

Coroutine-aware synchronization must suspend coroutines rather than worker threads.

Possible primitives include:

- coroutine mutex;
- semaphore;
- condition variable;
- event;
- barrier;
- read-write lock.

They can use the same waiter abstraction as futures.

Example mutex slow path:

```text
mutex unavailable
    ↓
register coroutine waiter
    ↓
CORO_RUNNING -> CORO_WAIT_FUTURE-like state
    ↓
unlock wakes one waiter
    ↓
CORO_RUNNABLE
```

Kernel-backed blocking primitives remain necessary for worker parking and interprocess synchronization.

---

## 31. Memory Management and Lifetime

### 31.1 Future state references

References may be held by:

- future handles;
- promise handles;
- request tables;
- timer entries;
- registered wait operations;
- continuation chains.

The shared state is freed when the reference count reaches zero.

### 31.2 Promise destruction

Destroying the last promise while the future is pending should reject the future with a broken-promise error:

```text
ERROR_BROKEN_PROMISE
```

This prevents waiters from sleeping forever when the producer disappears.

### 31.3 Waiter lifetime

A waiter must remain valid until one of these is true:

- it has been detached by completion;
- it has been removed by timeout or cancellation;
- it has been marked inactive and the future implementation guarantees it will no longer dereference the waiter.

Stack-allocated coroutine waiters are valid while the coroutine stack exists, but must be unregistered before the coroutine returns from `await`.

### 31.4 Coroutine lifetime

A coroutine structure and stack remain allocated while:

- it is runnable or running;
- it is registered as a waiter;
- a join handle exists;
- diagnostic or tracing references exist.

---

## 32. Memory Ordering

The initial implementation should prefer locks for complex object transitions. Atomics are still required for visible state and ownership.

Recommended ordering:

- publishing a terminal future state: `memory_order_release`;
- reading terminal state without lock: `memory_order_acquire`;
- publishing runnable coroutine state before queue insertion: release;
- consuming queue entries before execution: acquire;
- coroutine ownership claim: acquire or acquire-release;
- worker sleep publication: release;
- worker sleep observation before wake: acquire.

Locks provide acquire/release synchronization for protected fields.

Do not rely on `volatile` for inter-CPU synchronization.

---

## 33. Scheduler Invariants

The runtime shall continuously preserve these invariants:

1. A coroutine executes on at most one worker.
2. A worker executes at most one coroutine.
3. A runnable coroutine appears in exactly one run queue.
4. A running coroutine appears in no run queue.
5. A waiting coroutine appears in no run queue.
6. A waiter is woken at most once.
7. A future transitions from pending to exactly one terminal state.
8. Promise completion never schedules waiters while holding the future lock.
9. A coroutine stack is not freed while executing or registered as a waiter.
10. A worker never sleeps without an atomic recheck of work availability.
11. Kernel preemption does not modify coroutine scheduler state.
12. Runtime preemption-disable sections do not disable kernel scheduling.

Debug builds should assert these invariants aggressively.

---

## 34. Error Handling

Suggested generic errors:

```c
enum {
    ERROR_OK = 0,
    ERROR_NO_MEMORY,
    ERROR_INVALID_ARGUMENT,
    ERROR_INVALID_STATE,
    ERROR_TIMEOUT,
    ERROR_CANCELLED,
    ERROR_BROKEN_PROMISE,
    ERROR_ALREADY_COMPLETED,
    ERROR_COROUTINE_DEAD,
    ERROR_IPC_FAILED,
    ERROR_STACK_OVERFLOW,
};
```

Promise completion functions return `false` when the state is already terminal. Public APIs may map this to `ERROR_ALREADY_COMPLETED` where useful.

---

## 35. Observability and Diagnostics

### 35.1 Coroutine metadata

Track at least:

- unique coroutine ID;
- creation timestamp;
- current state;
- last worker;
- current wait reason;
- future/request ID;
- stack bounds;
- optional name;
- accumulated runtime;
- number of yields and migrations.

### 35.2 Runtime metrics

Expose:

- runnable coroutine count;
- waiting coroutine count by reason;
- queue length per worker;
- steals attempted and completed;
- worker park and wake counts;
- context-switch count;
- future completion count;
- stale/duplicate completion count;
- cancellation count;
- average wait duration;
- timer lateness;
- stack high-water mark where measurable.

### 35.3 Debug dump

A runtime dump should show:

```text
worker 0: running coro 42, local queue 3, sleeping false
worker 1: idle, local queue 0, sleeping true

coro 42: RUNNING, stack 31/64 KiB, worker 0
coro 51: WAIT_IPC, request 0x109a
coro 52: WAIT_TIMER, deadline 81234567
```

---

## 36. Security and Isolation

The kernel must not trust runtime-provided tokens as pointers.

Recommended properties:

- completion tokens are opaque 64-bit values;
- request IDs include generation protection;
- kernel validates user buffers and lengths;
- runtime validates event type and request ownership;
- stale completions cannot access freed future state;
- stack guard pages are non-present;
- executable stacks are prohibited unless explicitly required;
- future result destructors run in trusted runtime context.

Cross-process promises are not part of the generic runtime. IPC transports serialized results; the receiving process resolves its own local promise.

---

## 37. Suggested Kernel Interface

> **Verified 2026-07-18:** Mapping of the idealized names below to what the kernel
> actually exposes (see [§48.1](#481-primitive-mapping)):
>
> | Idealized (below)                 | Actual WASMOS facility                                                                     | Gap                                                                                                      |
> |-----------------------------------|--------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
> | `sys_thread_yield`                | `WASMOS_SYSCALL_YIELD`(3)/`THREAD_YIELD`(8); wasm hostcalls `sched_yield`/`thread_yield`   | none                                                                                                     |
> | `sys_wait_u32`/`sys_wake_u32`     | `futex_wait`/`futex_wake` (hostcall ids 16/17), keyed by physical address                  | **Not declared in libc `api.h`; absent from the native `int 0x80` syscall path** — must be surfaced      |
> | `sys_ipc_submit`/`sys_ipc_cancel` | *none* — IPC is synchronous `ipc_send` + `ipc_select_*`; replies correlate by `request_id` | build IPC futures in user space ([§23](#23-ipc-integration)); no submit/completion/cancel syscalls exist |
> | `sys_wait_events` (unified)       | `ipc_select_wait` / `ipc_select_wait_timeout` (≤ `IPC_SELECT_EPS_MAX = 8` endpoints)       | no single unified event syscall; timers/proc-death/IRQ already arrive as endpoint messages/notifications |
> | `sys_clock_monotonic`             | `sched_ticks` hostcall → `timer_ticks()`                                                   | coarse (~250 Hz / 4 ms), 32-bit (wraps ~198 days), **no vDSO time page**                                 |
> | `sys_arm_thread_timer`            | *none* — use `ipc_select_wait_timeout(ms)` or `futex_wait(…, timeout_ms)`                  | ms granularity, tick-quantized                                                                           |
>
> The `futex` gap and the missing native-side wait/wake are the two concrete
> kernel/libc changes this runtime needs before worker parking ([§14](#14-worker-parking-and-lost-wakeups)) can be implemented as written.

The minimal useful interface is:

```c
void sys_thread_yield(void);

int sys_wait_u32(
    _Atomic uint32_t *address,
    uint32_t expected,
    uint64_t absolute_deadline);

int sys_wake_u32(
    _Atomic uint32_t *address,
    uint32_t count);

int sys_ipc_submit(
    endpoint_t endpoint,
    const ipc_request_t *request,
    uint64_t completion_token);

int sys_ipc_cancel(uint64_t completion_token);

int sys_wait_events(
    event_t *events,
    size_t capacity,
    uint64_t absolute_deadline);

uint64_t sys_clock_monotonic(void);
```

A vDSO-like read-only shared time page is preferred for frequent safe-point deadline checks.

Optional additions:

```c
int sys_arm_thread_timer(
    uint64_t deadline,
    uint64_t event_token);

int sys_signal_event(event_handle_t event);
```

---

## 38. Public Runtime API Sketch

```c
/* Runtime */
int coroutine_runtime_init(const runtime_options_t *options);
void coroutine_runtime_shutdown(void);

/* Coroutines */
int coroutine_spawn(
    coroutine_t **out,
    void (*entry)(void *),
    void *arg,
    const coroutine_options_t *options);

coroutine_t *coroutine_current(void);
void coroutine_yield(void);
_Noreturn void coroutine_exit(void *result);
int coroutine_join(coroutine_t *coro, void **result);
int coroutine_detach(coroutine_t *coro);
int coroutine_cancel(coroutine_t *coro);

/* Futures and promises */
int future_create(future_t *future, promise_t *promise);
void future_retain(future_t *future);
void future_release(future_t *future);
void promise_retain(promise_t *promise);
void promise_release(promise_t *promise);

bool future_is_ready(const future_t *future);
int future_poll(const future_t *future, future_value_t *value);
int future_await(future_t *future, future_value_t *value);
int future_await_until(
    future_t *future,
    uint64_t deadline,
    future_value_t *value);

bool promise_resolve(promise_t *promise, future_value_t value);
bool promise_reject(promise_t *promise, int error);
bool promise_cancel(promise_t *promise, int reason);

/* Timers */
timer_future_t timer_at(uint64_t deadline);
timer_future_t timer_after(uint64_t duration);

/* IPC */
ipc_future_t ipc_call_async(
    endpoint_t endpoint,
    const ipc_request_t *request);

/* Cancellation */
void cancellation_token_init(cancellation_token_t *token);
void cancellation_request(cancellation_token_t *token);
bool cancellation_is_requested(const cancellation_token_t *token);
```

---

## 39. Example Flow

### 39.1 Application code

```c
static void service_request(void *arg)
{
    request_t *request = arg;

    ipc_future_t query = ipc_call_async(
        DATABASE_ENDPOINT,
        build_database_request(request));

    ipc_result_t response;
    int error = ipc_future_await_until(
        &query,
        runtime_now() + seconds(5),
        &response);

    if (error == ERROR_TIMEOUT) {
        send_timeout_response(request);
        return;
    }

    if (error != 0) {
        send_error_response(request, error);
        return;
    }

    send_success_response(request, &response);
}
```

### 39.2 Runtime sequence

```text
worker 0 executes coroutine A
    ↓
A submits async IPC request 72
    ↓
runtime stores request 72 -> promise
    ↓
A awaits the future
    ↓
future is pending
    ↓
A registers as waiter and becomes WAIT_FUTURE
    ↓
worker 0 executes coroutine B
    ↓
kernel reports completion for request 72
    ↓
runtime removes request 72 from request table
    ↓
promise is resolved
    ↓
A transitions WAIT_FUTURE -> RUNNABLE
    ↓
A is enqueued, possibly on worker 1
    ↓
worker 1 resumes A after future_await()
```

---

## 40. Structured Concurrency

A later high-level API should group child coroutines under a parent scope.

```c
task_group_t group;
task_group_init(&group);

task_group_spawn(&group, fetch_a, arg_a);
task_group_spawn(&group, fetch_b, arg_b);

int error = task_group_join(&group);
task_group_destroy(&group);
```

A task group ensures that children are joined or cancelled before parent-owned resources leave scope.

Detached coroutines should be reserved for process-lifetime services or explicitly supervised tasks.

---

## 41. Priority and Fairness

The initial scheduler may use equal-priority FIFO behavior within each worker queue.

Later extensions may add:

- coroutine priorities;
- deadline scheduling;
- latency-sensitive queues;
- priority donation for coroutine mutexes;
- worker affinity classes.

Priority must be considered across both scheduler layers. A high-priority coroutine cannot run if all of its workers are low-priority kernel threads.

The first implementation should avoid exposing coroutine priorities until kernel-thread priority semantics are stable.

---

## 42. Failure Modes

### 42.1 Double scheduling

**Symptom:** One coroutine executes on two CPUs.

**Prevention:** Queue uniqueness, owner CAS, state CAS, assertions.

### 42.2 Lost future wakeup

**Symptom:** A completed future has a permanently sleeping waiter.

**Prevention:** Serialize readiness check and registration under the future lock.

### 42.3 Lost worker wakeup

**Symptom:** Runnable work exists while all workers sleep.

**Prevention:** Atomic wait-if-equal kernel primitive and queue recheck after publishing sleep state.

### 42.4 Use-after-free from late IPC completion

**Symptom:** Completion resolves a recycled future or promise.

**Prevention:** Unique request IDs, generation counters, request-table-held references.

### 42.5 Stack freed while waiting

**Symptom:** Completion accesses a waiter stored on a freed stack.

**Prevention:** Waiting coroutine retains itself; waiter is unregistered before stack reclamation.

### 42.6 Blocking syscall on worker

**Symptom:** Unrelated coroutines stall.

**Prevention:** Runtime wrappers use asynchronous IPC and event waits. Mark unsafe blocking syscalls explicitly.

### 42.7 Preemption inside runtime critical section

**Symptom:** Queue or future state corruption.

**Prevention:** Safe-point-only preemption and runtime preemption-disable nesting.

---

## 43. Testing Strategy

### 43.1 Context switching

Test:

- register preservation;
- stack alignment;
- nested calls across yields;
- large local variables;
- coroutine exit;
- guard-page overflow.

### 43.2 Scheduler tests

Test:

- one worker, many coroutines;
- many workers, one coroutine;
- many workers, many coroutines;
- repeated migration;
- pinned coroutines;
- stealing from empty and non-empty queues;
- worker park/wake races;
- shutdown with sleeping workers.

### 43.3 Future tests

Test all races between:

- await and resolve;
- await and reject;
- await and cancel;
- timeout and resolve;
- cancellation and resolve;
- final promise release and await;
- multiple waiters and one completion;
- duplicate completion attempts.

Each race test should run for many iterations under SMP.

### 43.4 IPC tests

Test:

- immediate submission failure;
- normal completion;
- completion before await;
- completion during waiter registration;
- cancellation before completion;
- late completion after cancellation;
- duplicate completion;
- service death;
- process shutdown with requests pending.

### 43.5 Stress and fault injection

Inject:

- forced yields at runtime critical boundaries;
- random worker migration;
- delayed wake syscalls;
- duplicate and reordered completion events;
- allocation failures;
- timer lateness;
- worker termination;
- high request ID reuse pressure.

---

## 44. Implementation Phases

### Phase 1: Cooperative single-worker runtime

Implement:

- stack allocation;
- context initialization;
- context switch;
- spawn;
- yield;
- exit;
- join;
- detached reaping.

### Phase 2: Futures and promises

Implement:

- shared future state;
- lock-protected waiter registration;
- resolve, reject, and broken promise;
- coroutine-aware await;
- typed wrappers.

### Phase 3: Timers

Implement:

- monotonic clock access;
- per-worker timer heap;
- timer futures;
- sleep;
- await-with-timeout.

### Phase 4: SMP scheduling

Implement:

- one worker per CPU;
- per-worker queues;
- remote enqueue;
- work stealing;
- worker parking and waking;
- ownership and state assertions.

### Phase 5: Asynchronous IPC

Implement:

- IPC submission syscall;
- completion tokens;
- request table;
- event wait syscall;
- IPC futures;
- cancellation and stale-completion handling.

### Phase 6: Safe-point preemption

Implement:

- worker slice deadlines;
- cheap monotonic clock reads;
- WARP/wasm3 safe-point insertion;
- preemption-disable counters;
- scheduler fairness metrics.

### Phase 7: Advanced features

Consider:

- general future select;
- channels and streams;
- structured concurrency;
- priority scheduling;
- lock-free work-stealing deques;
- dynamic worker allocation;
- hard asynchronous coroutine preemption.

---

## 45. Recommended Initial Decisions

For the first production-capable version:

| Area                 | Decision                                      |
|----------------------|-----------------------------------------------|
| Coroutine type       | Stackful                                      |
| Scheduler            | M:N                                           |
| Workers              | One per available CPU                         |
| Run queues           | Per-worker, lock-protected deque              |
| Load balancing       | Work stealing                                 |
| Coroutine preemption | Cooperative safe points with timer deadline   |
| Kernel preemption    | Existing timer-based kernel scheduling        |
| IPC                  | Asynchronous submission and completion events |
| Wait API             | Kernel wait-if-equal plus event wait          |
| Future waiters       | Lock-protected linked list                    |
| Timer structure      | Per-worker binary min-heap                    |
| Cancellation         | Cooperative, state-transition based           |
| Join                 | Internal future/promise                       |
| Result API           | Generic core plus typed wrappers              |
| Stack growth         | Fixed stack plus guard page                   |
| Hard preemption      | Deferred                                      |

---

## 46. Open Questions

The following decisions depend on the final WASMOS kernel ABI:

1. Can one event wait consume IPC completions, timers, process-death events, and runtime wake events?
2. Can asynchronous IPC safely pin or copy user request buffers?
3. Is an IPC cancellation syscall available, and what are its completion guarantees?
4. Can a completion be delivered to any worker, or is it bound to the submitting thread?
5. Is a vDSO-like monotonic clock page available?
6. Does the process receive a fixed CPU allocation or dynamically varying virtual processors?
7. Can workers share one address space while retaining distinct kernel scheduling identities?
8. Are WASM executions permitted to migrate between workers without rebuilding CPU-local JIT state?
9. Does TLS identify workers, coroutines, or both?
10. Are runtime stacks mapped by a user-space pager, and can that pager deadlock on worker exhaustion?

---

## 47. Final Design Principle

The coroutine scheduler and future subsystem meet at one state transition:

```text
future_await():
    CORO_RUNNING -> CORO_WAIT_FUTURE

promise completion:
    CORO_WAIT_FUTURE -> CORO_RUNNABLE
```

The future records what the coroutine is waiting for. The promise represents the authority to complete that wait. The coroutine scheduler decides where and when the resumed coroutine runs.

The kernel remains responsible for CPUs, kernel threads, timer interrupts, IPC transport, and event delivery. The runtime remains responsible for user-space execution contexts, coroutine queues, asynchronous composition, and coroutine-level fairness.

This separation provides a generic foundation for IPC, timers, process waits, device I/O, channels, joins, and structured concurrency without teaching the kernel about individual coroutines.

---

## 48. Implementation Reality Baseline (verified 2026-07-18)

This section reconciles sections 1–47 with the WASMOS implementation as it exists
today. Authoritative sources: `src/kernel/include/{thread,process,sched,sched_event,futex,ipc}.h`,
`src/kernel/{process,ipc,futex,sched_event,timer}.c`, `src/kernel/syscall.c`,
`src/kernel/include/warp_ring3.h` (hostcall enum), `src/kernel/{wasm3/link.c,warp/link.cpp}`
(hostcall tables), `src/libc/include/wasmos/{api,thread,thread_x86_64,ipc}.h`,
`src/drivers/include/wasmos_native_driver.h`, and the sibling architecture docs
[`07-scheduling-and-preemption.md`](07-scheduling-and-preemption.md),
[`08-threading-and-lifecycle.md`](08-threading-and-lifecycle.md),
[`09-process-and-ipc.md`](09-process-and-ipc.md), and
[`29-threadable-scheduler.md`](29-threadable-scheduler.md).

### 48.1 Primitive mapping

| Design assumption (§)                                                 | What actually exists                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Verdict                                                                                                                                                                                                                                                                                                                                               |
|-----------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Kernel schedules **threads**, not coroutines (§1, §5)                 | `thread_t` is the schedulable unit (`thread.h:31`); per-CPU ready queues `cpu_sched_t` with 7 priority FIFOs; work-stealing `cpu_sched_try_steal` (`sched_thread.c:390`); up to `WASMOS_MAX_CPUS = 16`                                                                                                                                                                                                                                                                                                          | ✅ **Matches.** The two-level split the design relies on is the real kernel model.                                                                                                                                                                                                                                                                     |
| **N worker threads per process** (§6)                                 | Multiple kernel threads per process are supported: `process_t.thread_count`/`live_thread_count`; `process_thread_spawn_worker_internal` (`process.c:1396`) and `process_thread_spawn_user_internal` (`process.c:1438`). Native ring-3 uses `THREAD_CREATE` (syscall 10). WASM `thread_create` hostcall spawns a VM thread bound to a **named export** (`wasm3/link.c:3111`, `warp/link.cpp:1798`).                                                                                                              | ✅ **Substrate exists** for both native and WASM.                                                                                                                                                                                                                                                                                                      |
| **Parallel** coroutines across CPUs (§6.1)                            | Threads are globally scheduled and stolen across all CPUs — **but** any process with `needs_runtime_lock` (the built-in wasm3/WARP runtimes) serializes runtime execution under a per-process `runtime_lock` (`process.c:539–550`). Native processes do not take it.                                                                                                                                                                                                                                            | ⚠️ **Split.** Native = true parallelism. WASM = **concurrency only** (one runtime thread runs at a time); coroutines interleave on a single effective worker.                                                                                                                                                                                         |
| `sys_wait_u32` / `sys_wake_u32` for worker parking (§14, §37)         | `futex_wait(uaddr, expected, timeout_ms, ctx)` / `futex_wake(uaddr, count, ctx)` (`futex.c`), hostcall ids 16/17, keyed by **physical** address (cross-process via `shmem_grant`), built on `sched_event_t`.                                                                                                                                                                                                                                                                                                    | ⚠️ **Exists but not surfaced.** No `wasmos_futex_*` in libc `api.h`; **absent from the native `int 0x80` syscall path** (`syscall.c` has no futex). Must be wired up before §14 works. `mutex.h:50` even carries a TODO to consume it.                                                                                                                |
| `sys_thread_yield` (§12.2, §37)                                       | `WASMOS_SYSCALL_YIELD`(3) / `THREAD_YIELD`(8); hostcalls `sched_yield`(13) / `thread_yield`(71).                                                                                                                                                                                                                                                                                                                                                                                                                | ✅ **Matches.**                                                                                                                                                                                                                                                                                                                                        |
| **Asynchronous IPC** `sys_ipc_submit` + completion tokens (§23, §37)  | **Does not exist.** The synchronous request/response *pattern* (`wasmos_ipc_call`, native `IPC_CALL`, nested `ipc_select_one` reply-waits) **is being removed.** The non-blocking transport stays: `ipc_send` enqueues + `sched_event_wake_one`; the reply is a separate message correlated by `request_id`, drained by the one per-endpoint pump. The existing `wasmos_sys_event_loop` already tracks outgoing requests as **intents** (`request_id → on_resolve` continuation) — a hand-rolled promise table. | ❌ **Biggest correction.** The future/promise + single event-loop model **replaces** synchronous IPC; **nothing is layered on top of it.** Build the request-table directly on the non-blocking transport (`ipc_send` + one per-endpoint pump). No submit/completion/cancel syscall is needed; stale replies are discarded by `request_id`/generation. |
| Unified `sys_wait_events` (§24)                                       | `ipc_select_wait` / `ipc_select_wait_timeout` block on up to `IPC_SELECT_EPS_MAX = 8` endpoints; timers, process death, and IRQs are already delivered as messages/notifications to endpoints.                                                                                                                                                                                                                                                                                                                  | ⚠️ **Maps to select-sets**, not a new syscall. Note the 8-endpoint cap and that a worker's "wake doorbell" is naturally a `NOTIFICATION` endpoint or a futex word.                                                                                                                                                                                    |
| `sys_clock_monotonic`, vDSO time page (§15, §25, §37)                 | `sched_ticks` hostcall → `timer_ticks()`; default **250 Hz (4 ms/tick)**, returned as truncated `int32_t` (wraps ~198 days). No ns clock, **no vDSO page**. Timeouts are expressed in **ms** and tick-quantized.                                                                                                                                                                                                                                                                                                | ⚠️ **Coarse.** Safe-point deadline checks (§15.2) pay a real hostcall, not a memory read; fairness/timer granularity is 4 ms. A vDSO time page is a genuine future optimization, not a current facility.                                                                                                                                              |
| Stackful `coroutine_switch` swaps `RSP` between coroutine stacks (§9) | Kernel has `context_switch.S` for **thread** switching. There is **no** user-space coroutine/fiber/`ucontext` primitive anywhere in `src/` (only WARP's `setjmp/longjmp` trap unwinding). A raw `mov rsp` stack swap is valid for **native ring-3 code** on its own native stack.                                                                                                                                                                                                                               | ⚠️ **Native only.** A wasm3 guest runs on an interpreter-managed value/call stack; a WARP guest runs JIT/ring-3 code the runtime (in `libs/warp`, which must not be modified) controls. Neither guest stack can be swapped by a native `coroutine_switch`. See [§49](#49-wrapper-architecture-one-contract-native-and-wasm-cores).                    |
| Guard-page / reserve-then-commit stacks (§8)                          | MM supports committed regions + guard pages; reserved-VA linear-memory slots (commit-on-demand, 2 GiB reservation) already exist for both runtimes. Per-thread kernel stacks are allocated per thread.                                                                                                                                                                                                                                                                                                          | ✅ **Feasible** for native coroutine stacks; the 1 MiB-reserve / 64 KiB-commit policy is realistic.                                                                                                                                                                                                                                                    |
| Generic waiter abstraction (§17)                                      | No existing implementation, but the `wasmos_sys_event_loop` intent/handler tables are the de-facto continuation-waiter today.                                                                                                                                                                                                                                                                                                                                                                                   | ✅ **Sound.** Keep it; the intent record *is* the `continuation_waiter` of §17.3.                                                                                                                                                                                                                                                                      |
| Join via internal future (§28)                                        | Kernel `THREAD_JOIN`(11)/`WAIT`(4) block on `sched_event_t` and wake on exit. A *coroutine* join future is a user-space construct layered above.                                                                                                                                                                                                                                                                                                                                                                | ✅ **Compatible.**                                                                                                                                                                                                                                                                                                                                     |

### 48.2 Required adjustments

1. **The future/promise + single event-loop model *replaces* synchronous IPC — it
   is not layered on it.** The synchronous request/response *pattern*
   (`wasmos_ipc_call`, the native `IPC_CALL` syscall, nested `ipc_select_one`
   reply-waits) is being **removed**, and **nothing new is added on top of it.** A
   request returns a future; the runtime records `request_id → promise`; the single
   per-endpoint receive pump resolves the promise when the correlated reply arrives
   — never parking in a nested receive. This is the existing `intent` mechanism
   promoted to first-class promises, built directly on the surviving non-blocking
   transport (`ipc_send` + one receiver per endpoint). No
   `sys_ipc_submit`/`sys_ipc_cancel`/completion-token syscalls are needed. This is
   the documented cure for the real `fs-manager ↔ device-manager` boot deadlock
   ([`09-process-and-ipc.md`](09-process-and-ipc.md)).

2. **Split the coroutine substrate by execution model.** The native stackful core
   (§7–§15) is implementable as written for ring-3 native services/drivers. WASM
   guests need a different mechanism (single-worker cooperative fibers or
   source-language stackless async) — they cannot share `coroutine_switch`. See
   [§49](#49-wrapper-architecture-one-contract-native-and-wasm-cores). This is the
   central consequence for "coroutines available to both native and WASM."

3. **Treat WASM concurrency as M:1, not M:N, until the runtime lock is addressed.**
   `runtime_lock` serializes all runtime execution within a wasm3/WARP process, so
   the "one worker per CPU" policy (§6.2) yields no parallelism for WASM. Coroutines
   still deliver useful concurrency (hiding IPC/timer latency) on one worker. True
   WASM parallelism requires per-instance-reentrant runtimes or one process per
   parallel lane — out of scope here, tracked separately.

4. **Surface `futex` before implementing worker parking (§14).** Add
   `wasmos_futex_wait`/`wasmos_futex_wake` to libc `api.h` (they already exist as
   `"wasmos"` imports) and add a native `int 0x80` path for them, or park native
   workers on an IPC/notification endpoint instead. As written, §14's `sys_wait_u32`
   loop has no user-space entry point today.

5. **Re-express all timing in ms/ticks (§15, §25, §26).** Deadlines are
   `sched_ticks()`-based and 4 ms-quantized; there is no ns clock and no vDSO page.
   Safe-point checks (§15.2) cost a hostcall — budget accordingly, or gate
   preemption checks behind a loop-iteration counter rather than a per-iteration
   clock read.

6. **Worker count comes from `wasmos_sched_cpu_count()`, and the process holds no CPU
   reservation (§6.2).** Threads are globally scheduled with stealing; there is no
   per-process virtual-CPU allocation. `worker_count = min(sched_cpu_count(),
   configured_limit)` is a hint, not a guaranteed parallelism width.

### 48.3 Answers to the §46 open questions (as of 2026-07-18)

1. **One event wait for IPC + timers + proc-death + wake?** Partially — `ipc_select_wait[_timeout]` covers up to 8 endpoints, and timer/proc-death/IRQ already arrive as endpoint traffic, so a select-set *is* the unified wait; there is no single `sys_wait_events` beyond it.
2. **Async IPC buffer pinning/copy?** N/A today — IPC copies a fixed 16-byte `ipc_message_t`; bulk data moves through the borrow/xfer-buffer model, not the async path.
3. **IPC cancellation syscall?** No. Cancellation is user-space `request_id`/generation discard of late replies.
4. **Completion bound to submitter or any worker?** The reply is a message on the caller's reply endpoint; **any thread of that context that owns/pumps the endpoint** can consume it.
5. **vDSO monotonic clock page?** No — only the `sched_ticks` hostcall (4 ms granularity).
6. **Fixed CPU allocation vs dynamic?** Dynamic. Global scheduling with work-stealing across up to 16 CPUs; **no per-process reservation**.
7. **Workers share one address space with distinct kernel identities?** Yes — threads share `context_id`/address space and each has its own `tid` and saved `thread->ctx`.
8. **WASM migration between workers rebuilding CPU-local JIT state?** Constrained. WARP/wasm3 execution is serialized by `runtime_lock` and carries per-thread ring-3 return state; migration is a kernel scheduler decision, but WASM parallel migration across workers is not a supported concurrency model yet.
9. **Does TLS identify workers or coroutines?** The kernel identifies the current thread/CPU via `GS`-based per-CPU data (`cpu_local()`); coroutine identity is a user-space runtime concept with no TLS backing today.
10. **User-space pager deadlock on worker exhaustion?** Not a current risk — thread kernel stacks and committed regions are kernel-mapped; there is no user-space pager in the fault path that could deadlock on worker starvation.

---

## 49. Wrapper Architecture: One Contract, Native and WASM Cores

This section answers the driving requirement: **coroutines/futures must be usable
from native apps/drivers/services *and* from WASM apps, so a single core needs
multiple wrappers.** Verification shows the honest shape is **one shared
*future/promise* contract over two distinct *coroutine* cores**, wrapped across the
two build axes WASMOS already maintains.

### 49.1 The two axes that already exist

Every user-space facility in WASMOS is duplicated along two axes, and any
coroutine/future API must follow the same layout:

- **Axis A — execution model:** `src/libsys/wasm` (header-only `static inline` C
  helpers that call `"wasmos"` hostcalls) vs `src/libsys/native`
  (`libsys_native.c` C exports + `libsys.zig`/`c_abi.zig` Zig wrappers driving the
  `wasmos_driver_api_t` vtable, ABI magic `'WNAP'`, version 8).
- **Axis B — language shim:** `src/libc/include/wasmos/*.h` + `src/libc/src/*.c`
  (C), and one hand-maintained shim per guest language:
  `src/libc/rust/wasmos.rs`, `src/libc/go/wasmos.go`, `src/libc/zig/wasmos.zig`,
  `src/libc/assemblyscript/wasmos.ts`. AGENTS.md mandates these stay in sync.

### 49.2 Layering: shared contract, divergent core

```text
L2  Coroutine scheduler          NATIVE stackful core        WASM per-language facility
    (execution contexts)         (§7–§15 as written)         (fibers or stackless async)
                                        │                              │
                                        └──────────────┬───────────────┘
L1  Future / Promise core  ── single CONTRACT (§16–§22, §27–§30), materialized per shim ──
    (waiter-agnostic; intents = continuation waiters)
                                        │
L0  Kernel primitives  ── threads · futex · sched_event · ipc_send · ipc_select[_timeout] · sched_ticks ──
    (shared by everyone; §48.1)
```

- **L0 (kernel):** already shared and identical for native and WASM (native via
  `int 0x80` / `wasmos_driver_api_t`; WASM via `"wasmos"` hostcalls).

- **L1 — Future/Promise (single contract, replicated code):** the state machine of
  §16–§22 (`PENDING → READY/FAILED/CANCELLED`, lock-protected waiter list, resolve
  outside the lock, generic `future_waiter` with a `wake_fn`) is **pure logic over
  L0** — no stack switching. It can and should be **one design** wrapped in every
  shim, exactly as the `wasmos_sys_event_loop` intent/handler tables already are on
  both the wasm and native sides. Build it by promoting today's `intent`
  (`request_id → on_resolve`) into a first-class `promise`, and today's `handler`
  table into the incoming-message dispatch. Because IPC replies are just messages,
  L1 is portable with zero native-vs-WASM difference beyond the shim syntax.

- **L2 — Coroutine scheduler (the part that genuinely diverges):**

  - **Native core (single stackful implementation).** For ring-3 native
    services/drivers this is the design of §7–§15 verbatim: per-coroutine native
    stacks with a guard page, an asm `coroutine_switch` (callee-saved GPRs + `RSP`),
    per-worker run queues, and `THREAD_CREATE` workers parked on `futex`/endpoint.
    This is the "single coroutines implementation" the requirement asks for — it
    lives once in `src/libsys/native` (C, with the asm switch) and is wrapped for
    the native languages (C directly; Zig via `libsys.zig`). Native processes take
    no `runtime_lock`, so this is **true M:N**.

  - **WASM facility (cannot reuse the native core).** A wasm3 or WARP guest cannot
    have its execution stack swapped by native `coroutine_switch`, and `libs/warp`
    must not be modified. Two viable wrappers, selectable per language:
    1. **Single-worker cooperative fibers.** A "coroutine" is a WASM VM thread
       (`wasmos_thread_create` on a named export) that parks on a `futex` word or a
       notification endpoint; `coroutine_yield`/`await` returns control to the
       runtime's L1 event pump. Serialized by `runtime_lock` → **M:1 concurrency**,
       which is sufficient to overlap IPC/timer latency. Reuses L1 unchanged. This
       is the recommended first WASM target because it needs no toolchain support.
    2. **Source-language stackless async (the `continuation_waiter` of §17.3).**
       Rust `async`/`.await`, Zig async, Go/TinyGo goroutines, AssemblyScript
       async — the compiler provides suspension; L1 provides the awaitable
       `future`. Each language shim adapts its native async runtime's waker to
       `future_waiter.wake`. This is where the per-language wrappers do real,
       non-identical work.

  So coroutines are delivered to **both** substrates, but L2 is **two
  implementations sharing one L1 contract**, not one implementation with thin
  wrappers. The "single core, multiple wrappers" goal holds fully at L1 and holds
  for the *native* L2 (one core, C+Zig wrappers); the *WASM* L2 is necessarily a
  family of language-specific facilities over the shared future core.

### 49.3 Concrete placement

| Layer                      | Native (`src/libsys/native`, `src/libc` native build)                                                                         | WASM (`src/libsys/wasm`, `src/libc/{c,rust,go,zig,assemblyscript}`)                                                                 |
|----------------------------|-------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|
| L1 future/promise contract | `libsys_native.c` (C) + `libsys.zig` (Zig), evolving today's `wasmos_sys_native_event_loop_t` intents into `promise`/`future` | header-only C in `src/libsys/wasm/include/wasmos/` + per-language reimpl in each `wasmos.<ext>`, evolving `wasmos_sys_event_loop_t` |
| L2 coroutine scheduler     | **one** stackful core: `coroutine_switch.S` + run queues + `THREAD_CREATE` workers, parked on futex/endpoint                  | per-language: cooperative fibers over `wasmos_thread_create`+futex, and/or the language's stackless async bound to `future_waiter`  |
| Kernel gaps to close first | add native `int 0x80` `futex_wait`/`futex_wake` (or park on endpoints)                                                        | add `wasmos_futex_wait`/`wasmos_futex_wake` to `api.h` (imports already exist)                                                      |

### 49.4 Recommended sequencing (supersedes §44 phase boundaries where they conflict)

1. **Native single-worker core first.** Implement the caller-owned native
   stackful runtime and its local future/promise state in `src/libsys/native`;
   this is the implemented first slice described in §50.
2. **C WASM L1/L2 baseline.** The C guest runtime in `src/libsys/wasm` now
   provides the contract plus explicit stackless resumable tasks. It removes
   the need for a WASM stackful coroutine core.
3. **Language shims.** Generalize the existing intent/handler event loop into
   that contract for Zig, Rust, Go, and AssemblyScript, then expose each
   language's ergonomic async/state-machine surface.
4. Defer WASM parallelism (the `runtime_lock` question) and hard preemption (§15.5)
   as originally planned.

## 50. Native Single-Worker Baseline (implemented)

`src/libsys/native/{coroutine_native.c,coroutine_native_x86_64.S}` implements
the x86-64 target slice; `coroutine_native_aarch64.S` adds the matching AAPCS64
host backend for native ARM64 validation. Both use caller-owned coroutine
records and stacks plus one cooperative ready queue. No kernel worker is
created: the runtime runs on the calling native thread.

The initial future/promise core is intentionally local and single-worker:

- a future is `PENDING`, `READY`, or `FAILED`;
- `await` parks the current coroutine in the future's waiter list;
- resolve/reject settles exactly once and makes all waiters runnable;
- caller-owned `future_then` registrations provide separate success/error
  callbacks and return a child future held by the continuation record; a
  callback resolves that child with its output value or rejects it by returning
  a negative status, while a missing callback forwards the parent outcome;
  callbacks are queued by `runtime_run()` and never invoked inline by
  registration or settlement;
- every coroutine exposes its exit result as a join future.

`wasmos_async_start()` is the native C async-function boundary: it starts a
caller-owned coroutine and returns that coroutine's completion future. The
worker body remains an ordinary C function and can use `await` and `yield`.

The same caller-storage model supports `wasmos_future_race()` and
`wasmos_future_all()`, with `WASMOS_FUTURE_RACE(...)` and
`WASMOS_FUTURE_ALL(...)` variadic convenience macros. `race` settles from the
first source outcome; `all` resolves to the caller's value array after every
source succeeds, or rejects on the first failure. Neither combinator can
unregister its source continuations yet, so its group state and continuation
array must remain live until every source future has settled.
The Zig wrappers take slices for inputs, values, and continuations, checking
their matching lengths before registering the group.

`wasmos_sys_native_ipc_future_t` promotes one native event-loop intent into a
caller-owned future. `wasmos_sys_native_ipc_future_send()` posts the ordinary
non-blocking IPC request and records its generated `request_id`; the existing
single endpoint pump resolves the operation with a copy of the matched reply.
A protocol-specific reply-status callback may reject the future. Local
`wasmos_sys_native_ipc_future_cancel()` removes the intent and rejects it; it
does not cancel transport work, so a later reply is discarded by request ID.
The C and Zig wrappers expose the same caller-storage contract.
The native net-stack uses this path for its `virtio.net` service lookup: its
control coroutine awaits the future and only then records the driver endpoint.
Native services may use libsys's `async_initialize` ELF entry: it starts a
caller-owned service runtime and invokes `wasmos_async_main` as its predefined
root coroutine with the loader-provided `wasmos_driver_api_t *`.

Deadlines, generic future cancellation, multi-worker synchronization, CQ
dispatch, allocator ownership, and guard-page stack allocation are deferred.
The caller must retain all coroutine and stack storage until the coroutine is
dead and no joiner can reference it.

The public C ABI is in
`src/libsys/native/include/wasmos/coroutine_native.h`. `libsys.zig` exposes the
same C types and initial runtime/future helpers, so Zig native services can use
the core when their package links the C and assembly objects. The native
`net-stack` package links those objects now, providing continuous target-build
validation before it adopts coroutines for socket operations.

`tests/unit/test_native_coroutine.c` validates cooperative yield order,
pending-future suspension/wakeup, duplicate promise settlement rejection,
join, value-transforming chains, rejection recovery, callback-caused
rejection, and `wasmos_async_start()` completion-future return on x86-64 and
AArch64 hosts. It also covers waiter and joiner fan-out, terminal callback
registration, unchanged-outcome forwarding, re-entrant callback dispatch,
contract rejection, yield stress, and race/all success and failure paths.
Other development hosts cross-compile the x86-64 target objects instead;
target-package compilation is additionally validated by the net-stack build.

`tests/unit/test_native_ipc_future.c` validates request construction and reply
copying, protocol-directed rejection, immediate transport-send failure, and
local cancellation/late-reply discard through a fake native driver API.

`future_then` now returns the continuation record's caller-owned child future,
so native C and Zig can build value-transforming, rejection-propagating chains
without allocation.

## 51. WASM Stackless C Baseline (implemented)

`src/libsys/wasm/{coroutine_wasm.c,include/wasmos/coroutine_wasm.h}` provides
the C WASM counterpart. It has the same caller-owned `Future`, `Promise`,
`then`, `race`, `all`, completion-future, and `wasmos_async_start()` concepts
as the native core, but it never attempts to capture a C/WASM call stack.

A task supplies `wasmos_wasm_task_resume_fn` and records its own program
counter in caller-owned state. When `wasmos_future_await()` returns
`WASMOS_WASM_AWAIT_PENDING`, the task is parked and its resume function must
return `WASMOS_WASM_TASK_YIELDED`; after settlement the scheduler invokes it
again. A task returns zero with an output value to resolve its completion
future, or a negative status to reject it. This is the portable C substrate
that later Zig, Rust, Go, and AssemblyScript wrappers will hide behind their
own async/state-machine mechanisms.

Future callbacks are always queued through `wasmos_wasm_coroutine_run()`;
neither registration nor promise settlement invokes user callbacks inline.
`race` settles on the first source outcome, and `all` resolves to the supplied
value array only after every input succeeds. Group and continuation storage is
caller-owned and must remain live until all source futures settle.

`tests/unit/test_wasm_coroutine.c` runs the C core as host code and validates
cooperative yield order, plain pending-future runtime binding, parking/wakeup,
completion join, duplicate settlement rejection, deferred `then` callbacks,
value and error propagation, and race/all success and failure behavior. An
x86-64-host-only `warp_wasm_coroutine_test` target additionally compiles
`tests/unit/wasm_coroutine_warp_fixture.c` to wasm32 and executes it through
the WARP JIT; it is deliberately excluded on ARM64 hosts while WARP's host
AArch64 execution path is not yet suitable for this validation.

WASM IPC-future adaptation, deadlines, cancellation, CQ dispatch, parallel
workers, and language wrappers remain deferred.
