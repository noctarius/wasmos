# Threading Design Document

This document defines how native kernel threads are introduced into WASMOS
without breaking the current boot/process model.

Keep this document aligned with `README.md`, `docs/ARCHITECTURE.md`, and `docs/TASKS.md`
as implementation lands.

---

## 1. Goals

Threading is introduced to provide concurrent execution within one process
address space while preserving current microkernel constraints.

Primary goals:

1. Keep scheduler behavior deterministic and auditable.
2. Preserve existing process lifecycle and process-manager contracts.
3. Allow blocking operations (IPC wait, joins, sleeps) to block only the
   calling thread.
4. Reuse the existing per-process memory context (`context_id`) rather than
   introducing per-thread page tables.
5. Keep the first implementation single-core (SMP remains future work).

Non-goals for first delivery:

- POSIX-complete pthread compatibility.
- Kernel-level priority scheduling.
- WASM shared-memory threads proposal support.
- Cross-core synchronization primitives.

---

## 2. Current Baseline and Gap

Today the kernel equates one schedulable unit with one `process_t`.
`process_context_t` is embedded directly in `process_t`, and the ready queue
holds PIDs.

After ring3-isolation rollout, this baseline also includes strict kernel-entry
hardening tied to the scheduled process:

- per-process kernel stacks with guard pages and canaries
- scheduler-managed TSS `rsp0` updates on dispatch
- strict user/root separation checks that assume kernel ingress lands on a
  valid higher-half kernel stack

This prevents:

- concurrent activities in one process without process duplication
- thread-local blocking semantics
- multi-threaded native drivers/services in one address space

Threading still requires splitting:

- **Resource container:** process
- **Execution container:** thread

---

## 3. Architectural Model

### 3.1 Ownership Split

- `process_t` owns:
  - `pid`, `parent_pid`, `context_id`
  - address-space / memory-context ownership
  - capability registry association
  - process exit status and child-process wait semantics
- `thread_t` owns:
  - `tid`, owner `pid`
  - scheduling state, timeslice, run accounting
  - CPU register context (`process_context_t`-compatible layout)
  - kernel stack (and later user stack metadata)
  - thread block reason and join-wait metadata

### 3.2 Scheduling Unit

Scheduler dispatches runnable `thread_t` entries.

A process is runnable if and only if at least one of its threads is runnable.

### 3.3 Memory Model

All threads in a process share:

- the same `context_id`
- the same virtual memory mappings
- the same capability envelope

First phase does not introduce per-thread VM isolation.

---

## 4. Kernel Data Structures

## 4.1 New Thread Table

A fixed-size thread table is added (similar to process table style):

```c
typedef enum {
    THREAD_STATE_UNUSED = 0,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_ZOMBIE
} thread_state_t;

typedef enum {
    THREAD_BLOCK_NONE = 0,
    THREAD_BLOCK_IPC,
    THREAD_BLOCK_JOIN,
    THREAD_BLOCK_SLEEP,
    THREAD_BLOCK_WAIT_PROCESS
} thread_block_reason_t;

typedef struct thread {
    uint32_t tid;
    uint32_t owner_pid;
    thread_state_t state;
    thread_block_reason_t block_reason;

    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;

    uint8_t in_ready_queue;
    uint8_t detached;

    process_context_t ctx;

    uintptr_t kstack_base;
    uintptr_t kstack_top;
    uint32_t kstack_pages;

    int32_t exit_status;
    uint32_t join_waiter_tid;

    char name_storage[PROCESS_NAME_MAX];
    const char *name;
} thread_t;
```

Thread count should be bounded by a new constant (for example
`THREAD_MAX_COUNT 128`) and validated against memory footprint.

## 4.2 Process Structure Updates

`process_t` keeps process-level fields and drops direct scheduler coupling.
It tracks thread membership and thread-group state:

- `main_tid`
- `thread_count`
- `live_thread_count`
- `exiting` flag (group-exit in progress)

Process slot reclamation occurs only after all threads are `ZOMBIE`/reaped.

---

## 5. Scheduling and Context Switch

## 5.1 Ready Queue

Replace PID queue with TID queue.

Queue invariant:

- only `THREAD_STATE_READY` threads may be enqueued
- a thread appears at most once

## 5.2 Current Execution Pointers

Replace single current-process pointer with:

- `g_current_thread`
- `g_current_process` (derived from `g_current_thread->owner_pid`)

Helper APIs:

- `thread_current_tid()`
- `thread_current()`
- `thread_owner_process(thread_t *)`

## 5.3 Timer Preemption

`process_tick()`/preemption logic becomes thread-aware:

- decrement current thread slice
- mark reschedule when slice expires
- preserve current ring3 trampoline handling, but target thread context
- preserve strict trap-frame validation and watchdog liveness checks already
  enforced in ring3 smoke

## 5.4 Context Format

Reuse existing `process_context_t` layout for thread contexts so
`context_switch.S` can remain mostly unchanged.

Any assembly symbol names referring to process-context should be renamed only
if needed for readability; binary layout must stay stable.

## 5.5 Kernel Stack Contract (Ring3 Constraint)

Thread scheduling must preserve the current ring3 kernel-entry contract:

- each runnable thread has its own kernel stack (guard pages + canaries)
- scheduler refreshes TSS `rsp0` to the selected thread kernel stack before
  returning to user mode
- stack allocation continues to use the higher-half-safe mapping model used by
  strict ring3 validation (no regression to low-slot kernel stack exposure)

---

## 6. Lifecycle Semantics

## 6.1 Creation

Two creation paths:

1. **Bootstrap/main thread:** created with process spawn.
2. **Additional threads:** created by new thread-create kernel API/syscall.

Thread creation requirements:

- owner process exists and is not exiting
- stack allocation succeeds
- initial context points to thread trampoline

## 6.2 Exit

`thread_exit(status)`:

- marks thread `ZOMBIE`
- stores exit status
- wakes join waiter if present
- if last live thread, transitions process to exited and notifies process
  waiters/process-manager as today

## 6.3 Join and Detach

Join model (phase 1):

- one joiner per target thread
- joining self is invalid
- detached threads cannot be joined

Detached thread resources are reclaimed automatically after exit.

## 6.4 Kill/Process Exit

`process_kill(pid, status)` changes from single-context termination to
thread-group termination:

- mark process as exiting
- force all member threads toward exit
- preserve existing process-manager permission checks

---

## 7. Blocking and Wakeup Semantics

## 7.1 IPC

Current IPC wakeups use `process_wake_by_context(context_id)`.

Threading adds endpoint-level wait ownership:

- `ipc_recv_for` blocks current thread
- endpoint wait queue stores waiting TIDs
- send/notify wakes one (or more for notifications if required) waiting thread

Compatibility fallback may wake all threads in same context during transition,
but final behavior should be targeted wakeup to avoid thundering-herd behavior.

## 7.2 Wait APIs

- `process_wait` remains process-level and blocks only calling thread.
- `thread_join` is thread-level.

## 7.3 Sleep/Timer Blocking

If sleep primitives are introduced, they must block thread-only and keep other
threads in same process runnable.

---

## 8. Syscall and ABI Plan

Existing syscall primitives (`nop`, `getpid`, `exit`) remain valid.

Add thread-focused syscalls in phases:

1. `gettid`
2. `thread_exit`
3. `thread_yield`
4. `thread_create`
5. `thread_join`
6. `thread_detach`

`exit` remains process-group exit (terminates all threads).

User/kernel ABI rules:

- syscall numbers are append-only
- argument convention remains current register ABI
- unsupported syscalls return existing error convention

## 8.1 User API Direction (Continuation-Style Wrapper)

User-facing threading APIs should default to a continuation-like model layered
on top of raw thread syscalls rather than exposing a POSIX `pthread` surface.

Direction for later user-space/libc work:

- keep kernel ABI minimal and explicit (`thread_create`, `thread_yield`,
  `thread_join`, `thread_exit`, `thread_detach`, `gettid`)
- provide higher-level helpers that model spawn/yield/await flows
- allow runtimes/services to hide syscall details behind continuation/task
  handles

This keeps runtime integration simple for WASM-first workloads while preserving
the existing non-goal of POSIX-complete pthread compatibility.

---

## 9. Ring3 Integration

Threading must keep ring3 transition machinery correct:

- each runnable thread carries its own privilege return metadata
- scheduler updates TSS `rsp0` for selected thread kernel stack
- IRQ trampoline rewrite for CPL3 preemption applies per-thread

Future TLS support (FS/GS base) is deferred; reserve structure fields or
side-table hooks for later extension.

---

## 10. WASM Runtime Considerations

Phase 1 threads are kernel/native scheduling primitives; wasm3 remains
single-threaded per runtime instance.

Practical policy for initial rollout:

- one runtime instance per WASM process thread entrypoint (main thread)
- no concurrent hostcalls into same wasm3 instance
- native services/drivers may exploit additional threads first

WASM shared-memory atomics/thread proposal support is explicitly deferred.

---

## 11. Synchronization Primitives

Kernel must provide minimal primitives for safe shared-state access:

- existing spinlocks remain valid in kernel
- user-visible synchronization starts with simple futex-like wait/wake or
  thread join-only, depending on rollout phase

First landing can avoid a full futex interface if internal services do not
require general condition waiting immediately.

---

## 12. Failure Modes and Invariants

Critical invariants:

1. A thread context is never scheduled after resource reclamation.
2. A process context is reclaimed only after all member threads are dead.
3. Endpoint ownership checks remain by process context/capabilities; thread
   identity does not bypass capability checks.
4. Killing a process cannot leak blocked waiters on IPC/join queues.

Known risks:

- stale wait-queue entries during forced process kill
- races between join/detach and thread exit
- scheduler starvation if wakeup policy is too coarse

Add explicit assertions and diagnostic trace marks around these edges.

---

## 13. Implementation Phases

## Phase A: Kernel Refactor (No New Syscalls)

- Introduce `thread_t` and thread scheduler internals.
- Keep one thread per process functionally.
- Preserve all current user-visible behavior.

Current status:

- scheduler-internal migration complete for Phase A scope
- kernel now allocates a `thread_t` main thread per spawned process and mirrors
  baseline process state transitions into that thread record
- scheduler ready queue now stores `tid` entries and derives process ownership
  from the dequeued thread
- scheduler quantum/run accounting is now thread-owned (`thread_t`), while
  externally visible behavior remains one-thread-per-process

Exit criteria:

- existing boot flow unchanged
- existing tests pass
- strict ring3 baseline remains green (`run-qemu-test`,
  `run-qemu-ring3-test`, `run-qemu-cli-test`)
- no process-manager contract changes

## Phase B: Internal Multi-thread Enablement

- Enable kernel/native components to spawn extra threads.
- IPC wait/wake semantics migrated to thread-level.

Current status:

- exit criterion satisfied for current scope
- IPC endpoints now track a waiting `tid` and prefer targeted thread wakeup
  before falling back to context-wide wake behavior
- internal kernel API `process_thread_spawn_internal` now allocates additional
  per-process `thread_t` records with lifecycle accounting
- internal kernel worker threads are now schedulable with dedicated per-thread
  kernel stacks and explicit worker entrypoints; kernel smoke emits
  `[test] threading internal worker ok` when worker scheduling/lifecycle
  completes successfully
- targeted kernel multi-thread IPC stress smoke now runs in baseline boot and
  emits `[test] threading ipc stress ok` on pass

Exit criteria:

- targeted kernel stress test with multi-threaded IPC passes

## Phase C: User-visible Thread Syscalls

- Expose create/join/exit/yield in syscall table and libc shims.
- Add sample user program and integration tests.

Current status:

- complete for current scope
- syscall ABI now includes `gettid` and `thread_yield` for native ring3
  callers, with strict ring3 smoke markers:
  `[test] ring3 native gettid ok` and `[test] ring3 thread yield syscall ok`
- syscall ABI now also includes `thread_exit` with strict ring3 smoke marker
  `[test] ring3 thread exit syscall ok`; scheduler path now exits only the
  calling thread and preserves process execution while other threads remain
  live
- syscall ABI now includes functional `thread_create` with strict ring3 smoke
  marker (`[test] ring3 thread create syscall ok`); new threads now start with
  thread-owned user register context (RIP/RSP/CS/SS/root-table) and are queued
  runnable through the existing scheduler path
- syscall ABI now also includes initial `thread_join` handling and strict
  native ring3 coverage markers (`[test] ring3 thread join syscall ok`,
  `[test] ring3 thread join self deny ok`); dedicated strict ring3 threading
  smoke now runs via `run-qemu-ring3-threading-test` and validates join/detach
  syscall lifecycle signals without changing baseline strict startup pressure
- syscall ABI now also includes initial `thread_detach` handling and strict
  native ring3 coverage markers (`[test] ring3 thread detach syscall ok`,
  `[test] ring3 thread detach invalid deny ok`,
  `[test] ring3 thread detach join deny ok`); detached threads are now marked
  non-joinable and auto-reaped on exit in scheduler exit paths
- user-facing continuation-style thread wrapper API
  (`wasmos/thread_x86_64.h`) is now available for native ring3 callers; the
  strict lifecycle probe currently keeps raw-syscall flow until its early
  stack-writability constraints are widened

Exit criteria:

- user-level thread smoke tests pass under `run-qemu-test`

## Phase D: Ring3 + Hardening

- Full ring3 thread context handling, join/kill race hardening, trace coverage.

Current status:

- in progress
- wait-target wakeups now resume the actual blocked waiter thread selected via
  scheduler transition state (instead of always waking process main thread),
  which hardens process-exit wake behavior for multi-thread waiters
- dedicated threading smoke now includes a kill-while-blocked wait regression
  marker (`[test] threading wait kill wake ok`) to verify blocked waiters wake
  and observe kill exit status on target termination
- baseline now includes a dedicated threading join-order smoke probe that
  validates in-process join wake ordering with a delayed target and blocked
  waiter (`[test] threading join wake order ok`)
- dedicated threading smoke now also includes a focused join-after-exit probe
  in the lifecycle app path to preserve regression coverage while keeping the
  strict threading gate deterministic
- dedicated threading lifecycle probe now uses dedicated per-thread user stacks
  (instead of tightly-offset current-`rsp` reuse) to keep detach/join deny
  marker emission deterministic under scheduler/fault pressure

---

## 14. Test Plan

Required tests per phase:

1. Boot + scheduler smoke (`run-qemu-test` baseline).
2. Context-switch correctness under timer preemption.
3. IPC blocking/wakeup correctness with multiple threads in one process.
4. Join/detach semantics (success, double-join, self-join, detach-then-join).
5. Process kill while threads are blocked in IPC/join/sleep.
6. Ring3 syscall behavior from non-main thread contexts.

Test discipline:

- keep test binaries tiny and deterministic
- avoid parallel integration test runs (shared mutable `build/esp`)
- keep cross-thread ring3 lifecycle smoke on a dedicated opt-in target
  (`run-qemu-ring3-threading-test`) so baseline strict ring3 startup remains
  deterministic

---

## 15. Open Questions

1. Should endpoint wait queues wake exactly one waiter or all waiters for
   notification-type endpoints?
2. Do we keep PID/TID in one global namespace or maintain separate spaces?
3. Should process-manager report per-thread status for diagnostics, or only
   process-level state initially?
4. Do we require detached main-thread support, or reserve main-thread special
   behavior in phase 1?

---

## 16. Summary

Threading in WASMOS should be implemented as a minimal extension of current
scheduler/process infrastructure:

- process = resource/capability/address-space owner
- thread = schedulable execution context

This preserves the current microkernel split, enables incremental delivery,
and avoids introducing broad policy or runtime complexity before core invariants
(preemption, IPC wakeup, lifecycle safety) are proven.
