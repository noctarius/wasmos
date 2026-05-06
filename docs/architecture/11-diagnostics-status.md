## Diagnostics and Tracing

### Visible by Default
These remain visible even with tracing disabled:
- normal boot logs
- fatal diagnostics
- runtime failure diagnostics
- process listings and CLI-visible service output

### Visible Only With `WASMOS_TRACE=ON`
- init / PM / scheduler trace lines
- `debug_mark(tag)`
- periodic timer progress markers (`[timer] ticks`)
- verbose scheduling and runtime transition traces

### Existing Debug Hooks
- `g_skip_wasm_boot` can isolate runtime bring-up in the kernel init path
- GP fault reporting includes PID, name, stack bounds, RIP, CS, and RFLAGS
- CLI tests and smoke apps provide functional regression coverage for IPC,
  preemption, filesystem access, and language shims

## Current Status by Area

### Done
- boot contract versioning and validation
- ELF loading with aligned/overlap-safe segment handling
- single-image initfs bootstrap packaging
- serial-first early boot diagnostics
- physical memory allocator
- basic paging scaffold
- process contexts and stacks from physical memory
- preemptive round-robin scheduler
- timer-driven context switching
- IPC endpoint ownership enforcement
- process manager and WASMOS-APP loader
- bootstrap split between kernel `init` and user-space `sysinit`
- FAT-backed loading of `sysinit`, `cli`, and normal applications
- generated boot-config blob exposed to user space
- shared read-only file API in userland libc
- language-native application entrypoint shims

### Partially Done
- filesystem support
  - works for current boot and small-file scenarios
  - still limited in read breadth and filesystem operations
- hardware discovery
  - enough for ACPI RSDP discovery and storage bootstrap
  - not yet a general device manager
- memory service
  - kernel-hosted scaffold exists
  - real user-space pager does not

### Not Done
- ring 3 execution
- syscall ABI
- APIC / SMP
- shared-memory IPC fast paths
- service registry
- supervision / restart policy
- capability-granted device access
- broader config-driven startup policy beyond the current `sysinit.spawn` list

Open implementation work is tracked in `TASKS.md`.

