# Agents Guide

This repository uses Codex CLI to assist with development. Follow these conventions when working in this repo.

## Scope
- You are operating inside `/Volumes/git/wasmos`.
- The project is a minimal x86_64 UEFI boot + kernel scaffold intended to host a WASM runtime.
  Two backends are available: **wasm3** (default interpreter, `libs/wasm3`), **WARP**
  (single-pass JIT compiler, `libs/warp`, enabled with `-DWASMOS_WASM_RUNTIME_WARP=ON`), and
  **stb** (`libs/stb`).

## Skills
- `skills/` holds task-specific playbooks (each a `SKILL.md`). Consult the
  relevant one before starting that kind of task:
  - `skills/wasmos-add-hostcall` — add/change/retire a WASM host call
    (`abi/hostcalls.yaml` IDL + `scripts/gen_abi_hostcalls.py` regeneration +
    the hand-written kernel wrapper bodies).
  - `skills/wasmos-add-opcode` — add/change an IPC opcode (`abi/opcodes.yaml` IDL
    + `scripts/gen_abi_opcodes.py`; generated C enums + per-language constants).
  - `skills/wasmos-add-error` — add/change an error/status code or domain
    (`abi/errors.yaml` IDL + `scripts/gen_abi_errors.py`; transport axis vs packed
    `(domain, code)` + chain helpers).
  - `skills/wasmos-wasm-driver` — create and wire a new wasm device driver.
  - `skills/wasmos-system-service` — create and wire a new system service.
  - `skills/wasmos-system-util` — create a one-shot CLI utility under `src/utils/`.
  - `skills/wasmos-ide-targets` — keep CLion/clangd IDE coverage in sync (audit +
    fix "not in a project target").
  - `skills/wasmos-regression-test` — write and run the regression test for a
    bug (red before the fix, the `Regression:` marker, running/registering the
    host unit suites). Read it BEFORE writing any bug fix.
  - `skills/wasmos-integration-test` — add or change a QEMU integration test
    under `tests/` and assign it to a battery (`tests/batteries.json` is the
    single source for both the runner and the CI matrix; every test file must
    belong to exactly one battery).
  - `skills/wasmos-build-and-run` — build/run QEMU + test targets with the right
    runtime (wasm3 vs WARP) via Kconfig `.config` / `configs/*_defconfig`; the
    one-build-dir-per-config rule and why `-D` runtime flags can be overridden.
  - `skills/wasmos-kernel-internals` — kernel internals reference.
  - `skills/wasmos-shared-primitives` — the ownership/lifetime contract of a
    shared primitive (transfer buffers and DMA borrows, IPC endpoints, service
    classes, coroutines/futures, object tables) and which architecture document
    owns it. Read it BEFORE the first call to one, not when the task looks
    related: the contract you break is documented somewhere you had no reason
    to open.

## Always Do
- Read `README.md` and `docs/ARCHITECTURE.md` at the start of a new task.
- Keep `README.md` and `docs/ARCHITECTURE.md` updated with meaningful changes and new behaviors.
- Keep `docs/ARCHITECTURE.md` as an architecture index/entry point (stable structure, links, and guardrails), not a running implementation snapshot.
- Record current implementation/baseline changes in `docs/STATUS.md` (snapshot-style), not in `docs/ARCHITECTURE.md`.
- Keep `README.md` high-level and stable: do not append per-iteration or
  changelog-style feature bullets for each incremental step. `README.md`is user-facing (human).
  Updates should only be made if they are significant.
- Keep `libc` and its wrappers in sync, and keep `libsys` and its wrappers in
  sync across runtime-specific variants. Any API/behavior change in one side
  must be reflected in the corresponding wrapper/variant in the same change
  set.
- Add a short `TODO` or `FIXME` comment at the relevant source location when you
  identify a real known gap or deferred issue that is intentionally left in
  place.
- Make small, focused changes that preserve the project’s minimalism.
- Prefer `rg` for searching and `cmake` for build orchestration.
- Use `clang`/`lld` for UEFI targets (AppleClang is insufficient).
- Keep build logic in per-component `CMakeLists.txt` files (boot, kernel, drivers, services, and each example language).

## Never Do
- Do not introduce large frameworks or heavy dependencies.
- Do not add extra documentation files unless explicitly asked.
- Do not break the boot flow or kernel entry contract.
- NEVER modify code in `libs/wasm3` or `libs/warp` or in any other dependency imported via git subtree.
- NEVER return a bare `-1` as an error code on a subsystem boundary. Any value
  that leaves a subsystem — a host-call return, an IPC reply's code argument, or a
  status a peer decodes — must be a generated packed code from `abi/errors.yaml`
  (see `skills/wasmos-add-error`). Add a domain or a code if none fits; a
  deliberately unspecific code is not acceptable either, because it reintroduces
  exactly the ambiguity the packed model removes. Codes are negative, so return
  the constant directly (`return WASMOS_ERR_FS_NOT_FOUND;`) — never re-sign it.
  Bare `-1` stays acceptable only for internal helper returns that never cross a
  boundary; `scripts/quality.sh lint` reports those advisorily.
- Fix the bare `-1`s you walk past. Whenever you work in a service, driver, app,
  or the kernel, convert every boundary-crossing bare `-1` you see in the files
  you touch to a packed code from `abi/errors.yaml`, in the same change — not as
  a follow-up task. The backlog only shrinks if each visit leaves its files
  cleaner, and a `-1` left in place is one a later reader will copy. If a
  conversion needs a new domain or code, add it (`skills/wasmos-add-error`); if
  it would balloon the change beyond what one review can carry, say so in the
  commit message and leave a `TODO` naming the exact sites.
- NEVER call a shared primitive before reading the architecture document that
  owns its contract — transfer buffers and DMA borrows, IPC endpoints and select
  sets, service classes, coroutines and futures, object tables.
  `skills/wasmos-shared-primitives` maps each symbol to its document. The
  trigger is the SYMBOL you are about to type, not the subsystem you are
  editing: a primitive's contract lives with the primitive, which is a document
  no file in your change set points at. Reading the implementation is NOT a
  substitute — it says what the code does today, not what a caller is promised,
  and the two differ exactly where the contract is interesting. A doc that
  disagrees with the code is a `[DOCS]`/`[BUG]` entry for `docs/TASKS.md`, not
  permission to follow the code.
- NEVER design a cross-process interaction around the four IPC argument words.
  `ipc_message_t` carries exactly four opcode words, and that ceiling is not a
  budget to spend cleverly. A request that carries more than a couple of
  independent values, or any value that can grow (an LBA, a size, a string, a
  GUID, a list), goes in a request DESCRIPTOR in a transfer buffer the CLIENT
  owns — `arg0 = buffer_id, arg1 = offset, arg2 = size`. Bare arguments are for
  a fixed, small set that will not grow, and "will not grow" must be an argument
  you can make. Two tells that the message already outgrew its arguments: you
  are writing a shift or a mask into an argument (`(x << 12) | y`), or you are
  adding a sibling opcode that differs only in how a parameter is expressed.
  "It's the hot path" is not a reason — a client acquires and grants ONCE per
  operation and reuses both, so the per-request cost is one small write into an
  already-mapped buffer. See `skills/wasmos-add-opcode` §"Step 0".
- NEVER add bookkeeping to work around an error a primitive returns. A table,
  cache, retry, or special case introduced because a primitive "keeps failing"
  in a legitimate-looking way means you are holding it the wrong way round: stop
  and re-read its contract. `ALREADY_BORROWED` from a transfer buffer means the
  buffer is on the wrong side of the exchange — the CLIENT of a request owns it
  and the server is a transient grantee. A per-client grant table to route
  around that shipped once and had to be reverted.

## Code Style
- Keep C/ASM code minimal and explicit.
- Avoid unnecessary abstractions and macros.
- Prefer clarity over cleverness.

## Comments
- Write comments as reference documentation: state what the code is, what it
  guarantees, and which invariants or constraints a reader must respect. Present
  tense, declarative, no narrator.
- A comment describes the code as it stands now. It is not a diary, a changelog,
  or a record of how the code got here — git history holds that.
- Never write conversational or meta-conversational comments. Forbidden classes:
  - Change narration: `// now uses X instead of Y`, `// fixed the off-by-one`,
    `// this used to call foo()`, `// added in the ring3 migration`.
  - Address to a reader or to the agent's own reasoning: `// note that we ...`,
    `// as discussed`, `// let's ...`, `// I chose ...`, `// for clarity we ...`.
  - Review/self-assessment chatter: `// this is a bit hacky but works`,
    `// probably fine`, `// leaving this simple for now`.
  - Restating the statement below it: `// increment the counter` over `count++`.
- Explain the non-obvious: hardware/spec constraints, ordering requirements,
  ownership and lifetime rules, units, locking, error semantics. Cite the
  authority when one exists (spec section, ABI IDL in `abi/`, doc under `docs/`).
- Deferred work is a `TODO:` or `FIXME:` at the exact source location, phrased as
  the missing behavior and its consequence, not as commentary about the change
  being made.
- Rewrite comments that no longer match the code instead of leaving them beside
  it; a stale comment is a defect.

Do:
```c
/* Guest linear memory is reserved at a fixed VA and never relocated; only the
 * committed tail grows. Callers may cache pointers across a grow. */
```
Do not:
```c
/* We now reserve the VA up front instead of realloc'ing, which fixes the
 * dangling-pointer bug we hit earlier. Note that this is a bit subtle! */
```

## Boot Flow Reminder
- `BOOTX64.EFI` loads `kernel.elf` from the ESP, collects memory map, exits boot services, jumps to `_start`.
- `_start` prepares stack/BSS and calls `kmain(boot_info_t *)`.

## Build/Run Reminders
- Configure: `cmake -S . -B build`
- Build bootloader: `cmake --build build --target bootloader`
- Build kernel: `cmake --build build --target kernel`
- Build app packer: `cmake --build build --target make_wasmos_app`
- Run QEMU: `cmake --build build --target run-qemu`
- Run QEMU halt test: `cmake --build build --target run-qemu-test` (default compile+boot+halt check after changes)
- Quality gates: `cmake --build build --target fmt-check` and
  `cmake --build build --target lint` (or `quality` for both). Run them through
  the targets: the targets resolve the toolchain, and they build the wasmos
  clang-tidy plugin that the `wasmos-*` checks live in. `scripts/quality.sh`
  invoked directly now reads the same paths out of the build directory's
  CMakeCache, so it is equivalent when that directory is configured and built —
  but it warns and runs a weaker lint when the plugin is missing, and a warning
  is easy to miss. Prefer the targets.

## QEMU + GDB Debugging
- Use this default flow for non-ring3/kernel bring-up debugging.
- Prepare ESP + kernel artifacts:
  `cmake --build build --target run-qemu-test`
- Launch QEMU paused with gdbstub (default boot tree):
  `qemu-system-x86_64 -m 512M -serial mon:stdio -drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/share/qemu/edk2-x86_64-code.fd -nographic -drive format=raw,file=fat:rw:/Volumes/git/wasmos/build/esp -S -gdb tcp::1234`
- Attach GDB to symbols:
  `gdb -q build/kernel.elf`
  then run:
  `target remote :1234`
- Recommended early breakpoints (non-ring3):
  `b kmain`
  `b process_manager_entry`
  `b pm_handle_spawn_path`
  `b pm_fs_read_blob_for_spawn`
  `b wasmos_ipc_recv`
  `b x86_page_fault_handler`
  `b x86_exception_panic_frame`
- Core commands while debugging:
  `c`
  `bt`
  `info registers`
  `x/16i $rip`
  `p <expr>`
  `finish`
  `set pagination off`
- If reboot loops hide first fault, run QEMU with:
  `-no-reboot -d int,cpu_reset -D /tmp/qemu.log`
  then inspect `/tmp/qemu.log` for the first exception chain (for example `#PF -> #DF -> Triple fault`).
- Ring3-specific debug flow (when needed):
  - Prepare ring3 tree: `cmake --build build --target run-qemu-ring3-test`
  - Use `build/ring3/esp` + `build/ring3/kernel.elf` in the same paused QEMU/GDB flow above.

## Debug Playbook (High-Signal)
- Prefer this order when debugging boot/runtime failures:
  1. Reproduce with `cmake --build build --target run-qemu-test`
  2. Add minimal marker logs at one boundary only (caller or callee, not both everywhere)
  3. If cause is still unclear, switch to paused QEMU + GDB and inspect first failing transition
  4. Remove temporary debug logs after the fix is verified
- Keep debug edits small and reversible; avoid mixing refactors with diagnostics.
- If output volume explodes, remove/limit noisy markers immediately (log storms hide root cause and can change timing).

## Fast Failure Triage
- Spawn/service startup loops:
  - Breakpoints: `process_manager_entry`, `pm_handle_spawn_path`, `pm_fs_read_blob_for_spawn`, `pm_recv_fs_reply`
  - Check: request/reply IDs, `IPC_EMPTY` handling, and whether PM sends explicit error responses.
- FS relay/path issues:
  - Breakpoints: `wasmos_ipc_recv`, `wasmos_fs_buffer_copy`, `wasmos_fs_buffer_write`, `pm_fs_read_blob_for_spawn`
  - Check: who owns the active FS buffer context, borrow flags/source context, and peer-context clobbering during nested receives.
- Fault/triple-fault paths:
  - Breakpoints: `isr_exception_13`, `isr_exception_14`, `x86_page_fault_handler`, `x86_exception_panic_frame`
  - Use: `-no-reboot -d int,cpu_reset -D /tmp/qemu.log`.
- Scheduler/liveness stalls:
  - Breakpoints: `process_yield`, `process_block_on_ipc`, `ipc_recv_for`
  - Check: whether code is in a retry loop without a state transition or wake condition.

## GDB Command Snippets
- One-time setup:
  - `set pagination off`
  - `set confirm off`
  - `set print pretty on`
- Common inspections:
  - `bt`
  - `info registers`
  - `x/16i $rip`
  - `frame 0`
  - `p <expr>`
  - `x/s <ptr>`
  - `finish`
- Useful watch pattern for repeated failures:
  - Break at handler entry, print key args/fields, `continue`.
  - Example: inspect IPC message fields (`type`, `request_id`, `source`, `arg0..arg3`) at each hit.

## Marker Log Guidelines
- Prefix all temporary diagnostics with a short tag (for example `[dbg-spawn]`, `[dbg-fs]`).
- Log only fields that disambiguate state:
  - IDs (`pid`, `context_id`, `endpoint`, `request_id`)
  - operation (`type`)
  - one or two payload indicators (`len`, first bytes, status)
- Do not keep temporary markers in final commits unless explicitly requested.

## QEMU Session Hygiene
- Do not run integration QEMU targets in parallel.
- If a previous QEMU/GDB session may still exist, terminate it before reruns to avoid stale state/port conflicts.
- When a run behaves differently after many retries, reset to a clean single run with no extra markers and no parallel sessions.

## Artifact/Path Sanity Checks
- Verify expected files exist in ESP before deep debugging:
  - `build/esp/system/drivers`
  - `build/esp/system/services`
  - `build/esp/apps`
- For path-based spawn failures, verify both:
  - path string used by caller
  - actual ESP file path and filename form (including 8.3 compatibility constraints where relevant)

## Regression-Proofing After Fix
- After fixing, rerun:
  - `cmake --build build --target run-qemu-test`
- Confirm both:
  - the original failure is gone
  - no new boot-stage regressions appear later in the startup chain

## Git
- Make a git commit after each prompt iteration when changes are made.
- Do not amend commits unless explicitly requested.
- `git add` and `git commit` are always allowed in this repository.
- Commit messages must always be detailed (clear summary + meaningful body).
- Commits created by agents must include a `Co-authored-by:` trailer.
- ALWAYS run `cmake --build build --target run-qemu-test` before staging and committing changes.
- ALWAYS run unit tests, especially newly created tests, and also the full existing unit test suite, before declaring work complete.
- Unit/integration test targets MUST NOT be started in parallel (for example,
  do not run `run-qemu-test` and `run-qemu-cli-test` at the same time). They
  share mutable `build/esp` artifacts and parallel runs can cause flaky
  failures like `Error deleting` and boot-config corruption. CI runs the test
  batteries concurrently only because each runner has its own filesystem; that
  is a cross-machine property and does not make local parallel runs safe.
- Every file under `tests/` must belong to exactly one battery in
  `tests/batteries.json`, or it never runs. See
  `skills/wasmos-integration-test`.

## Testing Policy
- Every bug fix owes a regression test, and the test comes FIRST. The order is
  binding, not stylistic: a test written after the fix never demonstrated that it
  can fail, so it may assert nothing. See `skills/wasmos-regression-test`.
  1. Reproduce the bug and understand its mechanism.
  2. Write the failing test, carrying a `Regression:` marker in its comment — a
     GitHub issue number (`Regression: #142`) or, when there is none, a
     date-based identifier (`Regression: 2026-08-17-readdir-terminator`). State
     the failure and what it cost, not the fix.
  3. Run it against the UNFIXED tree and confirm it is red for the reason the bug
     describes. A test that fails to compile, or fails on a missing stub, has
     demonstrated nothing.
  4. Only then write the fix, and only a fix that turns those tests green.
- When a bug genuinely cannot be reproduced on the host — real SMP ordering, a
  context switch, hardware timing — test the nearest observable contract instead.
  If even that is impossible, say so plainly in the commit message and
  `docs/TASKS.md` along with what you verified instead (boot counts, sweep
  sizes). Never fake a test, and never weaken a real one until it passes.
- Valid unit tests MUST verify runtime behavior, outputs, state transitions, or API contracts.
- Unit tests MUST NOT use source-text presence assertions (for example regex/string matching
  against repository files to check whether specific words, sentences, or lines still exist).
  These tests are invalid because they are brittle and do not verify behavior.
- Changes that do NOT affect runtime behavior do not require test execution. This includes
  comment-only edits, typo fixes, pure documentation updates, symbol/function renames without
  semantic changes, formatting-only changes, and other refactors that preserve behavior.
- Do not add or extend source-text presence tests; existing ones are temporary and should be
  cleaned up/replaced over time.
