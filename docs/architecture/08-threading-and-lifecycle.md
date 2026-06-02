## Threading and Lifecycle

This document describes the kernel threading model, data structures, scheduler
dispatch, context switch, lifecycle transitions, and the WASM runtime thread
mapping.

---

### Model Overview

- **Process** (`process_t`) — resource/address-space/capability owner.
  Up to `PROCESS_MAX_COUNT = 48` processes.
- **Thread** (`thread_t`) — schedulable execution context.
  Up to `THREAD_MAX_COUNT = 128` threads across all processes.
- Threads in one process share `context_id`, virtual memory mappings, and the
  capability envelope.
- The ready queue stores TIDs; process ownership is derived from each dequeued
  thread's `owner_pid`.
- Multi-core capable when `WASMOS_SMP=1`: each online AP runs the same
  round-robin dispatch loop against the shared spinlock-protected ready queue.
  See `docs/architecture/28-smp.md` for the steady-state AP contract.

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

`process_context_t` is the register-save area used by the scheduler and shared
between `process_t` and `thread_t`:

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

Offsets are verified by `_Static_assert` at compile time. The assembly
context-switch path (`context_switch_high`) depends on this exact layout.

`irq_frame_t` is the hardware-pushed frame used by ISR stubs:

```c
typedef struct {
    uint64_t r15;        /* offset   0 */
    /* ... r14–rbx ... */
    uint64_t rax;        /* offset 112 */
    uint64_t rip;        /* offset 120 */
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;   /* offset 144 */
    uint64_t user_ss;
} irq_frame_t;
```

The preemption path (`process_preempt_from_irq`) copies `irq_frame_t` fields
into the current thread's `process_context_t`.

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
    process_state_t state;
    process_block_reason_t block_reason;
    uint32_t wait_target_pid;
    int32_t  exit_status;
    uint32_t time_slice_ticks;   /* currently unused at process level */
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    uint8_t  in_ready_queue, is_idle, in_hostcall;
    uint8_t  auto_reap, is_wasm, ready, require_explicit_ready;
    uint64_t ctx_canary_pre;     /* == PROCESS_CTX_CANARY_VALUE */
    process_context_t ctx;       /* current register state */
    uint64_t ctx_canary_post;    /* == PROCESS_CTX_CANARY_VALUE */
    uintptr_t stack_base, stack_top;
    uintptr_t stack_alloc_base_phys;
    uint32_t  stack_pages;
    process_entry_t entry;
    void *arg;
    char name_storage[64];
    const char *name;
} process_t;
```

#### Process States

| State                    | Value | Meaning                              |
|--------------------------|-------|--------------------------------------|
| `PROCESS_STATE_UNUSED`   | 0     | Slot free                            |
| `PROCESS_STATE_READY`    | 1     | At least one thread runnable         |
| `PROCESS_STATE_RUNNING`  | 2     | Currently dispatched                 |
| `PROCESS_STATE_BLOCKED`  | 3     | All threads blocked                  |
| `PROCESS_STATE_ZOMBIE`   | 4     | All threads exited; awaiting reap    |

#### Process Block Reasons

| Reason                  | Value | Meaning                    |
|-------------------------|-------|----------------------------|
| `PROCESS_BLOCK_NONE`    | 0     |                            |
| `PROCESS_BLOCK_IPC`     | 1     | Waiting on IPC recv        |
| `PROCESS_BLOCK_WAIT`    | 2     | Waiting on child PID       |

#### Process Run Results

Returned by `process_entry_t` (kernel WASM/native entry functions) to inform
the scheduler of the exit reason:

| Result                      | Value | Meaning                                |
|-----------------------------|-------|----------------------------------------|
| `PROCESS_RUN_YIELDED`       | 0     | Voluntarily yielded                    |
| `PROCESS_RUN_IDLE`          | 1     | Idle process (re-enqueue without tick) |
| `PROCESS_RUN_BLOCKED`       | 2     | Blocked; do not re-enqueue             |
| `PROCESS_RUN_EXITED`        | 3     | Process exited                         |
| `PROCESS_RUN_THREAD_EXITED` | 4     | Thread-only exit; process continues    |

---

### Thread Structure

Source: `src/kernel/include/thread.h`

```c
typedef struct thread {
    uint32_t tid;
    uint32_t owner_pid;
    thread_state_t state;
    thread_block_reason_t block_reason;
    uint8_t  in_ready_queue;
    uint8_t  is_kernel_worker;    /* runs on kernel stack via worker_entry */
    uintptr_t kstack_base, kstack_top;
    uintptr_t kstack_alloc_base_phys;
    uint32_t  kstack_pages;
    uintptr_t worker_entry;       /* C function pointer for kernel workers */
    void *worker_arg;
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    process_context_t ctx;        /* register state for context switch */
    uint32_t wait_target_pid;
    uint32_t join_waiter_tid;     /* TID of the thread waiting to join this one */
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
| `THREAD_STATE_READY`    | 1     | Runnable; in or eligible for ready queue   |
| `THREAD_STATE_RUNNING`  | 2     | Currently executing                        |
| `THREAD_STATE_BLOCKED`  | 3     | Waiting on IPC, join, or process-wait      |
| `THREAD_STATE_ZOMBIE`   | 4     | Exited; slot held for joiner or reap       |

#### Thread Block Reasons

| Reason                        | Value | Meaning                            |
|-------------------------------|-------|------------------------------------|
| `THREAD_BLOCK_NONE`           | 0     |                                    |
| `THREAD_BLOCK_IPC`            | 1     | Blocked in `ipc_recv_for`          |
| `THREAD_BLOCK_WAIT_PROCESS`   | 2     | Blocked in `process_wait`          |
| `THREAD_BLOCK_WAIT_THREAD`    | 3     | Blocked in `process_thread_join`   |

---

### Stack Layout

Each process and each thread gets a kernel stack. Both use `PROCESS_STACK_SIZE`
(512 KiB = 128 pages). Allocation includes one guard page on each side:

```
[guard page]  ← paged-out; #PF on underflow
[stack pages] ← 128 pages usable kernel stack
[guard page]  ← paged-out; #PF on overflow
```

`STACK_GUARD_PAGES = 1` on each side. Physical allocation:
`total_pages = stack_pages + 2 * STACK_GUARD_PAGES`.

Guard pages are restored before the physical pages are freed
(`process_restore_stack_guard_mappings`) so recycled pages remain reachable
through the shared higher-half alias window.

---

### Thread Spawn Paths

Source: `src/kernel/include/process.h`, `src/kernel/process.c`

| Function                                                                         | Use                                               |
|----------------------------------------------------------------------------------|---------------------------------------------------|
| `thread_spawn_main(owner_pid, name, *tid)`                                       | Creates the main thread for a new process (READY) |
| `thread_spawn_in_owner(pid, name, state, reason, *tid)`                          | Additional thread at given initial state          |
| `process_thread_spawn_internal(pid, name, *tid)`                                 | Spawn with kernel entry (compatibility shim)      |
| `process_thread_spawn_worker_internal(pid, name, entry, arg, *tid)`              | Spawn kernel worker (runs C function on kstack)   |
| `process_thread_spawn_user_internal(pid, name, entry_rip, user_stack_top, *tid)` | Spawn user-mode ring3 thread                      |

User threads spawned via `WASMOS_SYSCALL_THREAD_CREATE` call
`process_thread_spawn_user_internal`, which initializes
`thread.ctx.rip = entry_rip`, `thread.ctx.cs = USER_CS_SELECTOR`,
`thread.ctx.user_rsp = user_stack_top & ~0xFULL`, and a dedicated kernel stack.

---

### Scheduler

Source: `src/kernel/process.c`

#### Ready Queue

Circular ring buffer: `g_ready_queue[THREAD_MAX_COUNT]` of TIDs.
`g_ready_head`, `g_ready_tail`, `g_ready_count` track state.
`thread.in_ready_queue = 1` prevents double-enqueue.

Invariant: only `THREAD_STATE_READY` threads may be enqueued.

#### Dispatch Sequence (`process_schedule_once_impl`)

1. Dequeue TID from ready queue; look up `thread_t`.
2. Find owner `process_t` via `thread->owner_pid`.
3. Canary check: compare `ctx_canary_pre` and `ctx_canary_post` against
   `PROCESS_CTX_CANARY_VALUE`; halt (`hlt` loop) on mismatch.
4. `process_set_running(proc, thread)` — state → `RUNNING`.
5. Reset `ticks_remaining = time_slice_ticks` if zero.
6. Copy thread ctx to `proc->ctx` (validates context).
7. `cpu_set_kernel_stack(proc->stack_top - 16u)` — updates `TSS.rsp0`.
8. Set `g_current_pid`, `g_current_process`, `g_current_thread`, `thread_set_current(tid)`.
9. Load `run_ctx->root_table = mm_context_root_table(proc->context_id)`.
10. Kernel workers: `process_run_worker_on_stack(proc, thread)`.
    Others: `context_switch_high(&g_sched_ctx, run_ctx)`.
11. On return: `g_current_process`, `g_current_thread` cleared; handle exit result.

#### Timer Preemption

`process_tick()` is called by the timer interrupt handler (PIT at ~100 Hz):
1. Increment `g_current_thread->ticks_total`.
2. Decrement `g_current_thread->ticks_remaining`.
3. At 0: set `g_need_resched = 1`. Log `[test] preempt ok` on first occurrence.
4. Stall watchdog: if `g_need_resched` is set for ≥ `SCHED_RESCHED_STALL_TICKS`
   consecutive ticks, emit `[watchdog] resched stall ticks=...`.

`process_preempt_from_irq(irq_frame_t *frame)` is called from the timer ISR when
CPL=3 at entry (`user_ss & 0x3u == 0x3u`). It copies `irq_frame_t` registers
into `g_current_thread->ctx`, then yields via `process_preempt_trampoline`.

---

### IPC Blocking and Wakeup

Source: `src/kernel/ipc.c`

Each IPC endpoint holds one `waiter_tid`:

```c
ep->waiter_tid = thread_current_tid();   /* on ipc_recv_for block */
```

On send/notify: `process_wake_thread(waiter_tid)` — targeted single-thread
wakeup. No context-wide wake fallback. If no thread is currently blocked, the
message remains queued for a later receiver.

Receiving multiple pending messages (notification endpoint) uses the same
`waiter_tid` slot; only one thread waits per endpoint at a time.

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
- Wake `join_waiter_tid` if non-zero.
- Decrement `proc->live_thread_count`.
- If other threads remain: find next ready thread and continue process.
- If last live thread: transition process to zombie, wake any process-level
  waiters.

**Process exit** (`PROCESS_RUN_EXITED`):
- `proc->exiting = 1`.
- All member threads marked toward exit.
- All blocked IPC/join/wait waiters are woken.
- Process reclaimed only after all threads reach `ZOMBIE`.

**Detached threads** (`thread.detached = 1`):
- `join_waiter_tid` cannot be set (join attempt returns error).
- On exit: thread slot is auto-reaped rather than held as zombie.

#### Join (`process_thread_join`)

- Self-join: denied.
- Already-detached target: denied.
- Only one joiner (`join_waiter_tid`) per target thread.
- If target already zombie: returns immediately with stored `exit_status`.
- Otherwise: caller blocks with `THREAD_BLOCK_WAIT_THREAD`.

#### Detach (`process_thread_detach`)

- Sets `thread.detached = 1`.
- If any joiner is already blocked: denied.
- If thread already zombie: auto-reap is triggered immediately.

#### Process Kill (`process_kill(pid, exit_status)`)

- Marks `proc->exiting = 1`, sets `exit_status`.
- Forces all live member threads toward exit.
- Wakes blocked process-level waiters (all matching waiters, not just first).
- `auto_reap = 1` processes are reaped once all threads are zombie and no
  waiters remain.

---

### Context Switch

Source: `context_switch.S` (assembly), `src/kernel/process.c`

`context_switch_high(from_ctx, to_ctx)` is the assembly trampoline:
1. Save all GPRs + rsp/rip/rflags/cs/ss into `from_ctx`.
2. Load GPRs + rsp/rip from `to_ctx`.
3. Switch CR3 if `to_ctx.root_table` differs from current.
4. Return to `to_ctx.rip`.

The syscall handler (`x86_syscall_handler`) copies the full trap frame into
`current_thread->ctx` before any blocking path, so resumed threads continue at
the correct post-syscall RIP.

---

### WASM Runtime Thread Mapping

Source: `src/kernel/wasm_driver.c`, `src/kernel/wasm3_shim.c`

Each WASM thread maps to a dedicated wasm3 VM instance:
- Separate `IM3Environment`, `IM3Runtime`, `IM3Module` per thread.
- No concurrent entry into the same VM instance.
- Thread-local heap: `wasm3_heap_bind_pid(pid)` keyed by PID, not TID;
  `WASM_DRIVER_THREAD_SLOTS = 64` slots for VM threads.
- `wasm_driver_spawn_vm_thread()` allocates a slot and spawns a kernel
  worker thread that creates a fresh wasm3 stack and calls the target WASM export.

Preemption is gated around all wasm3 interpreter entry points:
`preempt_disable()` before wasm3 state access, `preempt_enable()` after.
This prevents a timer interrupt from re-entering wasm3 interpreter state.

---

### Process Statistics (`ps` output)

Source: `src/kernel/include/process.h` (`process_stats_t`)

| Field                       | Meaning                                                        |
|-----------------------------|----------------------------------------------------------------|
| `state`                     | Current process state                                          |
| `block_reason`              | Current block reason                                           |
| `is_wasm`                   | 1 if process runs a WASM payload                               |
| `thread_count`              | Total threads spawned for this process                         |
| `live_thread_count`         | Non-zombie threads                                             |
| `current_tid`               | TID of last dispatched thread                                  |
| `context_id`                | MM context ID                                                  |
| `cpu_ticks`                 | Cumulative ticks from all threads                              |
| `vm_total_bytes`            | Virtual memory total                                           |
| `thread_kstack_total_bytes` | Sum of all thread kernel stack bytes                           |
| `heap_committed_bytes`      | Committed wasm3 heap across all process chunks                 |
| `rss_est_bytes`             | RSS estimate (equal to vm_total until per-page tracking lands) |

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
| `[test] threading join after kill order ok` | Join sees kill exit status deterministically      |
| `[test] threading join kill wake ok`        | Joiner wakes when target is killed while blocked  |

---

### Structural Invariants

1. **Thread slot is never scheduled after reclamation.** `THREAD_STATE_UNUSED`
   slots are never enqueued. Reclamation (`thread_reap`) clears `tid`, sets
   state to `UNUSED`.

2. **Process slot is reclaimed only after all threads are zombie.**
   `live_thread_count` tracks this; the process zombie transition is guarded by
   `live_thread_count == 0`.

3. **Capability checks are process-context-bound.** Thread identity does not
   grant additional capabilities. `capability_has(context_id, kind)` uses the
   shared process `context_id`.

4. **Endpoint ownership is by context_id, not TID.** A thread can only send
   to endpoints its process owns. IPC wake targets a specific TID but the
   endpoint remains owned by the process.

5. **Context canaries bracket `process_t.ctx`.** Any stack corruption that
   overwrites the context struct will be detected before the corrupted context
   is dispatched. Mismatch → `hlt` loop (no silent bad dispatch).

6. **TSS.rsp0 is updated on every dispatch.** `cpu_set_kernel_stack(stack_top - 16u)`
   is called in `process_schedule_once_impl` before `context_switch_high`.
   Ring3 interrupts and syscalls always land on the current thread's kernel stack.
