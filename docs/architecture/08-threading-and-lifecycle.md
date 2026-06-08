## Threading and Lifecycle

This document describes the kernel threading model, data structures, lifecycle
transitions, blocking primitives, and the WASM runtime thread mapping.

---

### Model Overview

- **Process** (`process_t`) — resource/address-space/capability owner.
  Up to `PROCESS_MAX_COUNT = 48` processes.
- **Thread** (`thread_t`) — schedulable execution context.
  Up to `THREAD_MAX_COUNT = 128` threads across all processes.
- Threads in one process share `context_id`, virtual memory mappings, and the
  capability envelope.
- The priority queue stores thread pointers directly via intrusive list nodes;
  process ownership is derived from each dequeued thread's `owner_pid`.
- Current scope: single-core.  The scheduler data structures are ready for SMP
  (per-CPU `cpu_sched_t`, `cpu_affinity` per thread) but only one CPU is active.

---

### Key Constants

Source: `src/kernel/include/process.h`, `src/kernel/include/thread.h`,
`src/kernel/process.c`

| Constant                      | Value                       | Meaning                                      |
|-------------------------------|-----------------------------|----------------------------------------------|
| `PROCESS_MAX_COUNT`           | 48                          | Maximum simultaneous processes               |
| `THREAD_MAX_COUNT`            | 128                         | Maximum threads (all processes combined)     |
| `PROCESS_NAME_MAX`            | 64                          | Process name buffer bytes                    |
| `THREAD_NAME_MAX`             | 64                          | Thread name buffer bytes                     |
| `PROCESS_STACK_SIZE`          | 524288 (512 KiB)            | Kernel stack size per process/thread         |
| `PROCESS_DEFAULT_SLICE_TICKS` | 5                           | Round-robin timeslice in timer ticks         |
| `PROCESS_CTX_CANARY_VALUE`    | `0xC0FFEE0DD15EA5EULL`      | Context struct guard canary                  |
| `STACK_GUARD_PAGES`           | 1                           | Guard pages on each side of every stack      |
| `PAGE_SIZE`                   | 4096 (0x1000)               | Physical/virtual page size                   |
| `SCHED_RESCHED_STALL_TICKS`   | 512                         | Ticks before stall watchdog fires            |

---

### CPU Context Layout

Source: `src/kernel/include/process.h`

`process_context_t` is the register-save area used by the scheduler.  Every
`thread_t` embeds exactly one instance flanked by canary words.

```c
typedef struct {
    uint64_t r15;       /* offset   0 */
    uint64_t r14;       /* offset   8 */
    uint64_t r13;       /* offset  16 */
    uint64_t r12;       /* offset  24 */
    uint64_t r11;       /* offset  32 */
    uint64_t r10;       /* offset  40 */
    uint64_t r9;        /* offset  48 */
    uint64_t r8;        /* offset  56 */
    uint64_t rdi;       /* offset  64 */
    uint64_t rsi;       /* offset  72 */
    uint64_t rbp;       /* offset  80 */
    uint64_t rdx;       /* offset  88 */
    uint64_t rcx;       /* offset  96 */
    uint64_t rbx;       /* offset 104 */
    uint64_t rax;       /* offset 112 */
    uint64_t rsp;       /* offset 120 */
    uint64_t rip;       /* offset 128 */
    uint64_t rflags;    /* offset 136 */
    uint64_t cs;        /* offset 144 */
    uint64_t ss;        /* offset 152 */
    uint64_t user_rsp;  /* offset 160 — saved user RSP for ring3 return */
    uint64_t root_table;/* offset 168 — physical PML4 base for this thread */
} process_context_t;
```

Offsets are verified by `_Static_assert` at compile time.

`irq_frame_t` is the hardware-pushed frame used by ISR stubs.  The preemption
path copies it into the current thread's `process_context_t`.

---

### Process Structure

Source: `src/kernel/include/process.h`

```c
typedef struct process {
    uint32_t pid, parent_pid, context_id;
    uint32_t main_tid;
    uint32_t thread_count;       /* total threads ever spawned */
    uint32_t live_thread_count;  /* currently non-zombie threads */
    uint8_t  exiting;            /* group-exit in progress */
    process_state_t state;       /* UNUSED / ALIVE / REAPING / ZOMBIE */
    uint32_t wait_target_pid;    /* active process_wait target */
    int32_t  exit_status;
    uint32_t time_slice_ticks;
    uint64_t ticks_total;
    uint8_t  is_idle, in_hostcall;
    uint8_t  auto_reap, is_wasm, ready, require_explicit_ready;
    uintptr_t stack_base, stack_top;
    uintptr_t stack_alloc_base_phys;
    uint32_t  stack_pages;
    process_entry_t entry;
    void *arg;
    char name_storage[64];
    const char *name;
    spinlock_t  wasm3_lock;      /* held for duration of wasm3 entry_fn call */
    uint32_t    wasm3_owner;     /* TID currently executing in wasm3; 0 = free */
    sched_event_t wait_event;   /* process-level wait (child exit notification) */
} process_t;
```

`process_t` no longer contains `ctx` or the canary pair; those are per-thread.
`in_ready_queue` and `block_reason` are removed — scheduling state lives in
`thread_t`.

#### Process States

| State                    | Value | Meaning                              |
|--------------------------|-------|--------------------------------------|
| `PROCESS_STATE_UNUSED`   | 0     | Slot free                            |
| `PROCESS_STATE_ALIVE`    | 1     | At least one live thread             |
| `PROCESS_STATE_REAPING`  | 6     | Being reaped; slots still valid      |
| `PROCESS_STATE_ZOMBIE`   | 4     | All threads exited; awaiting reap    |

`PROCESS_STATE_ALIVE` is an alias for `PROCESS_STATE_READY` in the enum; code
that only distinguishes live vs. dead uses `>= ALIVE`.

#### WASM Reentrancy Guard

wasm3 is not reentrant.  `wasm3_lock` (spinlock) is acquired before every call
into wasm3 and released on return.  `wasm3_owner` records the occupying TID so
the owning thread can re-enter without deadlocking on its own lock.  Kernel
worker threads (`is_kernel_worker = 1`) never acquire `wasm3_lock`.

---

### Thread Structure

Source: `src/kernel/include/thread.h`

```c
typedef struct thread {
    uint32_t tid;
    uint32_t owner_pid;
    thread_state_t state;
    thread_block_reason_t block_reason;
    uint8_t  is_kernel_worker;
    uintptr_t kstack_base, kstack_top;
    uintptr_t kstack_alloc_base_phys;
    uint32_t  kstack_pages;
    uintptr_t worker_entry;
    void *worker_arg;
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    uint8_t  blocking_transition;  /* atomic: 1 while transitioning to BLOCKED */
    uint32_t wasm3_heap_bound_pid; /* heap slot override during worker dispatch */
    uint64_t ctx_canary_pre;       /* == PROCESS_CTX_CANARY_VALUE */
    process_context_t ctx;         /* register state for ALL thread types */
    uint64_t ctx_canary_post;      /* == PROCESS_CTX_CANARY_VALUE */
    uint8_t  sched_prio;           /* priority 0 (highest) – 6 (idle) */
    uint32_t cpu_affinity;         /* allowed CPU bitmask (~0u = any) */
    uint32_t last_cpu;             /* CPU where thread last ran */
    list_head_t sched_node;        /* intrusive linkage in ready_list[prio] */
    list_head_t event_node;        /* intrusive linkage in sched_event_t.wait_list */
    sched_event_t *wait_event;     /* event this thread is currently blocked on */
    sched_pend_state_t pend_state; /* PEND_NONE / PEND_OK / PEND_TIMEOUT / PEND_ABORT */
    uint64_t pend_data;            /* value from waker */
    sched_event_t join_event;      /* embedded event for join waiters */
    uint8_t  detached;
    int32_t  exit_status;
    char     name_storage[64];
    const char *name;
} thread_t;
```

#### Thread States

| State                   | Value | Meaning                                    |
|-------------------------|-------|--------------------------------------------|
| `THREAD_STATE_UNUSED`   | 0     | Slot free                                  |
| `THREAD_STATE_READY`    | 1     | Runnable; in priority queue                |
| `THREAD_STATE_RUNNING`  | 2     | Currently executing                        |
| `THREAD_STATE_BLOCKED`  | 3     | Waiting on IPC, join, futex, or event      |
| `THREAD_STATE_ZOMBIE`   | 4     | Exited; slot held for joiner or reap       |

#### Thread Block Reasons

| Reason                      | Value | Meaning                                      |
|-----------------------------|-------|----------------------------------------------|
| `THREAD_BLOCK_NONE`         | 0     |                                              |
| `THREAD_BLOCK_IPC`          | 1     | Blocked in `ipc_recv_blocking_for`           |
| `THREAD_BLOCK_WAIT_PROCESS` | 2     | Blocked in `process_wait`                    |
| `THREAD_BLOCK_WAIT_THREAD`  | 3     | Blocked in `process_thread_join`             |
| `THREAD_BLOCK_EVENT`        | 4     | Blocked via `sched_event_wait` (futex, etc.) |

---

### Stack Layout

Each process and each thread gets a kernel stack.  Both use `PROCESS_STACK_SIZE`
(512 KiB = 128 pages).  Allocation includes one guard page on each side:

```
[guard page]  ← paged-out; #PF on underflow
[stack pages] ← 128 pages usable kernel stack
[guard page]  ← paged-out; #PF on overflow
```

Guard pages are restored before physical pages are freed so recycled pages
remain reachable through the shared higher-half alias window.

---

### Thread Spawn Paths

Source: `src/kernel/process.h`, `src/kernel/process.c`

| Function                                                                         | Use                                               |
|----------------------------------------------------------------------------------|---------------------------------------------------|
| `thread_spawn_main(owner_pid, name, *tid)`                                       | Creates the main thread for a new process (READY) |
| `thread_spawn_in_owner(pid, name, state, reason, *tid)`                          | Additional thread at given initial state          |
| `process_thread_spawn_internal(pid, name, *tid)`                                 | Spawn with kernel entry (compatibility shim)      |
| `process_thread_spawn_worker_internal(pid, name, entry, arg, *tid)`              | Spawn kernel worker (runs C function on kstack)   |
| `process_thread_spawn_user_internal(pid, name, entry_rip, user_stack_top, *tid)` | Spawn user-mode ring3 thread                      |

`sched_thread_init` is a consolidated helper called by all spawn paths.  It sets
canary words, `sched_prio`, `cpu_affinity`, initializes `sched_node` and
`event_node` list heads, and calls `sched_event_init(&thread->join_event)`.

User threads spawned via `WASMOS_SYSCALL_THREAD_CREATE` call
`process_thread_spawn_user_internal`, which initializes
`thread.ctx.rip = entry_rip`, `thread.ctx.cs = USER_CS_SELECTOR`,
`thread.ctx.user_rsp = user_stack_top & ~0xFULL`, and a dedicated kernel stack.

---

### IPC Blocking and Wakeup

Source: `src/kernel/ipc.c`, `src/kernel/sched_event.c`

IPC receive uses two variants:

**`ipc_recv_for(context_id, endpoint, out)`** — non-blocking.  If a message is
available, dequeues it and returns `IPC_OK`.  If empty, registers the current
thread in `ep->event.wait_list` (so a future sender's `poll_notify` will wake
it) and returns `IPC_EMPTY`.  Used by the scheduler's YIELDED handler to clean
up stale registrations.

**`ipc_recv_blocking_for(context_id, endpoint, out)`** — true blocking.  On
`IPC_EMPTY`, calls `sched_event_wait(&ep->event, 0)` which parks the thread
until a sender wakes it.  Used by WASM host functions (`wasmos_ipc_select_one`,
`wasmos_ipc_select_wait`) and kernel init paths.

`ipc_send_from` calls `sched_event_wake_one(&ep->event, ...)` to wake any
blocked receiver.  It also calls `poll_notify(ep->poll_struct, POLL_EV_IN,
ep->id)` to signal any registered select sets.

`ipc_endpoint_t` no longer contains `waiter_tid`; it contains an embedded
`sched_event_t event` and a pointer `poll_struct_t *poll_struct`.

#### Single-Registration Invariant

A thread's `event_node` is in at most one `wait_list` at any time.  Before
`ipc_recv_for` adds the thread to a new endpoint's wait_list it removes it from
any previous one.  Similarly, `sched_event_wait` removes from a previous list
before adding to the new one.

---

### Thread Lifecycle Transitions

```
UNUSED ──spawn──▶ READY ──dispatch──▶ RUNNING ──yield/block──▶ READY/BLOCKED
                                         │
                                     exit/kill
                                         │
                                         ▼
                                      ZOMBIE ──join/auto-reap──▶ UNUSED
```

#### Exit (`PROCESS_RUN_EXITED` / `PROCESS_RUN_THREAD_EXITED`)

**Thread-only exit** (`PROCESS_RUN_THREAD_EXITED`):
- Thread state → `ZOMBIE`.
- Store exit status in `thread.exit_status`.
- `sched_event_wake_all(&thread->join_event, ...)` wakes any joiners.
- Decrement `proc->live_thread_count`.
- If other threads remain: find next ready thread in the same process.
- If last live thread: mark process ZOMBIE, wake process-level waiters via
  `sched_event_wake_all(&proc->wait_event, ...)`.

**Process exit** (`PROCESS_RUN_EXITED`):
- `proc->exiting = 1`.
- All member threads are marked toward exit.
- Blocked IPC/join/wait waiters are woken.
- Process is reclaimed only after all threads reach `ZOMBIE`.

**Detached threads** (`thread.detached = 1`):
- Join attempts return an error.
- On exit: thread slot is auto-reaped instead of held as zombie.

#### Join (`process_thread_join`)

- Self-join: denied.
- Already-detached target: denied.
- Only one joiner per thread (the `join_event` wait_list allows more, but the
  policy only sets one).
- If target already zombie: returns immediately with stored `exit_status`.
- Otherwise: caller calls `sched_event_wait(&target->join_event, 0)`.

#### Detach (`process_thread_detach`)

- Sets `thread.detached = 1`.
- If any joiner is already blocked: denied.
- If thread already zombie: auto-reap is triggered immediately.

#### Process Kill (`process_kill(pid, exit_status)`)

- Marks `proc->exiting = 1`, sets `exit_status`.
- Forces all live member threads toward exit.
- Wakes blocked process-level waiters via `sched_event_wake_all(&proc->wait_event, ...)`.
- `auto_reap = 1` processes are reaped once all threads are zombie.

---

### Context Switch

Source: `context_switch.S`, `src/kernel/process.c`

`context_switch_high(from_ctx, to_ctx)`:
1. Save all GPRs + rsp/rip/rflags/cs/ss into `from_ctx`.
2. Load GPRs + rsp/rip from `to_ctx`.
3. Switch CR3 if `to_ctx.root_table` differs from current.
4. Return to `to_ctx.rip`.

The syscall handler copies the full trap frame into `current_thread->ctx` before
any blocking path so resumed threads continue at the correct post-syscall RIP.

---

### WASM Runtime Thread Mapping

Source: `src/kernel/wasm_driver.c`, `src/kernel/wasm3_shim.c`

Each WASM thread maps to a dedicated wasm3 VM instance:
- Separate `IM3Environment`, `IM3Runtime`, `IM3Module` per thread.
- No concurrent entry into the same VM instance; `wasm3_lock` in `process_t`
  enforces serial wasm3 use per process.
- Thread-local heap: `wasm3_heap_bind_pid(pid)` keyed by PID, not TID.
  Up to `WASM_DRIVER_THREAD_SLOTS = 64` VM thread slots.
- `wasm_driver_spawn_vm_thread()` allocates a slot and spawns a kernel worker
  thread that creates a fresh wasm3 stack and calls the target WASM export.

Preemption is gated around all wasm3 interpreter entry points via
`in_hostcall = 1`; the preemption path skips threads with this flag set.

---

### Process Statistics (`ps` output)

Source: `src/kernel/include/process.h` (`process_stats_t`)

| Field                       | Meaning                                                        |
|-----------------------------|----------------------------------------------------------------|
| `state`                     | Current process state                                          |
| `block_reason`              | Legacy field; always NONE in new scheduler                     |
| `is_wasm`                   | 1 if process runs a WASM payload                               |
| `thread_count`              | Total threads spawned for this process                         |
| `live_thread_count`         | Non-zombie threads                                             |
| `current_tid`               | TID of last dispatched thread                                  |
| `context_id`                | MM context ID                                                  |
| `cpu_ticks`                 | Cumulative ticks from all threads                              |
| `vm_total_bytes`            | Virtual memory total                                           |
| `thread_kstack_total_bytes` | Sum of all thread kernel stack bytes                           |
| `heap_committed_bytes`      | Committed wasm3 heap across all process chunks                 |
| `rss_est_bytes`             | RSS estimate                                                   |

---

### Threading Test Markers

Threading correctness is validated by selftests that run automatically at boot.
Markers emitted to the serial log:

| Marker                                      | Condition                                         |
|---------------------------------------------|---------------------------------------------------|
| `[test] preempt ok`                         | First timer preemption fires and triggers resched |
| `[test] sched progress ok`                  | Scheduler completes N successful dispatches       |
| `[test] threading internal worker ok`       | Kernel worker thread runs to completion           |
| `[test] threading ipc stress ok`            | Multi-thread IPC stress (32 exchanges) completes  |
| `[test] threading wait kill wake ok`        | Blocked waiter wakes correctly on target kill     |
| `[test] threading join wake order ok`       | Join waiter wakes after delayed target exit       |

---

### Structural Invariants

1. **Thread slot is never scheduled after reclamation.** `THREAD_STATE_UNUSED`
   slots are never enqueued.  Reclamation (`thread_reap`) clears `tid`, sets
   state to `UNUSED`.

2. **Process slot is reclaimed only after all threads are zombie.**
   `live_thread_count` tracks this; the process zombie transition is guarded by
   `live_thread_count == 0`.

3. **Capability checks are process-context-bound.** Thread identity does not
   grant additional capabilities.  `capability_has(context_id, kind)` uses the
   shared process `context_id`.

4. **Endpoint ownership is by context_id, not TID.** A thread can only send to
   endpoints its process owns.  IPC wake targets a specific thread via
   `sched_event_wake_one` but the endpoint remains owned by the process.

5. **Context canaries bracket `thread_t.ctx`.** Any stack corruption that
   overwrites the context struct will be detected before the corrupted context
   is dispatched.  Mismatch → `hlt` loop (no silent bad dispatch).

6. **TSS.rsp0 is updated on every dispatch.** `cpu_set_kernel_stack(stack_top - 16u)`
   is called in `process_schedule_once_impl` before `context_switch_high`.
   Ring3 interrupts and syscalls always land on the current thread's kernel stack.

7. **`blocking_transition` guards against lost wakes.** When a thread sets
   `blocking_transition = 1` and begins saving its context, a concurrent
   `sched_wake_thread` spin-waits until the flag clears before enqueuing the
   thread.  This prevents the thread from being scheduled before its context
   save is complete.

8. **`event_node` is in at most one wait_list.** Before adding a thread to a new
   event's wait_list, its current membership is always removed first.  This
   prevents list corruption on successive blocking calls.
