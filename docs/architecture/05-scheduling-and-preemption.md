## Scheduling and Preemption

### Current Design
The scheduler is fully preemptive round-robin:
- PIT IRQ0 drives time-slice accounting.
- The default PIT rate is 250 Hz.
- The ready queue is FIFO.
- Each runnable process gets a fixed quantum
  (`PROCESS_DEFAULT_SLICE_TICKS` in `src/kernel/include/process.h`).
- An explicit idle task runs `hlt` whenever no process is ready.

### Implemented Preemption Path
The preemptive scheduling design that previously lived in a separate draft is
now the baseline architecture:
- `src/kernel/timer.c` programs PIT channel 0 and tracks global ticks.
- The IRQ0 handler increments tick accounting and triggers preemption logic.
- The kernel does not perform a full scheduler run inside the ISR.
  Instead, timer preemption rewrites the interrupted RIP to
  `process_preempt_trampoline`, which returns into the normal scheduler path.
- Per-process state includes saved register context, total tick accounting, and
  remaining time slice.
- Context switching is implemented in
  `src/kernel/arch/x86_64/context_switch.S`.
- Spinlocks disable preemption while held to keep critical regions short and
  consistent.

### Preemption Safety Rules
- Never preempt while a spinlock is held.
- Never perform heavy scheduling work directly in the interrupt handler.
- IPC queue mutations must remain atomic under preemption.
- Long host calls must mark themselves as non-preemptible if interrupting them
  would break wakeup or ownership invariants.

### Current Safe Points and Special Cases
- IPC receive host calls mark the current process as inside a host call so the
  empty-to-block transition cannot race against wakeups.
- If a blocked process is woken during that transition, the scheduler preserves
  the wakeup instead of forcing the process back to `BLOCKED`.
- The CLI calls `sched_yield` while polling for user input so other processes
  continue to make progress even when the shell is idle.

### What Is Still Missing
The preemptive core is implemented, but the following are still future work:
- Priorities or budgets.
- Per-CPU scheduling.
- User-mode context switching with kernel/user stack separation.
- Richer scheduling metrics and latency instrumentation.

