## Drivers and Services

### Implemented Drivers
- `ata`
  - PIO ATA block driver
  - owns the `block` endpoint
  - supports identify and read operations
- `fs-fat`
  - FAT12/16/32 filesystem driver
  - consumes the `block` endpoint
  - owns the `fs` endpoint
  - supports root/subdirectory listing, `cat`, `cd`, PM app loading, and the
    minimal shared libc read-only file API
  - follows FAT12/16 cluster chains for multi-cluster file reads on the
    current ESP baseline
- `chardev`
  - IPC-backed console/character device service
- `framebuffer`
  - optional native C driver packed as `FLAG_DRIVER|FLAG_NATIVE`
  - probes the kernel framebuffer APIs exposed via GOP
  - validates geometry and maps framebuffer pages into a fixed driver device
    virtual region through the native-driver API
  - paints a gradient on the standard QEMU VGA framebuffer when the device is present

### Implemented Services
- `process-manager`
  - validates WASMOS-APP containers
  - creates process/runtime state
  - resolves required endpoints
  - starts entries
  - exposes `spawn`, `spawn by name`, `wait`, `kill`, and `status`
- `device-manager`
  - scans ACPI RSDP data
  - starts the early storage driver chain
  - starts post-FAT display/input drivers by name from disk
- `sysinit`
  - intentionally narrow
  - starts post-FAT services and late user processes from the generated boot
    config
- `cli`
  - interactive shell over `proc` and `fs`

### Driver and Service Startup Chain
Current startup chain:
1. bootloader loads `initfs.img`
2. initfs contributes bootstrap `boot_module_t` entries for `device-manager`,
   `ata`, `fs-fat`, and the current smoke/bootstrap apps
3. kernel `init` spawns `device-manager`
4. `device-manager` starts the storage chain: `ata` and `fs-fat`
5. `device-manager` starts post-FAT hardware drivers by name: `serial`,
   `keyboard`, and `framebuffer`
6. kernel `init` waits for a successful FAT readiness probe
7. kernel `init` loads `sysinit` from disk via PM
8. `sysinit` loads the configured `sysinit.spawn` services/processes from disk,
   including `vt` and `cli`

This is the current stable bootstrap baseline.

### Boot Config
The initial config channel is a simple binary blob generated from TOML at build
time. The current generator reads `scripts/initfs.toml` and emits both the
initfs image and a compact `bootcfg.bin` payload.

Current config format:
- magic `WCFG0001`
- version
- bootstrap-module count
- sysinit-spawn count
- string-table size
- offset arrays for each string list
- NUL-terminated ASCII string table

Current `sysinit.spawn` validation:
- at least one late-start process must be configured
- process names must be unique
- process names must fit the current 16-byte PM by-name spawn ABI

Current use:
- the blob is carried in initfs for stable packaging
- the bootloader exposes the blob through `boot_info_t`
- wasm processes can read it through `wasmos_boot_config_size()` and
  `wasmos_boot_config_copy()`
- `sysinit` validates and consumes the `sysinit.spawn` string list for its
  late-start process policy and halts that policy path if the config is
  malformed

### What Is Still Missing
- full device-manager role split (bus enumeration + policy + lifecycle)
- full PCI enumeration
- richer ACPI-based device inventory publication
- hotplug handling
- capability-based MMIO/PIO/DMA/IRQ grants

### Device-Manager Design (MINIX-Style)
Goal: evolve the current bootstrap sequencer into a user-space hardware
discovery and driver lifecycle service, while keeping the kernel policy-light.

#### Design Principles
- Keep kernel scope minimal: scheduling, address spaces, IPC, interrupts,
  capability enforcement.
- Place discovery and policy in user space (`device-manager` + bus services).
- Run each driver as an isolated process with least privilege capabilities.
- Make driver startup deterministic at boot, then event-driven for hotplug.
- Preserve existing stable boot chain (`device-manager -> ata -> fs-fat ->
  sysinit`) while layering additional behavior incrementally.

#### Component Model
- `device-manager` (user space):
  - owns device registry and driver matching policy
  - coordinates probe/start/stop/restart lifecycle
  - consumes bus inventory from specialized bus services
- Bus services (user space):
  - `pci-bus`: PCI config-space enumeration and capability reporting
  - `acpi-bus`: ACPI table parsing and topology hints
  - future `usb-bus`: hotplug and endpoint inventory
- Driver processes (user space):
  - one process per driver instance
  - register service endpoints (`block`, `net`, `input`, `display`, etc.)
- Reincarnation monitor (user space, MINIX-style):
  - supervises critical drivers and restarts failed instances
- Kernel:
  - exports mechanism syscalls/hostcalls only (no driver matching policy)

#### Capability and Security Model
- Driver launch requires a capability manifest attached to spawn request:
  - `cap.ipc`: endpoint allow-list
  - `cap.io_port`: PIO port ranges
  - `cap.mmio`: physical MMIO ranges + mapping flags
  - `cap.irq`: allowed interrupt vectors/lines
  - `cap.dma`: approved DMA windows (phase-gated; initially disabled)
- Kernel validates capabilities at map/access/register time.
- `device-manager` is privileged to request capabilities but cannot bypass
  kernel checks.
- TODO: Introduce explicit kernel capability tokens so spawned drivers cannot
  inherit ambient access from bootstrap-era assumptions.

#### IPC Contract (Planned)
- `device-manager` service endpoint: `devmgr`
- Core messages:
  - `DEVMGR_PUBLISH_DEVICE`: bus service -> registry update
  - `DEVMGR_MATCH_DRIVER`: registry -> policy lookup
  - `DEVMGR_START_DRIVER`: manager -> process-manager spawn with manifest
  - `DEVMGR_DRIVER_READY`: driver -> manager readiness + service endpoints
  - `DEVMGR_DRIVER_EXIT`: monitor/pm -> manager failure notification
  - `DEVMGR_ENUM_QUERY`: clients -> snapshot query (debug/admin use)
- All messages carry `request_id`, source endpoint, and strict ownership
  checks (same anti-spoof style already used in ring3 IPC hardening).

#### Interrupt Delegation Model
- Kernel trap handler acknowledges low-level interrupt source and enqueues a
  lightweight interrupt event.
- Kernel routes event to the endpoint previously bound for that IRQ capability.
- Driver receives `IRQ_EVENT` IPC, performs device-specific handling in user
  space, and sends `IRQ_ACK`/rearm request if required.
- Kernel never runs device-specific logic.

#### Phase Plan and Implementation Tasks
Phase 0: Rename and baseline alignment
- Done:
  - rename `hw-discovery` service to `device-manager`
  - preserve existing storage-first bootstrap sequencing
- Tasks:
  - keep compatibility checks in tests/log markers
  - update docs and build paths

Phase 1: Internal registry and explicit state machine
- Tasks:
  - add in-memory device registry structure to `device-manager`
  - replace hardcoded spawn order with data-driven bootstrap plan entries
  - encode per-driver retry/backoff policy table
  - emit deterministic state transition logs for diagnostics
- Exit criteria:
  - no behavior regression in current `ata`/`fs-fat`/`serial`/`keyboard`/
    `framebuffer` startup path

Phase 2: Bus inventory services
- Tasks:
  - implement `pci-bus` user-space enumerator
  - parse ACPI roots in `acpi-bus` and expose normalized records
  - define `device_record` schema:
    - bus type, bus address, class/subclass/prog-if, vendor/device IDs
    - ACPI HID/CID/UID where available
    - BAR/MMIO/IRQ hints
  - publish records to `device-manager` through `DEVMGR_PUBLISH_DEVICE`
- Exit criteria:
  - registry snapshot reports discovered PCI devices on QEMU baseline

Phase 3: Driver matching and capability manifests
- Tasks:
  - introduce driver manifest format in boot config/initfs:
    - match predicates (bus/class/vendor/device/HID)
    - required capabilities (PIO/MMIO/IRQ)
    - startup policy (`critical`, `optional`, `on-demand`)
  - add matching engine in `device-manager`
  - extend process-manager spawn IPC to carry capability manifest handle
  - implement kernel-side capability checks for first-class resources
- Exit criteria:
  - `device-manager` can start drivers from discovered inventory, not only by
    fixed names

Phase 4: Driver supervision (reincarnation semantics)
- Tasks:
  - add driver liveness heartbeats and exit reason classification
  - implement restart policy:
    - bounded retries with cooldown
    - circuit-breaker for crash loops
    - escalation marker for fatal critical-driver failure
  - preserve endpoint/service identity semantics for restarted drivers
- Exit criteria:
  - intentional driver crash is isolated and auto-recovered without kernel
    panic

Phase 5: Interrupt ownership and hotplug pipeline
- Tasks:
  - add IRQ bind/unbind flow keyed by capability
  - route IRQ events to driver endpoints
  - add hotplug event path (`pci-bus`/future `usb-bus` -> `device-manager`)
  - support dynamic driver start/stop for add/remove events
- Exit criteria:
  - hotplug event can create/destroy driver instance without reboot

Phase 6: Observability and policy hardening
- Tasks:
  - add `devmgrctl` query surface (registry, drivers, restart counters)
  - add structured boot markers and negative-path tests
  - add deny-by-default capability policy with explicit allow-list updates
  - document threat model and residual trusted-computing-base assumptions
- Exit criteria:
  - deterministic debugability and repeatable failure triage for discovery and
    driver lifecycle paths

#### Validation Matrix (Per Phase)
- Boot path:
  - `run-qemu-test` remains green with no startup regressions
- Discovery:
  - expected inventory entries appear for baseline virtual hardware
- Isolation:
  - crashing driver process does not crash kernel or unrelated drivers
- Security:
  - unauthorized MMIO/PIO/IRQ requests are denied by kernel checks
- Lifecycle:
  - supervised restart policy behaves as configured under fault injection
