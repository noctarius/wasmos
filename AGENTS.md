# Agents Guide

This repository uses Codex CLI to assist with development. Follow these conventions when working in this repo.

## Scope
- You are operating inside `/Volumes/git/wasmos`.
- The project is a minimal x86_64 UEFI boot + kernel scaffold intended to host a WASM runtime (wasm3).

## Always Do
- Read `README.md` and `docs/ARCHITECTURE.md` at the start of a new task.
- Keep `README.md` and `docs/ARCHITECTURE.md` updated with meaningful changes and new behaviors.
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
- NEVER modify code in `libs/wasm/wasm3` or in any other dependency imported via git subtree.

## Code Style
- Keep C/ASM code minimal and explicit.
- Avoid unnecessary abstractions and macros.
- Prefer clarity over cleverness.

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
  failures like `Error deleting` and boot-config corruption.

## Testing Policy
- Valid unit tests MUST verify runtime behavior, outputs, state transitions, or API contracts.
- Unit tests MUST NOT use source-text presence assertions (for example regex/string matching
  against repository files to check whether specific words, sentences, or lines still exist).
  These tests are invalid because they are brittle and do not verify behavior.
- Changes that do NOT affect runtime behavior do not require test execution. This includes
  comment-only edits, typo fixes, pure documentation updates, symbol/function renames without
  semantic changes, formatting-only changes, and other refactors that preserve behavior.
- Do not add or extend source-text presence tests; existing ones are temporary and should be
  cleaned up/replaced over time.
