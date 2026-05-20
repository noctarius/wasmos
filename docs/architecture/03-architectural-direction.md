## Architectural Direction

### Microkernel Split
Kernel mechanisms:
- Boot-time platform handoff.
- Physical and virtual memory management primitives.
- Preemptive scheduling and process lifecycle control.
- IPC transport, endpoint ownership, and wakeup rules.
- Interrupt handling and timer-driven preemption.
- WASM runtime hosting plus native-driver ELF loading via WASMOS-APP hooks.

User-space policy:
- Driver startup order and long-running driver logic.
- Filesystem semantics.
- Process startup policy.
- Hardware discovery policy above the raw ACPI data scan.
- Future service registry, supervision, and namespace management.

### Device Discovery Direction
- Discovery must be bus-agnostic and event-driven.
- Bus services are producers of normalized device events (PCI first, USB next,
  then virtual/internal providers).
- `device-manager` is the policy engine:
  - maintains device registry state
  - evaluates rules
  - binds/unbinds driver instances
  - owns dynamic filesystem mount policy decisions
- Kernel remains mechanism-only for transport/capability enforcement.

### Unified Block-Device Identity Direction
- All storage providers (ATA, AHCI, NVMe, USB mass-storage, virtual block)
  must normalize to `bus=block` records in `device-manager`.
- Canonical block identity format:
  - `block:<parent-address>:<unit>`
  - examples:
    - `block:pci:00:01.01:ata0`
    - `block:pci:00:01.01:ata1`
    - `block:pci:00:04.00:nvme-ns1`
    - `block:usb:1-3.2:lun0`
- Each canonical identity also carries a deterministic hash-derived ID
  (SHA-256 over canonical identity, stored and exposed as a short operational
  identifier) for easier CLI/rule usage.
- Rules and mount policy should match `bus=block` records plus metadata
  (parent bus, removable, size, filesystem probe data) rather than matching
  specific transport-driver names.

### Privilege Model
- Today all processes still execute in ring 0 with per-process kernel data
  structures and separate runtime contexts.
- The architecture now includes a minimal syscall entry primitive (`int 0x80`)
  plus ring3-capable context-restore primitives (`iretq` path + per-process
  `rsp0` setup) and user-page mapping flag plumbing, but full user-mode
  process rollout and user-space pager ownership are still pending.
- Drivers are treated as privileged by policy.
- Services are intended to become least-privileged first once ring 3 support,
  page-table separation, and syscall entry are in place.
- Applications already carry the weakest semantic role in the container format,
  even if CPU privilege separation is not implemented yet.
