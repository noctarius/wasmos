# WAMR Debug Status (Preemptive Branch)

This document summarizes the current WAMR debugging findings on the preemptive
multitasking branch and the runtime instrumentation changes that were added.

## Current Issue
Preemptive runs now reach the expected bytecode prologues (no more `IMPDEP`
opcode as the "first opcode"), but the system still hangs after `sysinit`
returns, and the QEMU halt test never reaches the CLI prompt. We now need to
determine whether the hang is scheduler/process-state related rather than a
WAMR bytecode entry issue.

Additional observation: after fixing the packer flags, PM logs `sysinit` as
`flags=0x2` (SERVICE), but the entry call still returns immediately without any
sysinit logs appearing. This suggests the WAMR entry still exits after a single
opcode without executing the sysinit loop or native calls.

Quick probe: `native-call-min` (debug_mark + console_write only) is now spawned
before `native-call-smoke`. It still returns with `native call count=0`, so the
minimal native import path remains unexecuted in preemptive mode.

Interpreter opcode trace now shows `exec opcodes=0xCF` for both `native-call`
apps and `sysinit`, confirming that only the IMPDEP glue opcode is executed
before returning.

## Hypotheses Under Consideration
- Preemption/timer interaction disrupting interpreter state
- Incorrect frame/IP state when switching into the interpreter
- Module/code buffer residency or mapping issues (code buffer not consistently
  reachable from the process context)

## Runtime Instrumentation Added
The WAMR classic interpreter and the kernel WAMR wrapper were instrumented to
expose execution state around the interpreter dispatch loop.

### Files Touched
- `libs/wasm/wamr_runtime.c`
- `libs/wasm/wasm-micro-runtime/core/iwasm/interpreter/wasm_interp_classic.c`

### Added Trace Data
- Last native function pointer/index and native call count
- Bytecode call count, `call_indirect` count, and last call-indirect target
- Code start/end addresses for the current function
- Last frame instruction pointer and function pointer
- Entry call begin/end logs with `ok` result and exception state
- Executed opcode trace (first 16 opcode bytes observed by interpreter dispatch)
- Captured opcode bytes at code start
- Captured "first opcode" bytes + `ip`/`ip_end` recorded at interpreter entry

### Notes
- This instrumentation touches `libs/wasm/wasm-micro-runtime`, which is normally
  avoided, but was done for debugging.
- The same instrumentation is intended to be applied to the cooperative branch
  to compare call paths and trace output.

## Observed Runtime Output (Preemptive)
Latest trace (2026-03-13) shows correct bytecode entry:
- `first opcode ip` matches `code start`
- `first opcode bytes=23 00 41 10 6B CA ...` (expected prologue)
- `impdep hits=1` with `impdep ip=0`
- `opcode exec count=1` (expected for tiny smoke apps)
- `native-call-smoke` and `init-smoke` both return OK

Despite the correct bytecode entry, the system hangs after `sysinit` returns
and only `[timer] ticks` continue printing.

## Cooperative vs Preemptive Trace Comparison (2026-03-13)
Cooperative (main branch) shows the interpreter entering real bytecode directly:
- `first opcode ip` matches `code start`
- `first opcode bytes` match the code prologue (example: `41 81 20 41 00 3A 00 00`
  or `23 00 41 40 6A CA 00 24`)
- `opcodes bytes` match `first opcode bytes`

Preemptive (preemptive-clean branch) now shows the same entry pattern:
- `first opcode ip` matches `code start`
- `first opcode bytes` match the real code start bytes (`23 00 41 ...`)

## IMPDEP Handoff Trace
Additional instrumentation records the IMPDEP transition:
- `impdep hits=1` per entry call
- `impdep ip` recorded as `0x0` in cooperative runs (before the real code start)
- `impdep sp` recorded as a valid stack address

This suggests the interpreter is now starting from the IMPDEP glue frame and
transitioning correctly in preemptive mode as well. The remaining hang likely
lies in scheduler/process state after `sysinit` exits.

## Additional Debugging Steps
- Compare cooperative vs preemptive trace output with identical instrumentation.
- Confirm post-sysinit process state and whether any runnable tasks remain
  (CLI or hw-discovery) after `sysinit` returns.
- Investigate whether the frame `ip` is being set/cleared incorrectly at or
  after the IMPDEP glue frame.
