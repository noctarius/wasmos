# Current Task State (Preemptive Scheduling)

## Summary
Preemptive scheduling is implemented and stable. The IRQ0 timer tick drives time-slice accounting and preemption via a kernel trampoline that yields to the scheduler. Ready queue scheduling, idle task, and spinlock preemption guards are in place. Scheduler metrics (timer ticks, ready queue depth, current running PID) are exposed via wasm natives and shown in the CLI `ps` output. Process stacks now come from the physical frame allocator, and the kernel image range is reserved before allocating pages. Capacity limits were raised to allow CLI exec tests to spawn apps reliably.

## Current Behavior
- PIT timer IRQ0 updates ticks and triggers preemption.
- Preemption redirects the interrupted RIP to `process_preempt_trampoline`, which yields back to the scheduler.
- Ready queue is a ring buffer in `process.c`.
- Idle task runs `hlt` when no READY tasks exist.
- IPC queue operations are protected by spinlocks that disable preemption.
- QEMU test framework force-stops hung runs via the monitor sequence (`Ctrl+A` then `x`) on timeout.
- CLI `ps` shows scheduler metrics via wasm natives (`sched_ticks`, `sched_ready_count`, `sched_current_pid`).

## Tests (Last Run)
- `cmake --build build --target run-qemu-test` OK
- `cmake --build build --target run-qemu-cli-test` OK

## Key Files
- `src/kernel/process.c`
- `src/kernel/arch/x86_64/context_switch.S`
- `src/kernel/arch/x86_64/cpu_isr.S`
- `src/kernel/irq.c`
- `src/kernel/timer.c`
- `src/kernel/physmem.c`
- `src/kernel/arch/x86_64/linker.ld`
- `src/kernel/include/process.h`
- `src/kernel/include/memory.h`
- `PREEMPTIVE_SCHEDULING_DESIGN.md`
- `README.md`
- `ARCHITECTURE.md`

## Pending / Next Steps
- Add minimal scheduler metrics (e.g., expose ticks or run queue depth for diagnostics).
- Optional: reap short-lived test processes automatically to keep `ps` cleaner.
- Ensure documentation in `PREEMPTIVE_SCHEDULING_DESIGN.md`, `README.md`, and `ARCHITECTURE.md` stays aligned as features evolve.
