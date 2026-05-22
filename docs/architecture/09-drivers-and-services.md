## Drivers and Services

### Implemented Drivers
- `ata`
  - PIO ATA block driver
  - creates and registers the `block` endpoint via PM service registry
  - supports identify and read operations
- `fs-fat`
  - FAT12/16/32 filesystem driver
  - looks up the `block` endpoint via PM service registry
  - creates and registers the `fs` endpoint via PM service registry
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

Native payload policy:
- `FLAG_NATIVE` is valid for privileged module kinds (`FLAG_DRIVER` and `FLAG_SERVICE`).

### Implemented Services
- `process-manager`
  - validates WASMOS-APP containers
  - creates process/runtime state
  - resolves metadata-declared entry argument bindings
  - provides `register`/`lookup` service-registry IPC for endpoint discovery
  - starts entries
  - exposes `spawn`, `spawn by name`, `wait`, `kill`, and `status`
- `device-manager`
  - scans ACPI RSDP data
  - starts `pci-bus` and consumes published PCI inventory
  - starts the early storage driver chain using inventory-based matching
  - starts post-FAT display/input drivers by name from disk
  - now routes main reply-endpoint request/response traffic through the WASM
    `libsys` reactor/intent helpers for spawn/module-meta and mount-query
    operations, while keeping the existing phase/state policy flow intact
  - reserves udev-like policy rule roots:
    - `/init/devmgr/rules` (bootstrap policy from initfs)
    - `/boot/system/devmgr/rules` (runtime override policy from FAT)
- `pci-bus`
  - enumerates PCI config space in user space
  - looks up `device-manager` inventory endpoint (`devmgr.inv`) via PM service
    registry
  - publishes normalized device records to `device-manager`
- `sysinit`
  - intentionally narrow
  - starts post-FAT services and late user processes from the generated boot
    config
- `cli`
  - interactive shell over `proc` and `fs`
- `font-service` (native Zig)
  - loads built-in TTF fonts from `fs.vfs`
  - serves `FONT_IPC_OPEN_FONT_REQ`, `FONT_IPC_GET_METRICS_REQ`, and
    `FONT_IPC_RASTER_GLYPH_REQ`
  - now uses the shared native `libsys` event-loop/intent reactor pattern:
    single endpoint polling with registered handlers plus request-id-based
    intent completion for outbound FS IPC calls
  - logs a warning when an unexpected/unhandled IPC message type is received

### Driver and Service Startup Chain
Current startup chain:
1. bootloader loads `initfs.img`
2. initfs contributes bootstrap `boot_module_t` entries for `device-manager`,
   `ata`, `fs-fat`, and the current smoke/bootstrap apps
3. kernel `init` spawns `device-manager`
4. `device-manager` spawns `pci-bus` and waits for scan completion
5. `device-manager` starts the storage chain via rules:
   - `SUBSYSTEM=="pci", ATTR{...}, ... RUN+=...` selects storage bootstrap
     drivers from published
     PCI records when filters match
   - `SUBSYSTEM=="block", ATTR{unit}=="N", ENV{MOUNT}="...", RUN+=...` spawns
     `fs-fat` only after matching block-device registration
7. `device-manager` and later `/boot` policy continue driver bring-up through
   the same udev-style rule format
   - `/boot` override rules are post-storage policy only; entries targeting
     bootstrap storage drivers (`ata`, `fs-fat`) are rejected
8. kernel `init` waits for a successful FAT readiness probe
9. kernel `init` loads `sysinit` from disk via PM
10. `sysinit` loads the configured `sysinit.spawn` services/processes from disk,
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

DMA-specific design details, rollout phases, and done gates are tracked in
`docs/architecture/16-dma-transfers.md`.

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

#### Cross-Bus Device Model (Planned)
- Canonical `device_record` fields:
  - `device_id` (stable manager-assigned id)
  - `bus_type` (`pci`, `usb`, `virt`, ...)
  - `bus_addr` (bus-native address token)
  - `class`/`subclass`/`prog_if` when applicable
  - `vendor_id`/`device_id` when applicable
  - transport-specific attributes map (string key/value)
  - capability hints (MMIO/PIO/IRQ/DMA candidates)
  - state (`discovered`, `bound`, `active`, `removed`, `failed`)
- Bus services publish records; they do not decide binding policy.

#### `device_record` v0-draft (Normative)
- Schema version: `record_version = 1`
- Required fields:
  - `record_version: u16`
  - `device_uid: u64`
    - manager-assigned stable id for the lifetime of this discovered device
  - `bus_type: enum`
    - `pci | usb | virt`
  - `bus_addr: string[64]`
    - normalized address token (examples: `0000:00:01.1`, `usb:1-2.3`)
  - `event_gen: u32`
    - monotonic generation incremented on each state-changing event
  - `state: enum`
    - `discovered | bound | active | removed | failed`
  - `attr_count: u16`
  - `attrs[attr_count]`
    - tuple: `key:string[32]`, `value:string[96]`
- Optional fields:
  - `caps_hint`
    - `io_port_min:u16`, `io_port_max:u16`
    - `mmio_base:u64`, `mmio_len:u64`
    - `irq_line:u16`, `dma_hint:u32`
  - `bound_driver:string[96]`
  - `service_ref_count:u8`
  - `service_refs[service_ref_count]: string[32]`
- Identity and idempotence key:
  - `(bus_type, bus_addr, event_gen)`
- Required normalized attribute keys for `pci` records:
  - `vendor_id`, `device_id`, `class`, `subclass`, `prog_if`, `revision`

#### Device Event Model (Planned)
- Event types:
  - `DEVMGR_DEVICE_ADD`
  - `DEVMGR_DEVICE_REMOVE`
  - `DEVMGR_DEVICE_CHANGE`
  - `DEVMGR_BIND_RESULT`
  - `DEVMGR_UNBIND_RESULT`
- Events are idempotent by `(bus_type, bus_addr, generation)` and must be safe
  for replay after service restart.
- Removal is authoritative: once `REMOVE` is committed, active bindings must be
  torn down and exported services revoked.

#### `device_event` v0-draft (Normative)
- Required fields:
  - `event_version: u16` (`1`)
  - `event_type: enum`
    - `DEVMGR_DEVICE_ADD`
    - `DEVMGR_DEVICE_REMOVE`
    - `DEVMGR_DEVICE_CHANGE`
    - `DEVMGR_BIND_RESULT`
    - `DEVMGR_UNBIND_RESULT`
  - `device_uid: u64`
  - `bus_type: enum`
  - `bus_addr: string[64]`
  - `event_gen: u32`
  - `status: i32` (`0` success, negative error codes)
  - `source_service: string[32]`
- Replay semantics:
  - duplicate events for same `(bus_type,bus_addr,event_gen,event_type)` are
    ignored after first successful apply
- Ordering:
  - `REMOVE` with higher `event_gen` supersedes all older add/change events

#### Rule System (Planned)
- Rule roots (in load order):
  1. `/init/devmgr/rules` (bootstrap defaults)
  2. `/boot/system/devmgr/rules` (runtime override/extension)
- Override semantics:
  - later roots can replace or disable earlier matches by rule id/priority
  - deny rules take precedence over permissive defaults at equal priority
- Rule outputs:
  - target driver/service module
  - spawn capability profile selection
  - startup policy (`critical`, `optional`, `on-demand`)
  - mount policy (filesystem alias/path/priority)

#### `devmgr_rule` v1 (Normative)
- File format: line-based udev-style rules (`*.rules`)
- One rule per line, comma-separated clauses.
- Clause operators:
  - match: `KEY=="value"`
  - assign metadata: `KEY="value"`
  - append action: `KEY+="value"`
- Current accepted keys:
  - `SUBSYSTEM` (`"boot" | "pci" | "block"`)
  - `ATTR{bus}`, `ATTR{slot}`, `ATTR{function}` (optional PCI BDF selectors)
  - `ATTR{class}`, `ATTR{subclass}`, `ATTR{prog_if}`, `ATTR{vendor}`, `ATTR{device}` (hex for PCI matching)
  - `ATTR{unit}` (`0..255` or `"any"` for block matching)
  - `ENV{MOUNT}` (mount alias output for block-backed filesystem rules)
  - `RUN` (driver/service module path to spawn)
- Minimal examples:
```udev
SUBSYSTEM=="pci", ATTR{class}=="0x01", ATTR{subclass}=="0x01", RUN+="system/drivers/ata.wap"
SUBSYSTEM=="pci", ATTR{class}=="0x03", ATTR{subclass}=="0x00", ATTR{prog_if}=="0x00", RUN+="system/drivers/fbpci.wap"
SUBSYSTEM=="block", ATTR{unit}=="0", ENV{MOUNT}="/boot", RUN+="system/drivers/fs_fat.wap"
```

#### Rule Evaluation and Precedence (Normative)
- Load order:
  1. `/init/devmgr/rules`
  2. `/boot/system/devmgr/rules`
- Merge model:
  - parsed runtime-root rules replace in-memory bootstrap rule tables for the
    same rule family in current implementation
- Evaluation order:
  - rule family queues are processed deterministically (`boot` force-spawn,
    then match-driven `pci`/`block` flows)
  - malformed lines are ignored (do not abort manager startup)

#### Dynamic Mount Policy (Planned)
- Filesystem mounts are outcomes of rule evaluation, not fixed to `/boot`,
  `/user`, `/init`.
- Example policy knobs:
  - bind a discovered block device to `/user`
  - remap same backend to an alternate path by override rule
  - assign mount priority/fallback behavior when multiple candidates exist
- `fs-manager` remains the namespace router, while `device-manager` owns mount
  intent and updates.
- Mount table query/reporting is an `fs-manager` responsibility; CLI/tooling
  should query `fs.vfs` for mount state instead of relying on fixed
  device-manager mount indices.

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
  - load bootstrap device policy rules from `/init/devmgr/rules` and overlay
    runtime overrides from `/boot/system/devmgr/rules` once storage is online
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
  - bus-agnostic record schema supports first `usb-bus` integration without
    rewriting `device-manager` core registry paths

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
  - mount policy update path handles add/remove without reboot (including
    filesystem unmount on remove and remount-on-readd behavior)

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

#### Next Implementation Slice (Do This First)
Scope: implement the smallest end-to-end discovery pipeline that replaces
hardcoded storage startup order with PCI-driven ATA-first policy.

1. Add `pci-bus` user-space service
- Enumerate bus/device/function via PCI config-space access.
- Publish one normalized record per discovered function.
- Include class/subclass/prog-if and vendor/device IDs in each record.

2. Add `device-manager` in-memory registry
- Accept `DEVMGR_PUBLISH_DEVICE` records from `pci-bus`.
- Store records in a fixed-capacity table (minimal dynamic behavior first).
- Expose a debug dump marker/log for boot-time verification.

3. Add first matching rule set (storage only)
- Select PCI mass-storage class devices (`class=0x01`).
- Prioritize ATA/IDE-compatible controllers first for current boot baseline.
- Keep non-storage classes ignored in this slice.

4. Replace hardcoded storage spawn with match-driven spawn
- Spawn `ata` only when a matching controller record exists.
- Keep `fs-fat` dependency-ordered after successful `ata` startup.
- Preserve current retry/backoff behavior while changing only selection logic.

5. Keep post-storage behavior unchanged
- Continue loading non-storage drivers/services only after FAT readiness.
- Do not introduce hotplug, restart supervision, or broader capability policy
  in this first slice.

Exit criteria:
- QEMU baseline boots with `device-manager` using PCI inventory + match flow.
- `ata`/`fs-fat` still come up reliably and `sysinit` startup remains green.
- `run-qemu-test` passes with no regression in existing boot markers.
