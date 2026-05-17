# Tasks

This file tracks active implementation work after the architecture/docs
reorganization.

IMPORTANT: Keep this file aligned with `README.md` and `docs/ARCHITECTURE.md`.
Isolation execution baseline: see
`docs/architecture/14-ring3-isolation-and-separation.md` for the current ring-3
separation model and deferred hardening backlog.

## Status Sweep (from `docs/architecture/*`)

### Completed / Landed
- [x] Ring-3 strict policy enabled in normal boots; dedicated ring-3 smoke kept
  as separate test target.
- [x] Baseline syscall boundary and ring-3 transition plumbing (`int 0x80`,
  scheduler/restore ring-aware paths).
- [x] Shared-memory primitives landed for native and WASM paths
  (`shmem_create/map/unmap`) plus console-ring transport.
- [x] Capability/policy authorization path enforced for privileged operations
  (including control-plane paths).
- [x] VT baseline landed: multi-TTY model, switching, per-tty CLI ownership,
  bounded queue-backpressure behavior, and baseline line discipline controls.

### Open / Remaining
- [ ] Continue post-closure ring-3 hardening per
  `docs/architecture/14-ring3-isolation-and-separation.md`
  (boundary cleanup and CLI flake reduction).
  - Owner: kernel-security + kernel-memory + kernel-scheduler.
  - Includes explicit follow-ups for ring3 TODOs in `src/kernel/framebuffer.c`,
    `src/kernel/serial.c`, `src/kernel/cpu.c`, and `src/kernel/process.c`.
- [ ] Service registry and discoverability model (including VT endpoint
  discovery migration away from fixed/wired assumptions).
- [ ] Supervision and restart policy for long-running services/drivers.
- [ ] Broader config-driven startup policy beyond current `sysinit.spawn`.

## Boot and Platform
- [ ] Add APIC / IOAPIC support and retire PIC-only assumptions.
- [ ] Decide whether kernel should eventually read initfs directly instead of
  relying on synthesized bootstrap `boot_module_t` records.

## Scheduling and Process Model
- [x] Threading Phase C closure (current scope): native ring3 syscall baseline
  covers `gettid`, `thread_yield`, `thread_exit`, `thread_create`,
  `thread_join` (including self-join deny), and `thread_detach` (including
  invalid-argument deny and detach-then-join deny) with strict lifecycle gate
  coverage in `run-qemu-ring3-threading-test`.
- [x] Threading Phase D closure (current scope): join/kill race handling is
  hardened so blocked joiners are deterministically released during
  process-group kill, with dedicated strict-threading regression markers for
  join wake ordering and kill edges (`[test] threading join wake order ok`,
  `[test] threading join after kill order ok`, `[test] threading join kill wake ok`,
  `[test] threading wait kill wake ok`); stack teardown now restores guard-page
  mappings before allocator free to keep recycled pages safely reachable in the
  shared higher-half window.
- [ ] Add scheduler observability and latency instrumentation beyond basic `ps`
  metrics.
- [ ] Add priorities and/or execution budgets.
- [ ] Add SMP-aware scheduling only after single-core behavior is stable.
- [ ] Evaluate tickless scheduling strategy.
- [ ] Treat syscall/IPC boundaries as generic preemption-safe points where
  feasible.
- [ ] Add `fork` syscall.
- [ ] Add `exec` syscall.

## IPC and Shared Memory
- [ ] Add true notification objects separate from synchronous IPC endpoints.
- [ ] Define shared-memory bulk-transfer conventions (ownership, discovery,
  ABI contracts).
- [ ] Define explicit unmap/remap policy for WASM `wasmos_shmem_map` overlays
  (current unmap semantics do not restore prior mappings).
- [ ] Add endpoint badges / richer sender identity.
- [ ] Add service-level IPC allowlists beyond endpoint ownership checks.
- [ ] Improve async server support for multi-hop service chains.

## Memory and Isolation
- [ ] Move page-fault handling/pager policy fully out of kernel-hosted scaffold
  into user space.
- [ ] Continue kernel/user boundary hardening under strict ring-3 defaults.
- [ ] Extend capability-granted MMIO/PIO/DMA/IRQ resource assignment breadth
  and policy coverage.
- [x] DMA Phase 0: define shared capability/ABI contract scaffolding.
  - Added `DEVMGR_CAP_DMA`, DMA direction/status constants, and
    `PROC_IPC_SPAWN_CAPS_V2` contract ids in shared ABI headers.
  - Added `wasmos_spawn_caps_v2_t` + DMA window/direction/max-bytes schema
    types for spawn-profile transport.
  - Added kernel spawn-profile storage/query fields for DMA descriptors
    (direction/max-bytes/window base+length) for future policy enforcement.
  - Added explicit fail-closed behavior: legacy `PROC_IPC_SPAWN_CAPS` denies
    DMA flag usage until v2 descriptor transport is implemented.
- [x] DMA Phase 1: implement borrow-buffer-based DMA map/sync/unmap enforcement
  in kernel (owner/context checks, direction checks, window/range checks).
  - Added WASM hostcalls `dma_map_borrow`, `dma_sync_borrow`,
    `dma_unmap_borrow` in kernel link layer.
  - Enforced source-endpoint ownership checks + borrow-source context matching.
  - Enforced borrow grant compatibility with DMA direction flags.
  - Enforced spawn-profile DMA direction + max-bytes + window-range checks.
  - Added fail-closed mapped-state behavior for borrow release/unmap ordering.
- [x] DMA Phase 2: implement `PROC_IPC_SPAWN_CAPS_V2` end-to-end transport in
  `device-manager` + process-manager and wire descriptor parsing/validation.
  - Process-manager now accepts spawn capability descriptors via
    `PROC_IPC_SPAWN_CAPS_V2` payload pointer/size contract.
  - Process-manager now copies descriptors from caller memory and validates cap
    bitmask, IO range ordering, DMA direction flags, max-bytes, and DMA window
    overflow semantics before applying spawn profile.
  - `PROC_IPC_SPAWN_CAPS_V2` now uses variable-length DMA window payloads and
    rejects malformed size/count/range descriptors fail-closed.
- [x] DMA Phase 3: integrate first storage-path driver flow on borrow-based DMA
  with deterministic fallback path and deny-path coverage markers.
  - ATA storage path now attempts borrow-based DMA lifecycle
    (`buffer_borrow` + `dma_map_borrow` + `dma_sync_borrow` + `dma_unmap_borrow`)
    for block read/write requests.
  - Deterministic fallback remains active: deny/range/unavailable DMA results
    fall back to existing PIO/copy transfer path without breaking bootstrap.
  - ATA emits one-shot coverage markers for both DMA-active and fallback paths.
  - Native framebuffer borrow path now also attempts borrow-based DMA
    map/sync/unmap in kernel native-driver plumbing and emits one-shot active/
    fallback markers for additional path validation.
- [ ] Evaluate broader SLAB allocator rollout for kernel and user-space heaps.
- [ ] DMA Phase 4: expand validation/hardening around framebuffer DMA path.
  - Kernel native-driver framebuffer borrow path now emits
    `[test] framebuffer dma phase4 matrix ok|mismatch` covering wrong-source
    deny, repeated map/sync/unmap churn, and stale-unmap deny semantics.

## Runtime and Loading
- [ ] Enforce WASMOS-APP heap `max_pages` (current runtime cap is global).
- [ ] Expand runtime diagnostics without modifying vendored runtimes.
- [ ] Decide whether non-`wasm3` runtime experimentation should remain
  out-of-tree or behind explicit opt-in integration points.

## Drivers and Services
- [ ] Split `device-manager` into inventory/policy/lifecycle responsibilities
  and add dedicated bus services (`pci-bus`, `acpi-bus`).
- [ ] Extend `device-manager` beyond ACPI/storage bootstrap.
- [ ] Add PCI device inventory and driver matching.
- [ ] Add hotplug/event publication.
- [ ] Add filesystem/mountpoint manager (`mount-manager`) and VFS layer.
- [ ] Add virtual `sys`, `dev`, and `proc` filesystems.
- [ ] Add dynamic mount-point support and mount EFI as `/boot`.
- [ ] Add RTC/timer device support.
- [ ] Add NVMe support.
- [ ] Add NVMEM support.
- [ ] Add virtio support (`virtio-blk`, `virtio-console`, `virtio-rng`,
  `virtio-fs`, `virtio-net`).
- [ ] Add asynchronous I/O support.

## Virtual Terminal
- [ ] Complete richer ANSI/VT handling beyond current subset.
- [ ] Add UTF-8 expansion path for VT rendering/input pipeline.
- [ ] Add per-tty scrollback.
- [ ] Extend VT client API where needed for richer terminal behavior.
- [ ] Add explicit tty-switch behavior tests (`tty 1/2/3`) that verify
  per-tty shell/input isolation across virtual terminals.
- [ ] Add focused VT allocator stress tests (`memory.grow` / near-OOM paths).
- [ ] Evaluate extraction of VT startup allocator helper for shared WASM
  service reuse.
- [ ] Add optional VT memory pressure telemetry/soft-cap reporting.
- [ ] Revisit deferred framebuffer prompt duplication/misalignment artifact
  under rapid `Ctrl+Shift+Fn` switching once stable repro exists.

## Filesystem and Userland
- [ ] Extend `fs-fat` coverage beyond current baseline semantics.
- [ ] Extend libc/language-shim filesystem coverage: update modes, buffering,
  and non-ASCII filename handling.
- [ ] Decide whether initfs should carry additional early-userland payloads
  beyond bootstrap apps and boot config.
- [ ] Add full FAT32 support.
- [ ] Add ext4 support.
- [ ] Add minimal custom filesystem (`docs/CFS_CUSTOM_FILE_SYSTEM.md`).

## CLI and Userland Tools
- [ ] Extend `ps` with richer process/memory/CPU reporting.
- [ ] Add `top` command.
- [ ] Add `console` command to switch serial/framebuffer views.
- [ ] Add `mount` / `umount` commands.
- [ ] Extend `ls` with richer file metadata.
- [ ] Add `rm`, `cp`, `mv`, `mkdir`, `rmdir`, `touch`, `echo`, `pwd`, `clear`,
  and `sleep` commands.

## Buildsystem and Tooling
- [ ] Add a `make` wrapper for common workflows.
- [ ] Add ncurses-style build UI (`menuconfig`-like flow).

## Documentation and Tests
- [ ] Keep source comments aligned with architecture decisions as internals
  evolve.
- [ ] Add tests for new IPC notification/shared-memory paths.
- [ ] Add broader malformed boot-config coverage and startup-policy expansion
  tests beyond current `sysinit.spawn` list.
