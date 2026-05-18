# Architecture Notes

This document is the architecture index and entry point for the repository.
Detailed design and implementation status now live in focused documents under
`docs/architecture/`.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration, but do not use as a changelog.
IMPORTANT: Create a git commit after each prompt iteration.

## Current Status
- This file is now intentionally a concise index, not a release-changelog.
- Ring-3 strict isolation/hardening, threading phase rollout, DMA rollout,
  filesystem/PM service discovery, and CLI/runtime updates are tracked in the
  dedicated docs under `docs/architecture/`.
- CLI `ps` process diagnostics now also expose runtime kind (`wasm` true/false
  in table view and `wasm=true|false` annotations in tree view), sourced from
  process-manager spawn metadata.
- Threading is production-complete for the current single-core scope; final
  ABI/policy decisions and closure status are in
  `docs/architecture/15-threading-and-lifecycle.md` sections 15 and 17.
- Recent threading runtime hardening (user-thread kernel-stack setup for
  `THREAD_CREATE` and syscall frame/context synchronization for yield/block
  paths) is documented in `docs/architecture/15-threading-and-lifecycle.md`.
- Graphics/compositor Phase 0 scaffold (shared ABI constants and minimal
  native Zig `gfx-compositor` endpoint handshake path) is tracked in
  `docs/architecture/17-graphics-framebuffer-and-compositor.md`.
- Graphics/compositor baseline now also includes typed compositor opcode
  dispatch and minimal window lifecycle handling (`CREATE_WINDOW` /
  `DESTROY_WINDOW`) with owner-checked state slots and `GFX_STATUS_*` replies.
- Graphics/compositor baseline now also includes owner-checked
  `PRESENT_WINDOW` handling with a deterministic software fallback compose path
  (placeholder rectangle composition via framebuffer pixel API).
- Graphics/compositor baseline now also supports buffer-backed present in the
  fallback path (`PRESENT_WINDOW` shmem id), with full-frame redraw and
  damage-rect optimization deferred.
- Process-manager runtime bookkeeping now grows on demand (`apps`, `waits`,
  and `services` use internal linked-list pools), removing fixed small slot
  caps from PM-managed state.
- Kernel dynamic container baseline now includes a centralized `list`
  interface with selectable backends (linked vs growable array-chunk);
  process-manager list backend selection is wired through Kconfig.
- Higher-level components may use C++, while low-level kernel boundaries stay
  C/ASM. WASM C++ build policy is no exceptions/RTTI and explicit C ABI at
  integration points.
- Process-manager test injection hooks are now behind a dedicated Kconfig/CMake
  switch (`WASMOS_PM_TEST_HOOKS`) and are no-op when disabled.

## Architecture Document Map
- [Goals](architecture/01-goals.md)
- [Current System Summary](architecture/02-current-system-summary.md)
- [Architectural Direction](architecture/03-architectural-direction.md)
- [Boot and Handoff](architecture/04-boot-and-handoff.md)
- [Scheduling and Preemption](architecture/05-scheduling-and-preemption.md)
- [Process and IPC](architecture/06-process-and-ipc.md)
- [Memory Management](architecture/07-memory-management.md)
- [Runtime and Packaging](architecture/08-runtime-and-packaging.md)
- [Drivers and Services](architecture/09-drivers-and-services.md)
- [CLI and User-Space Baseline](architecture/10-cli-and-user-space.md)
- [Diagnostics and Status](architecture/11-diagnostics-status.md)
- [Repository Map and Validation Baseline](architecture/12-repo-map-and-validation.md)
- [Virtual Terminal](architecture/13-virtual-terminal.md)
- [Ring3 Isolation and Separation](architecture/14-ring3-isolation-and-separation.md)
- [Threading and Lifecycle](architecture/15-threading-and-lifecycle.md)
- [DMA Transfers](architecture/16-dma-transfers.md)
- [Graphics, Framebuffer, and Compositor](architecture/17-graphics-framebuffer-and-compositor.md)

## Update Rules
- Update the relevant feature document(s) in `docs/architecture/` when behavior
  changes.
- Keep cross-document references consistent across `README.md`, `docs/TASKS.md`, and
  architecture docs.
- Prefer appending concrete implementation notes over vague roadmap text.

## C++ Policy
- C++ is allowed for higher-level kernel subsystems, drivers, services, and apps.
- Keep boot/handoff, arch traps/interrupt paths, paging/MM primitives, and syscall/hostcall ABI boundaries in C/ASM.
- Enforce constrained C++ runtime usage for WASM modules:
  - `-fno-exceptions`
  - `-fno-rtti`
  - `-fno-threadsafe-statics`
  - `-fno-use-cxa-atexit`
- Maintain C ABI compatibility at subsystem boundaries with `extern "C"` declarations in shared headers.
