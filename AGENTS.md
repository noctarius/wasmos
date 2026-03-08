# Agents Guide

This repository uses Codex CLI to assist with development. Follow these conventions when working in this repo.

## Scope
- You are operating inside `/Volumes/git/wasmos`.
- The project is a minimal x86_64 UEFI boot + kernel scaffold intended to host a WASM runtime (WAMR).

## Always Do
- Read `README.md` and `ARCHITECTURE.md` at the start of a new task.
- Keep `README.md` and `ARCHITECTURE.md` updated with meaningful changes and new behaviors.
- Make small, focused changes that preserve the project’s minimalism.
- Prefer `rg` for searching and `cmake` for build orchestration.
- Use `clang`/`lld` for UEFI targets (AppleClang is insufficient).

## Never Do
- Do not introduce large frameworks or heavy dependencies.
- Do not add extra documentation files unless explicitly asked.
- Do not break the boot flow or kernel entry contract.
- NEVER modify code in `libs/wasm/wasm-micro-runtime` or in any other dependency imported via git subtree.

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
- Run QEMU: `cmake --build build --target run-qemu`
- Run QEMU halt test: `cmake --build build --target run-qemu-test` (default compile+boot+halt check after changes)

## Git
- Make a git commit after each prompt iteration when changes are made.
- Do not amend commits unless explicitly requested.
- `git add` and `git commit` are always allowed in this repository.
