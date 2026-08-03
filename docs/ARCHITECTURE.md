# Architecture Notes

This document is the architecture index and entry point for the repository.
Detailed design and implementation status now live in focused documents under
`docs/architecture/`.

## Documentation Status Labels

Architecture documents use the following labels immediately below their title:

- **Implemented reference**: describes behavior present in the source tree.
- **Mixed reference and proposal**: distinguishes existing mechanisms from
  explicitly labelled future work.
- **Design proposal**: describes an unimplemented design; it is not evidence of
  runtime behavior.

Implementation claims must name their source locations or validation path.
Future work must remain under a `Future Work`, `Deferred Work`, `Rollout Plan`,
or equivalent heading. Use `STATUS.md` for the concise current snapshot and
`TASKS.md` for unfinished work.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration, but do not use as a changelog.
IMPORTANT: Create a git commit after each prompt iteration.

Status snapshot has been moved to [STATUS.md](STATUS.md).

Transfer-buffer terminology note: the architectural abstraction is a generic
**transfer buffer**. It is used for file payloads, spawn payloads, broker
plans, packet data, and other bulk transfers; feature docs should describe the
generic transfer-buffer contract unless a filesystem-specific behavior is
actually meant.

Ring3 smoke validation note: `run-qemu-ring3-test` configures process-manager
test injection hooks so owner-deny marker checks (`wait`/`kill`/`status`/`spawn`)
remain deterministic in strict ring3 runs.
WARP runtime note: selecting the WARP backend now implies the ring-3 execution
model; non-ring3 WARP is no longer a supported configuration mode. Ring-3 is the
standard execution model, not a configuration choice: do not add new non-ring3
execution paths, ring0-only code paths, or mode toggles. Remaining ring-0 native
services are to be moved onto the ring-3 native path, not extended in place.
Scheduler note: the thread-centric scheduler described in
`architecture/29-threadable-scheduler.md` is now the only scheduler model.
The filename is retained for continuity, but it documents the current
in-kernel scheduler rather than an optional mode.

## Architecture Document Map

### Context and Goals
- [Goals](architecture/01-goals.md)
- [Current System Summary](architecture/02-current-system-summary.md)
- [Architectural Direction](architecture/03-architectural-direction.md)

### Boot and Hardware Setup
- [Boot and Handoff](architecture/04-boot-and-handoff.md)
- [x86_64 CPU Architecture](architecture/05-x86-cpu-architecture.md)
- [Memory Management](architecture/06-memory-management.md)
- [Scheduling and Preemption](architecture/07-scheduling-and-preemption.md)
- [Threading and Lifecycle](architecture/08-threading-and-lifecycle.md)

### Kernel Core
- [Process and IPC](architecture/09-process-and-ipc.md) - message-queue transport plus the planned vring bulk/zero-copy transport for device and service↔service channels
- [Capability and Policy](architecture/10-capability-and-policy.md)
- [Ring3 Isolation and Separation](architecture/11-ring3-isolation-and-separation.md)
- [WARP Ring3 Implementation Plan](architecture/31-warp-ring3-implementation.md) - ring-3 execution model, kernel trampolines, and remaining implementation guardrails
- [DMA Transfers](architecture/12-dma-transfers.md) - transient borrow-based DMA plus the planned driver-owned pinned DMA region primitive for virtqueue rings and other persistent device memory

### WASM Runtime
- [Runtime and Packaging](architecture/13-runtime-and-packaging.md) (wasm3 + WARP JIT)
- [libsys and IPC Service Runtime](architecture/14-libsys-and-service-runtime.md)

### Drivers and Services
- [Drivers and Services](architecture/15-drivers-and-services.md)
- [Device Manager and Bus Enumeration](architecture/16-device-manager-and-bus-enumeration.md)
- [Console I/O and Character Device](architecture/17-console-io-and-character-device.md)
- [Filesystem Stack](architecture/18-filesystem-stack.md)
- [Virtual Terminal — I/O Multiplexer](architecture/19-virtual-terminal.md) - keyboard/serial input routing, slot mux, klog-to-vt-1 (redesign in phases)
- [Graphics, Framebuffer, and Compositor](architecture/20-graphics-framebuffer-and-compositor.md)
- [Virtual Input Testing via Virtio-Serial](architecture/21-virtual-input-testing-via-virtio-serial.md)
- [Networking via Virtio-Net and User-Space Stack](architecture/22-networking-virtio-net-and-stack.md)

### User Space
- [CLI and User-Space Baseline](architecture/23-cli-and-user-space.md)
- [Environment Scopes and Inheritance](architecture/24-environment-scopes-and-inheritance.md)

### SMP
- [Symmetric Multi-Processing](architecture/28-smp.md)

### Scheduler
- [Threadable Scheduler Design](architecture/29-threadable-scheduler.md)
- [IPC Direct Switch](architecture/30-ipc-direct-switch.md)

### User-Space Concurrency
- [Coroutines, Futures, and Promises](architecture/32-coroutines-futures-promises.md) - proposed user-space coroutine/future/promise runtime (verified against the implementation); future/promise model replaces synchronous IPC; one shared future contract over separate native (stackful) and WASM (fiber/stackless) coroutine cores; §52 spike revisits the WASM-stackless conclusion with a green-thread (M:1) mechanism that suspends WASM guests at the host-call boundary with the engines untouched, unifying all languages (incl. AssemblyScript) and giving the AS gap a first-class fix
- [Completion Ports](architecture/33-completion-ports.md) - proposed kernel-owned, bounded completion queues with notification doorbells; a batched completion source for the future/promise runtime and high-rate networking operations

### Tooling and ABI
- [ABI IDL, Code Generation, and Error Model](architecture/34-abi-idl-and-error-model.md) - proposed single-source-of-truth IDL that generates the WASM host-call, IPC opcode, and error-code surfaces across wasm3/WARP(JIT+AOT) and all language variants; Zircon/Mach-style layered error model with a generated domain registry

### Operations and Validation
- [Diagnostics and Status](architecture/25-diagnostics-status.md)
- [Repository Map and Validation Baseline](architecture/26-repo-map-and-validation.md)
- [Python QEMU Test Framework](architecture/27-python-test-framework.md)

## Update Rules
- Update the relevant feature document(s) in `docs/architecture/` when behavior
  changes.
- Record implementation/baseline snapshot updates in `docs/STATUS.md` instead of
  this index.
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
