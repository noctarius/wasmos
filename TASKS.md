# Tasks

This file tracks implementation work that is still open after the current
architecture and README cleanup.

IMPORTANT: Keep this file aligned with `README.md` and `ARCHITECTURE.md`.

## Boot and Platform
- Add framebuffer console support in addition to serial.
- Add APIC / IOAPIC support and retire the PIC-only interrupt assumption.
- Decide whether the kernel should eventually read initfs directly instead of
  relying on synthesized bootstrap `boot_module_t` records.

## Scheduling and Process Model
- Add scheduler observability beyond basic `ps` metrics.
- Add priorities and/or execution budgets.
- Prepare the scheduler and context-switch path for user-mode execution.
- Add SMP-aware scheduling only after the single-core model is fully stable.

## IPC
- Add true notification objects separate from synchronous IPC endpoints.
- Add shared-memory bulk transfer primitives and conventions.
- Add endpoint badges / richer sender identification.
- Add service-level IPC allowlists beyond current endpoint ownership checks.
- Add better async server support for multi-hop service chains.

## Memory and Privilege Separation
- Move page-fault handling out of the kernel-hosted scaffold into user space.
- Introduce ring 3 execution, syscall entry, and kernel/user stack separation.
- Add capability-granted MMIO/PIO/DMA/IRQ resource assignment for drivers.

## Runtime and Loading
- Expand WASMOS-APP policy enforcement and metadata validation as needed.
- Enforce WASMOS-APP heap `max_pages` instead of relying on the current global
  2 GiB runtime-heap cap.
- Add richer runtime diagnostics without modifying vendored runtimes.
- Evaluate whether non-`wasm3` runtime experimentation should live out-of-tree or
  behind explicit opt-in integration points.

## Drivers and Services
- Add `driver-manager`.
- Extend `hw-discovery` beyond the current ACPI/storage bootstrap path.
- Add PCI device inventory and driver matching.
- Add hotplug/event publication.
- Decide on the long-term service registry / naming model.

## Filesystem and Userland
- Extend `fs-fat` beyond the current small-file/read-only path.
- Add write, seek, and stat support where appropriate.
- Decide whether initfs should eventually carry additional early-userland data
  beyond bootstrap apps and boot config.

## Documentation and Tests
- Keep source comments aligned with architecture decisions as internals evolve.
- Add tests for any new IPC notification or shared-memory paths.
- Add broader malformed boot-config coverage and future startup-policy
  expansion beyond the current `sysinit.spawn` list.
