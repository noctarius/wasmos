# Preemptive Scheduling Design (Draft)

This document describes a detailed design for adding preemptive multitasking and a timer-driven scheduler to WASMOS. It is intentionally separate from `ARCHITECTURE.md` and is meant to be reviewed and iterated before merging the concepts into the main architecture notes.

**Status**
Draft. No code changes yet.

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
- Cooperative scheduler: processes run until they yield, block, or exit.
- Blocking happens on IPC receive/wait.
- Timer interrupts are not used for preemption.
- Processes run in ring 0 with shared kernel address space.

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

**IRQ Path and Preemption**
- IRQ0 ISR must be minimal:
  - Acknowledge PIC.
  - Increment a global tick counter.
  - Update current process accounting.
  - Set `need_resched = 1`.
- Avoid doing scheduling directly in the ISR. Defer reschedule to a safe point.

**Preemption Control**
- Add a per-CPU `preempt_count` counter for nested preemption disables.
- Preemption is allowed only when:
  - `preempt_count == 0`
  - Not in a spinlock critical section
  - Not handling IPC queue manipulations
- Provide helpers:
  - `preempt_disable()`, `preempt_enable()`
  - `preempt_maybe_resched()` called at safe points

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

**Process Model Changes**
- Extend `process_t`:
  - `state` already exists (READY/BLOCKED/etc)
  - Add `time_slice_ticks`
  - Add `ticks_total`
  - Add `ticks_remaining`
  - Add `ctx` (saved register state)
- Track current process per CPU (single CPU for now).

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

**Blocking and Wakeup Semantics**
- IPC recv/wait:
  - Mark process BLOCKED and remove from run queue.
  - Reschedule immediately.
- IPC send:
  - If receiver is BLOCKED, set it READY and enqueue.
  - Receiver will run on next scheduling decision.
- Ensure wakeup is safe when called from interrupt context:
  - If in IRQ, only enqueue and set a resched flag.

**Kernel Main Loop**
- Replace cooperative loop:
  - Instead of polling `process_schedule_once`, the system relies on IRQ preemption.
  - The idle task runs when no READY processes exist.
- Idle task:
  - Executes `hlt` in a loop.
  - Must be preemptible by IRQ0.

**Interaction with WAMR and WASM Processes**
- WAMR execution is not aware of preemption.
- Preempting a WASM process is equivalent to preempting any kernel process.
- Ensure WAMR runtime state is per-process and does not assume uninterrupted execution.
- Avoid long critical sections inside WAMR host callbacks.

**IPC Path Considerations**
- IPC operations must be atomic with respect to preemption.
- Wrap IPC queue manipulation with both `preempt_disable` and spinlocks.
- IPC send/recv must remain lock-bounded to avoid priority inversion.

**Memory Management Implications**
- Context switch is designed for future per-process address spaces.
- Plan for `CR3` switching on context switch once user mode is introduced.
- Ensure kernel mappings remain shared and valid across address spaces.
- The scheduler must be aware of address space boundaries if/when introduced.

**User Mode and Privilege Separation (Future)**
- Preemption design should be compatible with ring-3:
  - Each process gets a user stack and kernel stack.
  - IRQ entry uses kernel stack.
  - Syscall path must be re-entrant under preemption.
- Add a privilege switch during context restore for user tasks.

**External Modules and Dependencies**
Changes required in external modules (WAMR):
- `libs/wasm/wasm-micro-runtime` should not be modified.
- WAMR usage in kernel must avoid global state that assumes single-threaded execution.
- Any WAMR host API calls invoked during IRQ must be avoided; IRQ path stays in kernel only.

Changes required in platform stubs:
- `src/wasm-micro-runtime/platform/wasmos/` must remain compatible.
- If any platform memory APIs assume non-preemptive execution, isolate them with locks.

**Files and Modules to Add**
- `src/kernel/timer.c` / `src/kernel/include/timer.h`
  - PIT init and tick accounting.
- `src/kernel/arch/x86_64/context_switch.S`
  - Low-level context switch.
- `src/kernel/include/scheduler.h`
  - Run queue operations and scheduling API.
- `src/kernel/scheduler.c`
  - Core scheduling logic.

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
- Running WAMR from IRQ context.
- Starvation if READY queue is mishandled.

**Validation Plan**
- Add a busy-loop WASM app and verify CLI responsiveness.
- Add a yield-free kernel task and verify time slicing.
- Confirm IPC wakeups still work under tick-based preemption.

**Open Questions**
- Preemption aggressiveness: start conservative (safe-point-only), then audit toward moderate if needed.

**Phased Implementation Plan**
1. Add PIT + IRQ0 tick counter; no preemption.
2. Add per-process `ticks_remaining` and `need_resched` logic.
3. Implement context switch and preemptive round-robin.
4. Convert kernel loop to idle task with timer-driven scheduling.
5. Audit IPC paths for preemption safety.
6. Add minimal scheduler metrics and diagnostics.
