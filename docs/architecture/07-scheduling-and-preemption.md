## Scheduling and Preemption

This document describes the WASMOS scheduler: the PIT clock, the priority-based
ready queues, per-thread state, the context-switch assembly, the preemption path,
and the guards that keep each phase safe.  The authoritative sources are
`src/kernel/process.c`, `src/kernel/sched_thread.c`,
`src/kernel/arch/x86_64/context_switch.S`, `src/kernel/timer.c`,
`src/kernel/spinlock.c`, `src/kernel/include/process.h`, and
`src/kernel/include/sched.h`.

---

### Overview

The scheduler is a preemptive, priority-based, single-core scheduler.
PIT channel 0 drives time-slice accounting.  When a running thread's quantum
expires the next IRQ0 fires the preemption path, which rewrites the interrupted
frame to redirect IRETQ into a scheduler trampoline.  The scheduler context then
picks the highest-priority ready thread and resumes it.  Blocking operations
(IPC wait, process wait, thread join, futex) suspend a thread without burning
quantum.

Threads are the unit of scheduling.  A process is purely a resource owner;
multiple threads of the same process may be simultaneously ready and run in
sequence.  The process-level READY/RUNNING/BLOCKED states of the legacy
scheduler are removed — only per-thread state drives dispatch.

---

### PIT Configuration

`timer_init(hz)` programs PIT channel 0 in square-wave mode (command byte `0x36`).

```
PIT_BASE_HZ = 1193182 Hz
divisor     = PIT_BASE_HZ / hz          (clamped to [1, 0xFFFF])
default hz  = 250 → divisor = 4772 → actual interval ≈ 4 ms per tick
```

At 250 Hz, `PROCESS_DEFAULT_SLICE_TICKS = 5` ticks gives each thread a 20 ms
quantum.

`timer_handle_irq()` increments `g_timer_ticks` and calls `process_tick()` on
every tick.  Heavy scheduling work stays out of the ISR body.

---

### Key Constants

| Constant                         | Value                | Notes                                                         |
|----------------------------------|----------------------|---------------------------------------------------------------|
| `PROCESS_DEFAULT_SLICE_TICKS`    | 5                    | Fixed quantum per thread (20 ms at 250 Hz)                    |
| `PROCESS_MAX_COUNT`              | 48                   | Fixed process table capacity                                  |
| `THREAD_MAX_COUNT`               | 128                  | Thread table capacity                                         |
| `PROCESS_STACK_SIZE`             | 524288               | 512 KB usable kernel stack per process/thread                 |
| `STACK_GUARD_PAGES`              | 1                    | Guard page count on each side of every stack                  |
| `STACK_REDZONE_BYTES`            | 4096                 | Margin from stack top to initial RSP                          |
| `STACK_CANARY_VALUE`             | `0xC0DEC0DEF00DFACE` | Written at base/mid/top of each stack                         |
| `PROCESS_CTX_CANARY_VALUE`       | `0xC0FFEE0DD15EA5E`  | Flanks `process_context_t` in `thread_t`                      |
| `SCHED_PRIO_MAX`                 | 7                    | Number of priority levels                                     |
| `SCHED_ANTISTARVATION_STREAK`    | 4                    | Max consecutive picks at one priority before forced lower run |
| `SCHED_PROGRESS_MARKER_SWITCHES` | 256                  | Logs `[test] sched progress ok` after this many switches      |
| `SCHED_RESCHED_STALL_TICKS`      | 512                  | Watchdog threshold: stall logged if resched pending this long |
| `SCHED_TRAMPOLINE_STACK_BYTES`   | 8192                 | Private scheduler-trampoline stack                            |

---

### Priority Model

Seven priority levels are defined in `src/kernel/include/sched.h`:

```c
typedef enum {
    SCHED_PRIO_REALTIME   = 0, /* IRQ handler workers, timer callbacks */
    SCHED_PRIO_DRIVER     = 1, /* native drivers (fbpci, serial, ata, kbd) */
    SCHED_PRIO_SERVICE    = 2, /* system services (device-manager, fs-manager) */
    SCHED_PRIO_SYSTEM     = 3, /* kernel services (process-manager, chardev) */
    SCHED_PRIO_WASM       = 4, /* default WASM processes */
    SCHED_PRIO_BACKGROUND = 5, /* batch / background jobs */
    SCHED_PRIO_IDLE       = 6, /* idle process only */
} sched_prio_t;
```

Default priority assigned at thread spawn:

| Process type                       | Default priority     |
|------------------------------------|----------------------|
| `is_idle == 1`                     | `SCHED_PRIO_IDLE`    |
| Native kernel worker               | `SCHED_PRIO_SYSTEM`  |
| WASM native driver (`FLAG_DRIVER`) | `SCHED_PRIO_DRIVER`  |
| WASM service (`FLAG_SERVICE`)      | `SCHED_PRIO_SERVICE` |
| WASM application                   | `SCHED_PRIO_WASM`    |

Priority is stored in `thread_t.sched_prio` (`uint8_t`).

---

### Per-CPU Scheduler State

Source: `src/kernel/include/sched.h`, `src/kernel/sched_thread.c`

```c
typedef struct {
    spinlock_t   lock;
    uint8_t      ready_bitmap;                /* bit i = 1 → ready_list[i] non-empty */
    list_head_t  ready_list[SCHED_PRIO_MAX];  /* one FIFO per priority */
    uint32_t     thread_count[SCHED_PRIO_MAX];
    uint32_t     nr_threads;
} cpu_sched_t;
```

A single `g_cpu_sched` instance covers the current single-core build.
`cpu_sched()` returns a pointer to it; cross-core paths are wired for SMP
extension but not currently active.

**O(1) find-highest-ready** via `ffs_table[128]`:

```c
static const uint8_t ffs_table[128] = { 0xFF, 0, 1, 0, 2, 0, 1, 0, ... };
/* ffs_table[bitmap] = index of lowest set bit (= highest-priority slot),
 * or 0xFF when bitmap == 0. */
```

This is equivalent to Minos2's `ffs_one_table[pcpu->local_rdy_grp]`.

**Enqueue** (`cpu_sched_enqueue`): sets the bitmap bit, appends to the tail of
`ready_list[prio]`.  Guards against double-enqueue by checking
`list_head_empty(&t->sched_node)`.

**Dequeue** (`cpu_sched_pick_next`): reads the bitmap, selects the highest
non-empty list, removes the head entry, clears the bitmap bit if the list
empties.  Returns the idle thread when the bitmap is zero.

**Anti-starvation**: `cpu_sched_pick_next` tracks a `streak` counter.  After
`SCHED_ANTISTARVATION_STREAK` consecutive picks from priority level N, the next
pick is forced from any non-empty level > N, preventing lower-priority threads
from starving permanently.

---

### Data Structures

#### `process_context_t`

Full register save frame.  Layout is verified by `_Static_assert`; the field
offsets are load-bearing in `context_switch.S`.

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

Every `thread_t` embeds this struct as a regular member. The per-thread
`ctx_canary_pre`/`ctx_canary_post` guard words live elsewhere in `thread_t`,
so they detect broader thread-slot corruption but do not physically flank the
`rip`/`rflags` words inside `ctx`.

#### `irq_frame_t`

The hardware-pushed exception/IRQ frame captured by the timer ISR stubs.

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
| `state`                 | `process_state_t`        | UNUSED / ALIVE / REAPING / ZOMBIE                     |
| `time_slice_ticks`      | `uint32_t`               | Unused at process level (per-thread ticks used)       |
| `is_idle`               | `uint8_t`                | Idle task marker                                      |
| `in_hostcall`           | `uint8_t`                | Set during wasm3 execution; blocks preemption         |
| `wasm3_lock`            | `spinlock_t`             | Reentrancy guard; held for duration of wasm3 entry    |
| `wasm3_owner`           | `uint32_t`               | TID currently executing in wasm3 (0 = free)           |
| `wait_event`            | `sched_event_t`          | Process-level wait event (child exit notification)    |

Process-level `ctx`, `ctx_canary_pre/post`, `in_ready_queue`, and
`block_reason` are removed; all scheduling state is per-thread.

#### `thread_t` (scheduling fields)

| Field                 | Type                    | Purpose                                               |
|-----------------------|-------------------------|-------------------------------------------------------|
| `tid`                 | `uint32_t`              | Thread identity                                       |
| `owner_pid`           | `uint32_t`              | Owning process                                        |
| `state`               | `thread_state_t`        | UNUSED / READY / RUNNING / BLOCKED / ZOMBIE           |
| `block_reason`        | `thread_block_reason_t` | NONE / IPC / WAIT_PROCESS / WAIT_THREAD / EVENT       |
| `is_kernel_worker`    | `uint8_t`               | Worker threads run directly on kstack; no trampoline  |
| `time_slice_ticks`    | `uint32_t`              | Per-thread quantum                                    |
| `ticks_remaining`     | `uint32_t`              | Ticks left in current quantum                         |
| `ticks_total`         | `uint64_t`              | Cumulative ticks consumed by this thread              |
| `ctx_canary_pre`      | `uint64_t`              | Per-thread corruption guard for the thread slot       |
| `ctx`                 | `process_context_t`     | Per-thread register save for all thread types         |
| `ctx_canary_post`     | `uint64_t`              | Per-thread corruption guard for the thread slot       |
| `sched_prio`          | `uint8_t`               | Priority 0–6 (lower = higher priority)                |
| `cpu_affinity`        | `uint32_t`              | Allowed CPU bitmask (currently `~0u`)                 |
| `last_cpu`            | `uint32_t`              | CPU where thread last ran                             |
| `sched_node`          | `list_head_t`           | Intrusive linkage in `cpu_sched_t.ready_list[prio]`   |
| `event_node`          | `list_head_t`           | Intrusive linkage in `sched_event_t.wait_list`        |
| `wait_event`          | `sched_event_t *`       | Event this thread is currently blocked on             |
| `pend_state`          | `uint32_t`              | `SCHED_PEND_OK / PEND_TIMEOUT / PEND_ABORT`           |
| `pend_data`           | `uint64_t`              | Data from waker (futex return value, etc.)            |
| `blocking_transition` | `uint8_t` (atomic)      | 1 while transitioning to BLOCKED; prevents early wake |
| `join_event`          | `sched_event_t`         | Embedded event for thread-join waiters                |
| `kstack_base/top`     | `uintptr_t`             | Thread-private kernel stack                           |

All kernel-worker threads now use `thread->ctx` for context save/restore; the
legacy `proc->ctx` shared slot is removed.

---

### Process and Thread States

```
              ┌──────────┐
   spawn      │  READY   │◄──────────────────────────────┐
  ──────────► └──────────┘                               │
                   │ schedule                    sched_wake_thread
                   ▼                                     │
              ┌─────────┐  yield/preempt       ┌──────────────────┐
              │ RUNNING │─────────────────────►│     BLOCKED      │
              └─────────┘  sched_event_wait    └──────────────────┘
                   │
                   │ exit / kill
                   ▼
               ┌────────┐
               │ ZOMBIE │──► join/auto-reap ──► UNUSED
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

Process states (`process_state_t`):

| State                    | Value | Meaning                              |
|--------------------------|-------|--------------------------------------|
| `PROCESS_STATE_UNUSED`   | 0     | Slot free                            |
| `PROCESS_STATE_ALIVE`    | 1     | At least one live thread             |
| `PROCESS_STATE_REAPING`  | 6     | Being reaped; slots still valid      |
| `PROCESS_STATE_ZOMBIE`   | 4     | All threads exited; awaiting reap    |

The READY/RUNNING/BLOCKED distinction is gone at the process level — only
thread state drives scheduling.

---

### Unified Event System

Source: `src/kernel/include/sched_event.h`, `src/kernel/sched_event.c`

`sched_event_t` is the single blocking primitive for all wait operations:

```c
typedef struct {
    spinlock_t          lock;
    list_head_t         wait_list;   /* thread_t.event_node members */
    uint32_t            cnt;
    sched_event_type_t  type;
} sched_event_t;
```

| Event type                 | Used by                                       |
|----------------------------|-----------------------------------------------|
| `SCHED_EVENT_TYPE_IPC`     | IPC endpoint blocking (ipc_recv_blocking_for) |
| `SCHED_EVENT_TYPE_JOIN`    | Thread join                                   |
| `SCHED_EVENT_TYPE_PROCESS` | Process-level wait (child exit)               |
| `SCHED_EVENT_TYPE_SELECT`  | Select-set multi-endpoint wait                |
| `SCHED_EVENT_TYPE_FUTEX`   | Futex wait                                    |

**Blocking a thread** (`sched_event_wait`):
- Sets `thread->blocking_transition = 1` (prevents premature wake before context save)
- Appends `thread->event_node` to `ev->wait_list`
- Calls `process_yield(PROCESS_RUN_BLOCKED)`
- On return from yield the `blocking_transition` handler has already cleared the flag

**Waking one waiter** (`sched_event_wake_one`):
- Removes the head entry from `ev->wait_list`
- Writes `pend_state` and `pend_data` to the woken thread
- Calls `sched_wake_thread(t)` which enqueues `t` back onto the priority queue

**`sched_wake_thread`** spin-waits on `blocking_transition` before enqueuing,
ensuring the sleeping CPU has finished its context save.

The `single-registration invariant`: a thread's `event_node` may only be in one
`wait_list` at a time.  Before adding to a new event, any existing membership
is removed.

---

### Scheduler Entry: `process_schedule_once`

The main scheduling loop calls `process_schedule_once` after each dispatch.

1. **Stack safety**: if RSP is below the higher-half base, execute on the
   dedicated 8 KB `g_sched_trampoline_stack` to avoid aliasing.
2. `cpu_sched_pick_next(cpu_sched())` → next `thread_t` (or idle thread).
3. Find owner `process_t` via `thread->owner_pid`.
4. Validate thread context canaries; halt on mismatch.
5. `process_set_running(proc, thread)`.
6. Refresh `ticks_remaining` if zero.
7. `cpu_set_kernel_stack(stack_top - 16)` → update TSS.rsp0.
8. Set `g_current_pid`, `g_current_process`, `g_current_thread`.
9. Load `root_table` from the process MM context.
10. **Kernel workers** (`is_kernel_worker`):
    - Snapshot callee-saved registers + RSP into `g_sched_ctx` so the worker's
      yield returns correctly.
    - If `thread->ctx.rsp == 0`: call `process_run_worker_on_stack(proc, thread)` (fresh thread).
    - If `thread->ctx.rsp != 0`: `context_switch_high(&g_sched_ctx, &thread->ctx)` (resume blocked worker).
11. **Other threads**: `context_switch_high(&g_sched_ctx, &thread->ctx)`.
12. On return, handle the run result:
    - `YIELDED` → re-enqueue on priority queue; clean up any stale event
      registrations from a non-blocking `ipc_recv_for` call.
    - `BLOCKED` → leave thread off the queue; `blocking_transition` flag was
      cleared by the BLOCKED handler.
    - `EXITED` → `process_mark_exited`; wake waiters; try auto-reap.
    - `THREAD_EXITED` → decrement live thread count; mark zombie or find next
      ready thread.

---

### Context Switch: `context_switch.S`

`context_switch_high(process_context_t *out, process_context_t *in)` saves all
15 GPRs plus RSP, RIP (synthesized as `lea 1f(%rip)`), RFLAGS, CS, SS, user_rsp,
and root_table into `*out`.  Then restores the same set from `*in`.

The final resume step branches on privilege level (`testb $0x3, cs`):

- **Ring0**: push RFLAGS + RIP on the kernel stack, `popfq` + `ret`.
- **Ring3**: construct a five-word `iretq` frame (user_ss, user_rsp, RFLAGS, CS,
  RIP), then `iretq`, restoring CPL3 state and user RSP.

The restore path must keep the incoming GPR set intact while it stages the
final frame. Scratch assembly must not reuse live restored registers such as
`%rdi`, `%rsi`, `%r8`, or `%r10` to hold `CS`, `SS`, `user_rsp`, `CR3`, or the
temporary resume stack pointer.

Both paths load `root_table` into CR3 before restoring GPRs.
`g_in_context_switch` is set at entry and cleared before `ret`/`iretq`.

`context_switch_to(process_context_t *in)` is the one-way variant (no save).
Used by `process_preempt_trampoline`.

---

### Process Trampoline

All non-worker threads start execution at `process_trampoline`:

1. Check stack canaries at base, mid, and top.  Halt on corruption.
2. `preempt_enable()` until `preempt_disable_depth() == 0`.
3. Call `entry(process, arg)`.
4. `context_switch_high(ctx, &g_sched_ctx)` to hand back to the scheduler.

---

### Preemption Path

**Step 1** — `timer_handle_irq()` → `process_tick()`: increments `ticks_total`,
decrements `ticks_remaining`, sets `g_need_resched = 1` at zero.

**Step 2** — `x86_timer_irq_handler(irq_frame_t *frame)` calls
`process_preempt_from_irq(frame)`.

**Step 3** — Gate checks.  Preemption is refused if any of the following:

| Guard                       | Condition                                     |
|-----------------------------|-----------------------------------------------|
| `g_in_scheduler`            | Scheduler is running                          |
| `g_in_context_switch`       | Context switch in flight                      |
| PM guard                    | PM outside `pm_preempt_safe_enter` scope      |
| `!process_should_resched()` | Quantum not yet expired                       |
| `!preempt_is_enabled()`     | Preemption disabled                           |
| Not RUNNING                 | Current thread not RUNNING                    |
| `in_hostcall`               | Inside wasm3 execution                        |
| Kernel-origin frame         | `cs & 0x3 == 0x0`                             |
| Invalid frame               | `rip == 0`, wrong selectors, missing user_rsp |

**Step 4** — Context capture into `g_current_thread->ctx`.

**Step 5** — Frame rewrite: overwrite frame `cs` → `KERNEL_CS`, `rip` →
`process_preempt_trampoline`.  Re-enqueue thread.  Clear `g_need_resched`.

**Step 6** — IRETQ jumps to `process_preempt_trampoline` in ring0.

**Step 7** — `process_preempt_trampoline` calls `context_switch_to(&g_sched_ctx)`,
returning to `process_schedule_once`.

---

### Preemption Disable and Spinlocks

**`g_preempt_disable_count`** — prevents scheduler preemption.  Does not disable
hardware interrupts.  Held for the entire wasm3 execution of a WASM process.

**`g_irq_disable_depth` + `g_irq_saved_flags`** — managed by spinlocks.
`spinlock_lock` saves RFLAGS, issues `cli`, increments the depth counter, calls
`preempt_disable()`.  `spinlock_unlock` reverses on the final release.

`preempt_safepoint()` — cooperative resched checkpoint: if `g_need_resched` is
set and preemption is enabled, calls `process_yield`.

`pm_preempt_safe_enter()` / `pm_preempt_safe_leave()` — PM-scoped guard allowing
selective preemption inside the process manager.

---

### Lock Hierarchy

Strict ordering to prevent deadlock.  Acquire from outermost to innermost:

```
cpu_sched_t.lock         (per-CPU ready queue)
  │
  └─► process_t.wasm3_lock   (WASM reentrancy guard, if needed)
        │
        └─► sched_event_t.lock   (event wait list)
              │
              └─► ipc_endpoint_t.lock  (message queue + poll_struct)
                    │
                    └─► futex_table_bucket.lock  (futex hash bucket, leaf)
```

Rules:
- Never acquire a coarser lock while holding a finer one.
- `cpu_sched_t.lock` acquired with IRQs saved (`spinlock_lock_irqsave`) from
  IRQ context.
- `sched_event_t.lock` is acquired before transitioning to BLOCKED and released
  before `process_yield`.
- Futex bucket lock is a leaf — nothing else acquired while it is held.

---

### Kernel Stack Layout

```
Physical layout: [guard_low][usable][guard_high]
  guard_low:  STACK_GUARD_PAGES × 4 KB (unmapped → #PF on underflow)
  usable:     PROCESS_STACK_SIZE bytes (512 KB)
  guard_high: STACK_GUARD_PAGES × 4 KB (unmapped → #PF on overflow)
```

Initial RSP on spawn: `stack_top - (STACK_REDZONE_BYTES + 8)`, 16-byte aligned.

Three canary words at base/mid/top; checked in `process_trampoline` on every
dispatch.  TSS.rsp0 updated on every dispatch.

---

### Thread Types

| Type          | `is_kernel_worker` | Entry                                  | Context       | Stack              |
|---------------|--------------------|----------------------------------------|---------------|--------------------|
| Main thread   | 0                  | via `process_trampoline`               | `thread->ctx` | `process_t` stack  |
| Kernel worker | 1                  | `process_run_worker_on_stack` directly | `thread->ctx` | dedicated `kstack` |
| User thread   | 0                  | via `iretq` to user RIP                | `thread->ctx` | dedicated `kstack` |

All thread types use `thread->ctx`.  The legacy `proc->ctx` shared context slot
is removed.

---

### Observability

- `[test] preempt ok` — first quantum expiry proves preemption works.
- `[test] sched progress ok` — 256 successful context switches (scheduler liveness).
- `[test] sched bitmap-ffs ok` — priority bitmap lookup table is correct.
- `[test] sched priority-ordering ok` — higher-priority threads preempt lower ones.
- `[test] sched dequeue ok` — dequeue returns correct thread.
- `[test] sched event-wake-one ok` — `sched_event_wake_one` wakes exactly one waiter.
- `[test] sched event-wake-all ok` — `sched_event_wake_all` wakes all waiters.
- `[test] sched default-prio ok` — `sched_default_prio` returns correct priority.
- `[test] sched sched-list ok` — intrusive list operations are correct.
- `[test] sched wake-during-block-transition ok` — `blocking_transition` guard prevents lost wakes.
- `[test] sched selftest all ok` — all scheduler selftests passed.
- `[watchdog] resched stall ticks=N` — `g_need_resched` pending without switch for too long.
- `[sched] stack canary tripped` — halt on corrupted stack canary.
- `[sched] ctx canary corrupt` — halt on corrupted context canary.
