# WAMR Debug Status (Preemptive Branch)

This document summarizes the current WAMR debugging findings on the preemptive
multitasking branch and the runtime instrumentation changes that were added.

## Current Issue
WAMR instances launch and return successfully, but WASM bytecode appears to
execute only a single opcode and never reaches native imports such as
`console_write` or `debug_mark`. This is visible in the runtime trace output:
- `native call count` stays at `0`
- `opcode exec count` is `1`
- `last frame ip` stays `0`
- opcode bytes captured from the code buffer show the expected prologue
  (`23 00 41 ...`), but execution does not proceed beyond the initial glue frame

The trace also shows the first opcode captured as `0xCF` (WAMR's `IMPDEP`
glue-frame opcode), which indicates the interpreter is still observing the
call-entry trampoline rather than the first real bytecode instruction.

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
- Captured opcode bytes at code start
- Captured "first opcode" bytes + `ip`/`ip_end` recorded at interpreter entry

### Notes
- This instrumentation touches `libs/wasm/wasm-micro-runtime`, which is normally
  avoided, but was done for debugging.
- The same instrumentation is intended to be applied to the cooperative branch
  to compare call paths and trace output.

## Observed Runtime Output (Preemptive)
Example pattern seen in logs:
- `first opcode bytes=CF` (IMPDEP glue)
- `opcodes bytes=23 00 41 ...` (real code start)
- `native call count=0`
- `opcode exec count=1`
- `last frame ip=0`

## Additional Debugging Steps
- Compare cooperative vs preemptive trace output with identical instrumentation.
- Confirm the interpreter reaches real bytecode instructions in cooperative
  mode and whether `first opcode` changes from `CF` to the expected prologue.
- Investigate whether the frame `ip` is being set/cleared incorrectly at or
  after the IMPDEP glue frame.
