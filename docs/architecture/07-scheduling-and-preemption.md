## Scheduling and Preemption

This document describes the WASMOS scheduler in full implementation detail: the
PIT clock, the ready queue, per-process and per-thread state, the context-switch
assembly, the preemption path, and the guards that keep each of those phases
safe. The authoritative sources are `src/kernel/process.c`,
`src/kernel/arch/x86_64/context_switch.S`, `src/kernel/timer.c`,
`src/kernel/spinlock.c`, and `src/kernel/include/process.h`.

---

### Overview

The scheduler is preemptive round-robin, single-core. PIT channel 0 drives
time-slice accounting at a fixed rate. When a running thread's quantum expires,
the next IRQ0 fires the preemption path, which rewrites the interrupted frame to
redirect return-from-interrupt into a scheduler trampoline. The scheduler
context then picks the next ready thread and resumes it. Blocking operations
(IPC wait, process wait, thread join) suspend a thread without burning quantum.

---

### PIT Configuration

`timer_init(hz)` programs PIT channel 0 in square-wave mode (command byte `0x36`).

```
PIT_BASE_HZ = 1193182 Hz
divisor     = PIT_BASE_HZ / hz          (clamped to [1, 0xFFFF])
default hz  = 250 → divisor = 4772 → actual interval ≈ 4 ms per tick
```

At 250 Hz, `PROCESS_DEFAULT_SLICE_TICKS = 5` ticks gives each thread a
20 ms quantum.

`timer_handle_irq()` increments `g_timer_ticks` and calls `process_tick()` on
every tick. Heavy scheduling work stays out of the ISR body.

`timer_ms_to_ticks(ms)` converts milliseconds to tick counts:

```c
(ms * g_timer_hz + 999) / 1000
```

---

### Key Constants

| Constant                         | Value                | Notes                                                         |
|----------------------------------|----------------------|---------------------------------------------------------------|
| `PROCESS_DEFAULT_SLICE_TICKS`    | 5                    | Fixed quantum per thread (20 ms at 250 Hz)                    |
| `PROCESS_MAX_COUNT`              | 48                   | Fixed process table capacity                                  |
| `THREAD_MAX_COUNT`               | 128                  | Ready-queue capacity and thread table cap                     |
| `PROCESS_STACK_SIZE`             | 524288               | 512 KB usable kernel stack per process/thread                 |
| `STACK_GUARD_PAGES`              | 1                    | Guard page count on each side of every stack                  |
| `STACK_REDZONE_BYTES`            | 4096                 | Margin from stack top to initial RSP                          |
| `STACK_CANARY_VALUE`             | `0xC0DEC0DEF00DFACE` | Written at base/mid/top of each stack                         |
| `PROCESS_CTX_CANARY_VALUE`       | `0xC0FFEE0DD15EA5E`  | Flanks `process_context_t` in `process_t`                     |
| `SCHED_PROGRESS_MARKER_SWITCHES` | 256                  | Logs `[test] sched progress ok` after this many switches      |
| `SCHED_RESCHED_STALL_TICKS`      | 512                  | Watchdog threshold: stall logged if resched pending this long |
| `SCHED_TRAMPOLINE_STACK_BYTES`   | 8192                 | Private scheduler-trampoline stack                            |

---

### Data Structures

#### `process_context_t`

Full register save frame. Layout is verified by `_Static_assert` at compile
time; the field offsets are load-bearing in `context_switch.S`.

```c
typedef struct {
    uint64_t r15;        // offset 0
    uint64_t r14;        // 8
    uint64_t r13;        // 16
    uint64_t r12;        // 24
    uint64_t r11;        // 32
    uint64_t r10;        // 40
    uint64_t r9;         // 48
    uint64_t r8;         // 56
    uint64_t rdi;        // 64
    uint64_t rsi;        // 72
    uint64_t rbp;        // 80
    uint64_t rdx;        // 88
    uint64_t rcx;        // 96
    uint64_t rbx;        // 104
    uint64_t rax;        // 112
    uint64_t rsp;        // 120  ← kernel stack pointer
    uint64_t rip;        // 128
    uint64_t rflags;     // 136
    uint64_t cs;         // 144  ← KERNEL_CS (0x08) or USER_CS (0x1B)
    uint64_t ss;         // 152  ← KERNEL_DS (0x10) or USER_DS (0x23)
    uint64_t user_rsp;   // 160  ← user-mode stack pointer (ring3 contexts)
    uint64_t root_table; // 168  ← physical CR3 value for this context
} process_context_t;
```

Every `process_t` embeds this struct between two 64-bit canary words
(`ctx_canary_pre`/`ctx_canary_post`) initialized to `PROCESS_CTX_CANARY_VALUE`.
The canaries are checked at every context-validation callsite.

#### `irq_frame_t`

The hardware-pushed exception/IRQ frame captured by the timer ISR stubs. The
timer preemption path reads this to snapshot the interrupted thread state.

```c
typedef struct {
    uint64_t r15;       // offset 0   (pushed by ISR stub)
    ...
    uint64_t rax;       // 112
    uint64_t rip;       // 120  (pushed by CPU)
    uint64_t cs;        // 128
    uint64_t rflags;    // 136
    uint64_t user_rsp;  // 144  (pushed by CPU only for CPL change)
    uint64_t user_ss;   // 152
} irq_frame_t;
```

#### `process_t` (scheduling fields)

| Field                   | Type                     | Purpose                                               |
|-------------------------|--------------------------|-------------------------------------------------------|
| `state`                 | `process_state_t`        | UNUSED / READY / RUNNING / BLOCKED / ZOMBIE           |
| `block_reason`          | `process_block_reason_t` | NONE / IPC / WAIT                                     |
| `time_slice_ticks`      | `uint32_t`               | Quantum (default `PROCESS_DEFAULT_SLICE_TICKS`)       |
| `ticks_remaining`       | `uint32_t`               | Ticks left in current quantum                         |
| `ticks_total`           | `uint64_t`               | Cumulative ticks consumed by this process             |
| `in_ready_queue`        | `uint8_t`                | Prevents double-enqueue                               |
| `is_idle`               | `uint8_t`                | Idle task; never in normal ready queue                |
| `in_hostcall`           | `uint8_t`                | Set while inside wasm3 execution; blocks preemption   |
| `ctx`                   | `process_context_t`      | Saved register state (snapshot of running thread ctx) |
| `ctx_canary_pre/post`   | `uint64_t`               | Corruption detection flanking ctx                     |
| `stack_base/top`        | `uintptr_t`              | Kernel stack virtual address range                    |
| `stack_alloc_base_phys` | `uintptr_t`              | Physical base for stack reclaim on reap               |

#### `thread_t` (scheduling fields)

Threads are the unit of scheduling. Each process has at least one main thread;
additional kernel-worker and user threads can be added at runtime.

| Field              | Type                    | Purpose                                              |
|--------------------|-------------------------|------------------------------------------------------|
| `tid`              | `uint32_t`              | Thread identity; used in ready queue                 |
| `owner_pid`        | `uint32_t`              | Owning process                                       |
| `state`            | `thread_state_t`        | UNUSED / READY / RUNNING / BLOCKED / ZOMBIE          |
| `block_reason`     | `thread_block_reason_t` | NONE / IPC / WAIT_PROCESS / WAIT_THREAD              |
| `in_ready_queue`   | `uint8_t`               | Prevents double-enqueue                              |
| `is_kernel_worker` | `uint8_t`               | Worker threads run directly on kstack; no trampoline |
| `time_slice_ticks` | `uint32_t`              | Per-thread quantum                                   |
| `ticks_remaining`  | `uint32_t`              | Ticks left in current quantum                        |
| `ticks_total`      | `uint64_t`              | Cumulative ticks consumed by this thread             |
| `ctx`              | `process_context_t`     | Per-thread register save (non-worker threads)        |
| `kstack_base/top`  | `uintptr_t`             | Thread-private kernel stack                          |
| `join_waiter_tid`  | `uint32_t`              | Thread waiting on this thread's exit                 |

---

### Process and Thread States

```
                  ┌──────────┐
   spawn          │  READY   │◄────────────────────────────────┐
  ──────────►     └──────────┘                                 │
                      │ schedule                        wake (IPC/wait)
                      ▼                                        │
                  ┌─────────┐  yield/preempt            ┌──────────────┐
                  │ RUNNING │──────────────────────────►│   BLOCKED    │
                  └─────────┘  block_on_ipc / wait      └──────────────┘
                      │
                      │ exit / kill
                      ▼
                  ┌────────┐
                  │ ZOMBIE │──► reap ──► UNUSED
                  └────────┘
```

`process_run_result_t` values returned from the process entry function:

| Value                       | Meaning                                  |
|-----------------------------|------------------------------------------|
| `PROCESS_RUN_YIELDED`       | Cooperative yield; re-enqueue            |
| `PROCESS_RUN_IDLE`          | Nothing to do; idle fallback             |
| `PROCESS_RUN_BLOCKED`       | Caller blocked; park with block reason   |
| `PROCESS_RUN_EXITED`        | Process exit; mark zombie                |
| `PROCESS_RUN_THREAD_EXITED` | Worker thread exit; decrement live count |

---

### Ready Queue

The ready queue is a FIFO circular ring buffer of `THREAD_MAX_COUNT` (128) slots
holding thread IDs. State is maintained in three globals: `g_ready_queue[]`,
`g_ready_head`, `g_ready_tail`, `g_ready_count`.

**Enqueue** (`ready_queue_enqueue`): rejects threads not in `THREAD_STATE_READY`,
rejects idle process, rejects already-queued threads. Advances tail.

**Dequeue** (`ready_queue_dequeue`): advances head, validates that the dequeued
thread is still READY and its owner process is still READY. Stale entries from
killed/reaped owners are silently dropped.

The idle process (`g_idle_process`) is excluded from the queue. The scheduler
falls back to it only when `ready_queue_dequeue` returns nothing.

---

### Scheduler Entry: `process_schedule_once`

The main scheduling loop calls `process_schedule_once` after each process
yields. The function has two entry variants:

1. **Normal path**: if the current RSP is already in the higher-half kernel
   window, call `process_schedule_once_impl` directly.
2. **Bootstrap safety path**: if RSP is below the higher-half base (early
   boot or low-stack path), execute `process_schedule_once_impl` on the
   dedicated 8 KB `g_sched_trampoline_stack` to avoid stack aliasing with
   user-space address ranges.

`process_schedule_once_impl` does:
1. `ready_queue_dequeue()` → thread + owner process.
2. Fall back to idle process if queue empty.
3. Validate context canaries.
4. `process_set_running(proc, thread)`: sets both process and thread to RUNNING.
5. Refresh `ticks_remaining` if zero; assert non-zero slice.
6. `cpu_set_kernel_stack(stack_top - 16)` → update TSS.rsp0 so ring3
   interrupt/syscall entries land on the right kernel stack.
7. `context_switch_high(&g_sched_ctx, run_ctx)` → save scheduler context,
   restore process context and resume.
8. On return from the switch, handle the run result:
   - `PROCESS_RUN_YIELDED` → `process_set_ready` + re-enqueue.
   - `PROCESS_RUN_BLOCKED` → leave thread blocked; enqueue next owner thread
     if one is ready.
   - `PROCESS_RUN_EXITED` → `process_mark_exited`; wake waiters; try auto-reap.
   - `PROCESS_RUN_THREAD_EXITED` → decrement live thread count; mark zombie or
     enqueue next owner thread.

---

### Context Switch: `context_switch.S`

`context_switch(process_context_t *out, process_context_t *in)` saves all 15
GPRs plus RSP, RIP (synthesized as `lea 1f(%rip)` so the restored context
resumes just after the call), RFLAGS, CS, SS, user_rsp, and root_table into
`*out`. Then it restores the same set from `*in`.

The final resume step has two branches depending on the target privilege level,
detected by `testb $0x3, cs`:

- **Ring0 (kernel) resume**: push RFLAGS + RIP on the new kernel stack, then
  `popfq` + `ret`. The `ret` pops the synthesized RIP and resumes.
- **Ring3 (user) resume**: push user_ss, user_rsp, RFLAGS, CS, RIP as a
  five-word `iretq` frame on the kernel stack, then `iretq`. This correctly
  restores CPL3 state and loads the user RSP.

Both paths load `root_table` into CR3 before restoring GPRs. `g_in_context_switch`
is set to `1` at entry and cleared to `0` before the final `ret`/`iretq`; this
flag prevents re-entrant preemption during an in-flight switch.

`context_switch_to(process_context_t *in)` is the one-way variant: no save,
just restore and resume. Used by `process_preempt_trampoline` to re-enter the
scheduler context from a preempted user process.

The `process_preempt_trampoline` symbol:

```asm
process_preempt_trampoline:
    leaq g_sched_ctx(%rip), %rdi
    jmp context_switch_to
```

---

### Process Trampoline

All processes (except kernel-worker threads) start execution at
`process_trampoline`. On each scheduler dispatch it:

1. Checks stack canaries at base, mid, and top. Halts with a diagnostic if any
   are tripped.
2. Calls `preempt_enable()` until `preempt_disable_depth() == 0` (clears any
   stale preempt counts from a previous slice).
3. Calls `entry(process, arg)`.
4. On return, calls `context_switch_high(ctx, &g_sched_ctx)` to hand back to
   the scheduler, carrying the run result in `g_last_run_result`.

This pattern means every process re-enters through the trampoline after every
context switch; the stack unwinds cleanly on each dispatch.

---

### Preemption Path

**Step 1 — Tick accounting.**
`timer_handle_irq()` → `process_tick()`. This increments the running thread's
`ticks_total`, decrements `ticks_remaining`, and sets `g_need_resched = 1` when
`ticks_remaining` reaches zero. Logs `[test] preempt ok` on the first expiry.

**Step 2 — IRQ handler.**
`x86_timer_irq_handler(irq_frame_t *frame)` receives the full register frame
pushed by the timer ISR stub. It calls `process_preempt_from_irq(frame)`.

**Step 3 — Preemption gate checks.** `process_preempt_from_irq` refuses to
preempt if any of the following is true:

| Guard | Condition |
|---|---|
| `g_in_scheduler` | Scheduler is running |
| `g_in_context_switch` | Context switch in flight |
| PM guard | `process-manager` process with `pm_preempt_safe_depth == 0` |
| `!process_should_resched()` | Quantum not yet expired |
| `!preempt_is_enabled()` | Preemption disabled (`g_preempt_disable_count > 0`) |
| Not RUNNING | Current process is not in RUNNING state |
| `in_hostcall` | Inside wasm3 execution (WASM hostcall in progress) |
| Kernel-origin frame | `cs & 0x3 == 0x0` — kernel code cannot be preempted |
| Invalid frame | `rip == 0`, wrong selectors, `user_rsp == 0` for CPL3 |

If all checks pass and the frame originates from user space (CPL3):

**Step 4 — Context capture.** All fields from `irq_frame_t` are copied into
`g_current_process->ctx` and the current thread's `ctx`: all 15 GPRs, cs,
user_rsp, ss, rip, rflags.

**Step 5 — Frame rewrite.** The frame's `cs` is overwritten with
`KERNEL_CS_SELECTOR` and `rip` is overwritten with the address of
`process_preempt_trampoline`. The process is marked READY and re-enqueued.
`g_need_resched` is cleared.

**Step 6 — IRETQ redirect.** When the IRQ handler returns with `iretq`, the
modified frame causes the CPU to jump to `process_preempt_trampoline` in ring0
on the kernel stack — not back into the user process.

**Step 7 — Scheduler re-entry.** `process_preempt_trampoline` calls
`context_switch_to(&g_sched_ctx)`, which restores the scheduler context and
returns control to `process_schedule_once`.

---

### Preemption Disable and Spinlocks

Two independent depth counters manage preemption and interrupt delivery:

**`g_preempt_disable_count`** — prevents the scheduler from preempting the
current thread. Incremented by `preempt_disable()` / `critical_section_enter()`,
decremented by `preempt_enable()` / `critical_section_leave()`. Does not
disable hardware interrupts. The wasm3 execution wrapper holds preempt_disable
for the entire WASM execution of a process to prevent mid-execution preemption
while the interpreter is in an unknown state.

**`g_irq_disable_depth` + `g_irq_saved_flags`** — managed by spinlocks only.
`spinlock_lock` saves RFLAGS and issues `cli`, increments the depth counter,
and calls `preempt_disable()`. `spinlock_unlock` releases the lock, calls
`preempt_enable()`, and restores RFLAGS on the final unlock (re-enabling
interrupts if they were enabled before). Spinlocks are always held with IF=0,
preventing the IRQ deadlock that would otherwise occur when `ipc_send_from`
(called from IRQ context) tries to acquire a lock held by an interrupted
`ipc_recv_for` caller.

`preempt_safepoint()` is a cooperative checkpoint: if `g_need_resched` is set
and preemption is enabled, it clears the flag and calls `process_yield`. Called
by long-running kernel paths that want to be responsive without waiting for the
next IRQ.

`pm_preempt_safe_enter()` / `pm_preempt_safe_leave()` are a process-manager
scoped guard (`g_pm_preempt_safe_depth`). The PM holds this around sections that
can tolerate preemption; outside those sections the PM is not preemptible even
if `g_preempt_disable_count == 0`.

---

### Kernel Stack Layout

Every process and thread gets an independent kernel stack with guard pages:

```
Physical layout: [guard_low][usable][guard_high]
  guard_low:  STACK_GUARD_PAGES × 4 KB (unmapped → #PF on underflow)
  usable:     PROCESS_STACK_SIZE bytes (512 KB)
  guard_high: STACK_GUARD_PAGES × 4 KB (unmapped → #PF on overflow)
```

All stacks are allocated in the higher-half window
(`pfa_alloc_pages_below(pages, 512 MiB)`) so they are reachable under kernel
CR3 without any additional mapping. Stack overruns into guard pages produce
deterministic `#PF` exceptions rather than silent corruption.

Initial RSP on spawn: `stack_top - (STACK_REDZONE_BYTES + 8)`, 16-byte aligned.

Three canary words are written at allocation time:
- `*stack_base = STACK_CANARY_VALUE`
- `*stack_mid = STACK_CANARY_VALUE`
- `*(stack_top - 8) = STACK_CANARY_VALUE`

These are checked in `process_trampoline` before every process entry and would
trap before returning into the scheduler if overwritten.

TSS.rsp0 is updated by `cpu_set_kernel_stack(stack_top - 16)` on every dispatch
so CPL3→CPL0 transitions (syscalls and hardware exceptions) always land on the
correct process kernel stack.

---

### Thread Types

| Type          | `is_kernel_worker` | Entry                                  | Context                 | Stack              |
|---------------|--------------------|----------------------------------------|-------------------------|--------------------|
| Main thread   | 0                  | via `process_trampoline`               | `thread->ctx`           | `process_t` stack  |
| Kernel worker | 1                  | `process_run_worker_on_stack` directly | `proc->ctx`             | dedicated `kstack` |
| User thread   | 0                  | via `iretq` to user RIP                | `thread->ctx` (cs=0x1B) | dedicated `kstack` |

`process_sched_ctx_for_thread` returns `proc->ctx` for worker threads and
`thread->ctx` for all others. The scheduler always dispatches threads, not
processes: `ready_queue_enqueue`/`dequeue` operate on `tid`.

---

### Observability

- `[test] preempt ok` — logged once on first quantum expiry (first proof that
  preemption is working).
- `[test] sched progress ok` — logged once after 256 context switches
  (scheduler liveness proof).
- `[watchdog] resched stall ticks=N` — logged every `SCHED_RESCHED_STALL_TICKS`
  ticks when `g_need_resched` remains set without a switch. Increments
  `g_resched_stall_reports`.
- `[watchdog] trap frame invalid` — logged when `process_preempt_from_irq` sees
  a malformed IRQ frame. Increments `g_trap_frame_invalid_reports`.
- `[sched] stack canary tripped` — logged and halted (`cli; hlt`) when
  `process_trampoline` detects a corrupted canary.
- `[sched] ctx canary corrupt` — logged and halted when `process_validate_context`
  detects a corrupted `ctx_canary_pre` or `ctx_canary_post`.
- `[sched] invariant fail` — logged and halted on `process_sched_invariant_fail`
  for structural consistency errors (null pointers, owner mismatch, zero slice).

`process_watchdog_issue_count()` returns `g_resched_stall_reports + g_trap_frame_invalid_reports`
for test harness assertions.

---

### What Is Not Implemented

- **Priorities or budgets.** All threads get the same fixed quantum.
- **Per-CPU scheduling.** The ready queue and all scheduler state are global,
  single-core. SMP requires per-CPU run queues, scheduler locks, and IPI-based
  remote wakeup — deferred until single-core model is fully exercised.
- **Kernel preemption.** The preemption path only fires on CPL3 frames.
  Kernel-originated frames are skipped; the kernel runs to completion unless
  it cooperatively yields.
- **APIC/IOAPIC.** The timer clock is PIT-based using the legacy PIC. APIC
  support is a deferred item listed in the known gaps.
- **CPU accounting and scheduling metrics.** `ticks_total` is tracked per
  thread and surfaced through `process_stats_t`, but there is no per-CPU
  utilization budget, load tracking, or latency instrumentation.
