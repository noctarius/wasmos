# Architecture Notes

This document is the architecture index and entry point for the repository.
Detailed design and implementation status now live in focused documents under
`docs/architecture/`.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration, but do not use as a changelog.
IMPORTANT: Create a git commit after each prompt iteration.

Status snapshot has been moved to [STATUS.md](STATUS.md).

Ring3 smoke validation note: `run-qemu-ring3-test` configures process-manager
test injection hooks so owner-deny marker checks (`wait`/`kill`/`status`/`spawn`)
remain deterministic in strict ring3 runs.

Current baseline note: `fs-manager` no longer relies on a fixed-size client
slot table; it grows client state in heap-backed chunks.
Process-manager context buffer tracking for filesystem/framebuffer borrows is
now list-backed instead of fixed `PROCESS_MAX_COUNT` arrays.
Kernel list internals now include an early-boot static-arena allocator fallback
so list-backed modules can initialize before general heap allocation is ready.
MM context registration and capability state tracking are now list-backed, so
context growth is no longer bounded by static `MM_MAX_CONTEXTS` slot arrays.
Per-context memory-region storage is now also list-backed, removing the fixed
`MM_MAX_REGIONS` array limit within each context.
WASM `libui` component-tree state is now heap-backed (dynamic component, text,
and list-item storage) instead of fixed compile-time slot/text/item caps, and
includes first shared form controls such as list views and dropdowns; text
rendering is wired through `font-service` as a required dependency.
Compositor pointer delivery to focused clients uses content-local coordinates,
and window client buffers render in the content pane below chrome/titlebar.
WASM libc now implements a process-local linear-memory allocator
(`malloc/free/calloc/realloc`) backed by `memory.grow`.

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

## Shared Service/Driver Helpers
- `src/libsys` is split by runtime:
  - `src/libsys/wasm` for wasm-compiled apps/services/drivers (hostcall-backed C helpers).
  - `src/libsys/native` for native services/drivers (C exports plus Zig wrappers).
- Scope is intentionally narrow and explicit: common IPC wait/call patterns, IPC send-retry flow-control helpers, name pack/unpack helpers, buffer borrow/release helpers, filesystem path-read helpers, libc-style string/ctype adapters, and small shared primitives such as SHA-256.
- Keep `libsys` lightweight and dependency-free; it should reduce duplicated control-flow/error handling without hiding protocol behavior.
