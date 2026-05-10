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
  (boundary cleanup, adversarial coverage expansion, and flake reduction).
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
- [ ] Threading Phase D (in progress): harden join/kill race handling so
  blocked joiners are deterministically released during process-group kill;
  current baseline now includes in-process join wake ordering coverage
  (`[test] threading join wake order ok`); remaining work includes explicit
  strict-threading regression markers for kill-during-join/join-after-kill
  ordering and tighter per-thread cleanup invariants on non-main-thread ring3
  lifecycle edges.
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
- [ ] Evaluate broader SLAB allocator rollout for kernel and user-space heaps.

## Runtime and Loading
- [ ] Enforce WASMOS-APP heap `max_pages` (current runtime cap is global).
- [ ] Expand runtime diagnostics without modifying vendored runtimes.
- [ ] Decide whether non-`wasm3` runtime experimentation should remain
  out-of-tree or behind explicit opt-in integration points.

## Drivers and Services
- [ ] Add `driver-manager`.
- [ ] Extend `hw-discovery` beyond ACPI/storage bootstrap.
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
