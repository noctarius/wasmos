# Preemptive Scheduling Design (Draft)

This document describes a detailed design for adding preemptive multitasking and a timer-driven scheduler to WASMOS. It is intentionally separate from `ARCHITECTURE.md` and is meant to be reviewed and iterated before merging the concepts into the main architecture notes.

**Status**
Implemented and tested in-tree. See “Current State” and “Phased Implementation Plan” for what is complete vs pending.

**Scope**
- Introduce preemptive scheduling with periodic timer interrupts.
- Preserve existing IPC semantics and process lifecycle model.
- Keep the kernel minimal; avoid adding heavy policy mechanisms.
- Provide a roadmap for later ring-3 user mode integration.

**Non-Goals**
- Priority inheritance, CPU affinity, or multi-core scheduling.
- Full real-time scheduling guarantees.
- Kernel debugging/tracing infrastructure beyond minimal counters.

**Current Baseline**
- Preemptive round-robin scheduling is enabled.
- Timer interrupts (PIT/IRQ0) drive time-slice accounting and preemption.
- Blocking happens on IPC receive/wait; wakeups enqueue READY processes.
- Processes run in ring 0 with shared kernel address space.

**Current State (Implementation)**
- PIT timer implemented in `src/kernel/timer.c` / `src/kernel/include/timer.h`.
- IRQ0 handler updates tick accounting and invokes preemption logic.
- Ready queue is a fixed-size ring buffer in `process.c`.
- Per-process context (`process_context_t`) holds full GPRs + RIP/RSP/RFLAGS.
- Context switch implemented in `src/kernel/arch/x86_64/context_switch.S`.
- Timer preemption uses an IRQ preempt trampoline (`process_preempt_trampoline`) that yields to the scheduler.
- Spinlocks disable preemption while held.
- Scheduler metrics (timer ticks, ready queue depth, current running PID) are exposed via wasm natives and surfaced in `ps`.
- Idle task exists and runs `hlt` when no READY tasks are available.
- Process stacks are allocated from the physical frame allocator (not static BSS).
- Kernel image range is reserved from the physical frame allocator at boot.
- Process/context caps raised: `PROCESS_MAX_COUNT=32`, `MM_MAX_CONTEXTS=32`.
- Tests: `run-qemu-test` and `run-qemu-cli-test` pass.

**Design Principles**
- Determinism and minimalism first: keep the preemption core small.
- Explicit state transitions: READY/RUNNING/BLOCKED/ZOMBIE.
- Decouple mechanism (preemption) from policy (later).
- Ensure IPC wakeups remain reliable under preemption.

**Timer Source**
- Use PIT (IRQ0) initially, given current PIC setup.
- Configure a fixed tick rate (250 Hz default, 4ms).
- Make the tick rate a build-time knob (e.g., `-DTICK_HZ=250`).
- Provide a `timer_init(hz)` that programs PIT channel 0.
- Hook IRQ0 handler into IDT with `IRQ_TIMER_VECTOR` after PIC remap.
- Keep PIT configuration in `src/kernel/timer.c` and `src/kernel/include/timer.h`.
**State**: Implemented, tick rate fixed at 250 Hz default.

**IRQ Path and Preemption**
- IRQ0 ISR must be minimal:
  - Acknowledge PIC.
  - Increment a global tick counter.
  - Update current process accounting.
  - Set `need_resched = 1`.
- Avoid doing scheduling directly in the ISR. Defer reschedule to a safe point.
**State**: Implemented. IRQ0 updates tick accounting and preempt logic; preemption redirects to a trampoline that yields to the scheduler.

**Preemption Control**
- Add a per-CPU `preempt_count` counter for nested preemption disables.
- Preemption is allowed only when:
  - `preempt_count == 0`
  - Not in a spinlock critical section
  - Not handling IPC queue manipulations
- Provide helpers:
  - `preempt_disable()`, `preempt_enable()`
  - `preempt_maybe_resched()` called at safe points
**State**: Implemented `preempt_disable`/`preempt_enable` and guards in spinlocks. Safe-point helper not added; preemption is timer-driven via trampoline.

**Context Switching**
- Introduce a saved CPU context struct per process:
  - General registers: `RAX..R15`
  - Instruction pointer `RIP`
  - Stack pointer `RSP`
  - `RFLAGS`
  - Optional: `FS/GS` base if used later
- Add ASM routines:
  - `context_save(ctx_t *out)`
  - `context_restore(ctx_t *in)`
  - Or a single `context_switch(old, new)` that saves and restores
- Place ASM in `src/kernel/arch/x86_64/context_switch.S`.
- Ensure interrupt state is consistent:
  - Switch with interrupts disabled
  - Restore `RFLAGS` for the new task
**State**: Implemented with a single `context_switch`.

**Process Model Changes**
- Extend `process_t`:
  - `state` already exists (READY/BLOCKED/etc)
  - Add `time_slice_ticks`
  - Add `ticks_total`
  - Add `ticks_remaining`
  - Add `ctx` (saved register state)
- Track current process per CPU (single CPU for now).
**State**: Implemented. `process_t` carries tick accounting and saved context.

**Run Queue**
- Add a ready queue data structure:
  - Linked list now; evolve to multi-queue when priorities are added.
  - Operations: `enqueue_ready`, `dequeue_ready`.
- Use spinlocks or preemption disable to guard queue operations.
- A process transitions:
  - READY: in run queue.
  - RUNNING: current.
  - BLOCKED: removed from run queue.
  - ZOMBIE: removed from run queue.

**Scheduling Policy (Initial)**
- Round-robin with fixed quantum.
- On each timer tick:
  - Decrement `ticks_remaining` of current process.
  - If `ticks_remaining == 0`, set `need_resched`.
- On reschedule:
  - Move current RUNNING to READY (if still runnable).
  - Pop next READY from queue.
  - Reset `ticks_remaining` to `time_slice_ticks`.
**State**: Implemented round-robin with fixed time slices.

**Blocking and Wakeup Semantics**
- IPC recv/wait:
  - Mark process BLOCKED and remove from run queue.
  - Reschedule immediately.
- IPC send:
  - If receiver is BLOCKED, set it READY and enqueue.
  - Receiver will run on next scheduling decision.
- Ensure wakeup is safe when called from interrupt context:
  - If in IRQ, only enqueue and set a resched flag.
**State**: Implemented. Wakeups enqueue READY tasks; preemption scheduling occurs via the trampoline.

**Kernel Main Loop**
- Replace cooperative loop:
  - Instead of polling `process_schedule_once`, the system relies on IRQ preemption.
  - The idle task runs when no READY processes exist.
- Idle task:
  - Executes `hlt` in a loop.
  - Must be preemptible by IRQ0.
**State**: Implemented idle task and scheduler loop; idle executes `hlt`.

**Interaction with wasm3 and WASM Processes**
- wasm3 execution is not aware of preemption.
- Preempting a WASM process is equivalent to preempting any kernel process.
- Ensure runtime state is per-process and does not assume uninterrupted execution.
- Avoid long critical sections inside runtime host callbacks.

**IPC Path Considerations**
- IPC operations must be atomic with respect to preemption.
- Wrap IPC queue manipulation with both `preempt_disable` and spinlocks.
- IPC send/recv must remain lock-bounded to avoid priority inversion.
**State**: Implemented with spinlocks disabling preemption around IPC queue manipulation.

**Memory Management Implications**
- Context switch is designed for future per-process address spaces.
- Plan for `CR3` switching on context switch once user mode is introduced.
- Ensure kernel mappings remain shared and valid across address spaces.
- The scheduler must be aware of address space boundaries if/when introduced.
**State**: Kernel reserves its image range in the physical frame allocator and allocates process stacks from PFA. No per-process address spaces yet.

**User Mode and Privilege Separation (Future)**
- Preemption design should be compatible with ring-3:
  - Each process gets a user stack and kernel stack.
  - IRQ entry uses kernel stack.
  - Syscall path must be re-entrant under preemption.
- Add a privilege switch during context restore for user tasks.

**External Modules and Dependencies**
Changes required in external modules (wasm3):
- `libs/wasm/wasm3` should not be modified.
- wasm3 usage in kernel must avoid global state that assumes single-threaded execution.
- Any wasm runtime host API calls invoked during IRQ must be avoided; IRQ path stays in kernel only.

**Files and Modules to Add**
- `src/kernel/timer.c` / `src/kernel/include/timer.h`
  - PIT init and tick accounting.
- `src/kernel/arch/x86_64/context_switch.S`
  - Low-level context switch.
- `src/kernel/include/scheduler.h`
  - Run queue operations and scheduling API.
- `src/kernel/scheduler.c`
  - Core scheduling logic.
**State**: Not added. Scheduling logic resides in `process.c` for now.

**Existing Files to Modify**
- `src/kernel/kernel.c`
  - Initialize timer and scheduler.
  - Replace cooperative loop with idle loop.
- `src/kernel/process.c`
  - Add per-process context, time slice, and state transitions.
- `src/kernel/ipc.c`
  - Guard IPC queues under preemption.
- `src/kernel/irq.c`
  - Add IRQ0 handler for timer tick and reschedule path.
**State**: Implemented.

**State Machine: Process Lifecycle**
- NEW -> READY: inserted in run queue.
- READY -> RUNNING: selected by scheduler.
- RUNNING -> READY: time slice expired.
- RUNNING -> BLOCKED: IPC wait or explicit sleep.
- BLOCKED -> READY: IPC wakeup.
- RUNNING -> ZOMBIE: exit.
- ZOMBIE -> FREE: reaped by parent/PM.

**Tick Accounting**
- Global: `ticks_total`.
- Per-process: `ticks_total`, `ticks_remaining`.
- Expose `ticks_total` to `ps` output later (optional).

**Locking Rules**
- Scheduler queue manipulation must be atomic:
  - Use `spinlock` or `preempt_disable`.
- IPC wakeup from IRQ must not allocate memory.
- Context switch must run with interrupts disabled.

**Failure Modes to Avoid**
- Preempting while holding spinlocks: deadlock or corruption.
- Scheduling from IRQ while another core (future) is modifying queues.
- Running wasm runtime from IRQ context.
- Starvation if READY queue is mishandled.

**Validation Plan**
- Add a busy-loop WASM app and verify CLI responsiveness.
- Add a yield-free kernel task and verify time slicing.
- Confirm IPC wakeups still work under tick-based preemption.
**State**: Implemented via kernel preemption smoke test plus IPC wakeup and timer tick tests.

**Open Questions**
- Preemption aggressiveness: start conservative (safe-point-only), then audit toward moderate if needed.

**Phased Implementation Plan**
1. Add PIT + IRQ0 tick counter; no preemption. **Done**
2. Add per-process `ticks_remaining` and `need_resched` logic. **Done**
3. Implement context switch and preemptive round-robin. **Done**
4. Convert kernel loop to idle task with timer-driven scheduling. **Done**
5. Audit IPC paths for preemption safety. **Done (spinlock-preempt guard)**
6. Add minimal scheduler metrics and diagnostics. **Pending**
**State**: Phases 1-5 are complete; phase 6 remains pending.
