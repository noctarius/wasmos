# Tasks

This file tracks implementation work that is still open after the current
architecture and README cleanup.

IMPORTANT: Keep this file aligned with `README.md` and `ARCHITECTURE.md`.

## Boot and Platform
- Add APIC / IOAPIC support and retire the PIC-only interrupt assumption.
- Decide whether the kernel should eventually read initfs directly instead of
  relying on synthesized bootstrap `boot_module_t` records.

## Scheduling and Process Model
- Add scheduler observability beyond basic `ps` metrics.
- Add priorities and/or execution budgets.
- Prepare the scheduler and context-switch path for user-mode execution.
- Add SMP-aware scheduling only after the single-core model is fully stable.
- Potentially a more sophisticated scheduler design (e.g., a "fair" scheduler).
- Potentially a tickless preemptive multitasking scheduler.
- Any syscall/IPC call should be considered a safe point for preemption (would remove the need to `sched_yield`).
- Add a `fork` syscall to create a new process with a copy of the current process's
  memory and state.
- Add a `exec` syscall to replace the current process image with a new one.

## IPC
- Add true notification objects separate from synchronous IPC endpoints.
- Define shared-memory bulk-transfer conventions on top of the new
  `shmem_create/map/unmap` primitives (ownership rules, discovery, ABI docs).
- Add an explicit unmap/remap policy for WASM `wasmos_shmem_map` overlays
  (current unmap releases ownership but does not restore prior linear mapping).
- Add endpoint badges / richer sender identification.
- Add service-level IPC allowlists beyond current endpoint ownership checks.
- Add better async server support for multi-hop service chains.

## Memory and Privilege Separation
- Move page-fault handling out of the kernel-hosted scaffold into user space.
- Introduce ring 3 execution, syscall entry, and kernel/user stack separation.
- Add capability-granted MMIO/PIO/DMA/IRQ resource assignment for drivers.
- Introduction of an alternative SLAB allocator for the kernel and user space heap.

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
- Add a filesystem and mountpoint manager `mount-manager`.
- Add a filesystem abstraction.
- Add a virtual filesystem as the initial mount point `/` (root).
- Add a virtual `sys` filesystem for system-level configuration.
- Add a virtual `dev` filesystem for device-level configuration.
- Add a virtual `proc` filesystem for process-level information.
- Add dynamic mount points per filesystem.
- Mount the EFI filesystem as `/boot`.
- Add timer/clock (RTC) device support.
- Add NVMe support.
- Extend virtual terminal support beyond the current baseline:
  multi-TTY, richer ANSI handling, cooked/raw modes, history, and scrollback.
- Add NVMEM support.
- Add virtio support (virtio-blk, virtio-console, virtio-rng, virtio-fs, virtio-net).
- Asynchronous I/O support (e.g., `async-io`).

## Filesystem and Userland
- Extend `fs-fat` beyond the current small-file/read-only path.
- Extend libc and language-shim filesystem coverage beyond the current C
  low-level path and basic stdio support:
  update-mode semantics, richer buffering, and non-ASCII filename handling.
- Decide whether initfs should eventually carry additional early-userland data
  beyond bootstrap apps and boot config.
- Add full FAT32 support.
- Add ext4 support.
- Add a minimal custom filesystem (CFS_CUSTOM_FILE_SYSTEM.md).

## Userland and CLI
- Extend `ps` with more process-level information and memory and CPU usage.
- Add a `top` command to display process-level metrics.
- Add a `console` command to switch between serial and framebuffer consoles.
- Add a `mount` command to mount filesystems.
- Add a `umount` command to unmount filesystems.
- Extend a `ls` command with more file-level information such as size.
- Add a `rm` command to remove files.
- Add a `cp` command to copy files.
- Add a `mv` command to move files.
- Add a `mkdir` command to create directories.
- Add a `rmdir` command to remove directories.
- Add a `touch` command to create empty files.
- Add a `echo` command to write to the console.
- Add a `pwd` command to display the current working directory.
- Add a `clear` command to clear the console.
- Add a `sleep` command to suspend the current process for a specified duration.

## Buildsystem and Tooling
- Add a `make` wrapper to simplify the build process.
- Add an ncurses UI for the build system (similar to `menuconfig`).

## Documentation and Tests
- Keep source comments aligned with architecture decisions as internals evolve.
- Add tests for any new IPC notification or shared-memory paths.
- Add an interactive framebuffer regression test that drives keyboard input
  through VT/CLI (multiple commands + prompt returns) and fails on stuck input.
- Add broader malformed boot-config coverage and future startup-policy
  expansion beyond the current `sysinit.spawn` list.
