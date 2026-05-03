# Agents Guide

This repository uses Codex CLI to assist with development. Follow these conventions when working in this repo.

## Scope
- You are operating inside `/Volumes/git/wasmos`.
- The project is a minimal x86_64 UEFI boot + kernel scaffold intended to host a WASM runtime (wasm3).

## Always Do
- Read `README.md` and `ARCHITECTURE.md` at the start of a new task.
- Keep `README.md` and `ARCHITECTURE.md` updated with meaningful changes and new behaviors.
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
- Build the ring3 debug tree first: `cmake --build build --target run-qemu-ring3-test` (or any target that prepares `build/ring3/esp` and `build/ring3/kernel.elf`).
- Launch QEMU paused with gdbstub:
  `qemu-system-x86_64 -m 512M -serial mon:stdio -drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/share/qemu/edk2-x86_64-code.fd -nographic -drive format=raw,file=fat:rw:/Volumes/git/wasmos/build/ring3/esp -S -gdb tcp::1234`
- Attach GDB to symbols:
  `gdb -q build/ring3/kernel.elf`
  then run:
  `target remote :1234`
- Typical breakpoints for ring3 isolation faults:
  `b isr_exception_13`
  `b isr_exception_14`
  `b x86_page_fault_handler`
  `b x86_exception_panic_frame`
  `b x86_syscall_handler`
- Continue with `c`, inspect with `info registers`, `x/16i $rip`, and `bt`.
- If reboot loops hide the first fatal fault, run QEMU with `-no-reboot -d int,cpu_reset -D /tmp/qemu-ring3.log` and inspect exception order in `/tmp/qemu-ring3.log` (for example `#PF -> #DF -> Triple fault`).

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
