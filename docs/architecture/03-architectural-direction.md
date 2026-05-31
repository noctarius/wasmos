## Architectural Direction

This document describes the intended structure and evolution of WASMOS: the
key design decisions, how subsystems are expected to grow, and the structural
invariants that all changes must preserve. It bridges between the goals
(`01-goals.md`) and the current implementation (`02-current-system-summary.md`).

---

### Microkernel Split

The kernel does scheduling, memory, IPC, interrupts, and capability enforcement.
Everything else belongs in user space. This split is not provisional — it is the
architectural contract that all other design decisions extend from.

**What the kernel will always own:**
- Physical frame allocation and virtual memory mapping primitives.
- Per-process page tables and CR3 management.
- IPC endpoint table and message routing.
- Interrupt and exception dispatch; IRQ routing to registered endpoints.
- Capability grant table and enforcement at every hostcall/syscall boundary.
- wasm3 runtime hosting and native ELF loading via the WASMOS-APP hook.

**What the kernel will never own:**
- Device identification or driver matching policy.
- Filesystem mount policy or VFS routing semantics.
- Network protocol state or socket lifecycle.
- Display policy, window management, or surface composition.
- Service discovery or naming beyond endpoint ID allocation.
- Process restart or supervisor policy.

The test for a proposed kernel addition: does it require kernel privilege to
implement correctly, and can it not be built from existing primitives in user
space? If the answer to either question is no, it belongs in user space. When in
doubt, start in user space and move to the kernel only when a concrete privilege
requirement surfaces.

---

### IPC Architecture

IPC is the system's primary integration mechanism. The fixed-layout message
(type, source, destination, request_id, arg0–arg3) is the transport primitive;
it is intentionally minimal. The direction for growth is:

**Bulk data through borrow handles, never through messages.**
Small control traffic stays in-message. Bulk payloads — file content, network
frames, pixel buffers — use the buffer-borrow model with explicit grant/release
semantics and typed borrow kinds. DMA extensions attach to borrow handles rather
than introducing a separate allocator. This keeps IPC messages fast and
composable regardless of payload size.

**Typed contracts in shared headers.**
Message types for each service are defined in one place (`wasmos_driver_abi.h`
and per-subsystem headers). Undocumented IPC coupling is not a supported design
pattern. If a component depends on a message type, that type is in a shared
header with documented argument layouts.

**Opcode space allocated by range.**
Current allocation:
- `0x100–0x1FF` chardev
- `0x200–0x2FF` proc/spawn
- `0x300–0x3FF` block
- `0x400–0x4FF` filesystem
- `0x600–0x6FF` fbtext
- `0x700–0x7FF` VT
- `0x800–0x8FF` input/RTC/virtio-serial
- `0x900–0x9FF` device-manager
- `0xA00–0xBFF` networking

New subsystems claim the next free range; they do not scatter opcodes into
existing ranges.

**Single-endpoint event loops.**
Services use `libsys` intent-keyed request/reply tracking and type-dispatched
handlers on a single receiver endpoint. Multiple ad-hoc receive loops on the
same endpoint cause response stealing and are an anti-pattern. New services
should follow the `wasmos_sys_event_loop_t` pattern from the start.

**Notifications distinct from request/reply.**
The current model handles synchronous request/reply and fire-and-forget
notifications. Richer async primitives (readiness notification, edge-triggered
wakeup) are future work once the basic model is exercised across more subsystems.

---

### Privilege and Isolation Model

Ring3 execution is fully implemented and enabled by default. The privilege
hierarchy is:

**CPL0 — kernel.** Scheduler, memory manager, interrupt handlers,
hostcall/syscall dispatch. Kernel code runs with full hardware access.

**CPL0 — native drivers (function-table ABI).** Native ELF drivers run in
kernel context through an explicit function-table ABI with validated arguments.
They are trusted-but-structured: same privilege level as the kernel, but invoked
only through declared entry points. Native is the escape hatch for hardware
access that WASM cannot express — not a general privileged-execution model.

**CPL3 — WASM services and drivers (wasm3 sandbox + capabilities).** WASM
processes execute in user space. The wasm3 interpreter enforces WASM memory
safety; the capability system enforces hardware access. A WASM driver can hold
`io.port`, `irq.route`, and `dma.buffer` capabilities and perform real hardware
I/O while still being sandboxed at the language level.

**CPL3 — applications.** No I/O ports, no IRQ routing, no DMA. IPC only. The
WASMOS-APP `FLAG_APP` kind is the weakest role in the system.

The direction for privilege evolution:
- Native drivers remain CPL0 for the foreseeable future.
- WASM services are the primary long-term execution environment. The security
  story is wasm3 sandbox + capability enforcement + IPC contract isolation.
- The kernel pager is still kernel-hosted. Moving pager policy toward user-space
  ownership is long-term future work after the basic ring3 model is fully
  exercised.
- IPC endpoint badging (scoped endpoint access so a driver can reach exactly one
  service, not all services) is the next planned hardening step.

---

### Capability and Security Architecture

Capabilities are the kernel's hardware-access enforcement boundary. The model:

- Five kinds: `io.port`, `irq.route`, `mmio.map`, `dma.buffer`,
  `system.control`.
- Grants are per-context, assigned at spawn time by `device-manager`. Nothing is
  ambient or inherited.
- Enforcement is at every relevant entry point. `policy_authorize(context,
  action, arg0)` is the single kernel authorization path; new privileged
  operations add a check there rather than inline at the call site.
- Capability declarations are validated fail-closed at pack time by
  `make_wasmos_app`. Unknown capability names and invalid flags are pack errors,
  not runtime unknowns.

The direction for capability evolution:
- **IPC endpoint tokens.** Service-level allowlists so a driver can be granted
  access to exactly the `fs.vfs` endpoint without being able to reach other
  endpoints by guessing IDs.
- **Capability renegotiation.** A driver currently cannot acquire new capabilities
  after spawn. The planned model is that `device-manager` can revoke and reissue
  a capability profile on driver restart, preserving the deny-by-default property
  across restarts.

---

### Device Discovery and Driver Lifecycle

The current `device-manager` is a bootstrap sequencer that has grown a
policy-rule engine. The intended direction is a full MINIX-style device manager:

**Bus-agnostic discovery.**
PCI (done) and ACPI (done) publish normalized `device_record` events to
`device-manager`. USB and virtual providers follow the same contract. The
manager does not know about bus topology beyond what records carry; it evaluates
rules against records.

**Data-driven driver matching.**
Rules in `/init/devmgr/rules` (bootstrap, from initfs) and
`/boot/system/devmgr/rules` (runtime override, from FAT) select driver paths and
capability profiles from device records. No hardcoded spawn order in the manager.
Bootstrap storage drivers (`ata`, `fs-fat`) are permanently owned by initfs rules
and cannot be overridden by runtime rules.

**Supervised driver lifecycle.**
Drivers should be restartable without kernel reboot. A crashed driver is
detected, its stale endpoints revoked, and a replacement spawned. Endpoint
identity preservation across restart (so callers can rebind) and liveness
tracking by the manager are the required primitives.

**Dynamic mount policy.**
Filesystem mount assignments are rule outcomes, not compile-time constants. A
block-device rule specifies the mount alias; `fs-manager` routes accordingly.
CLI and tooling query `fs.vfs` for mount state rather than relying on fixed
device-manager mount indices.

**Unified block-device identity.**
All storage transports normalize to `block:<parent-address>:<unit>` canonical
IDs with a short SHA-256-derived operational ID. Rules and mount policy match
normalized block records, not transport-specific driver names. Examples:
`block:pci:00:01.01:ata0`, `block:pci:00:04.00:nvme-ns1`, `block:usb:1-3.2:lun0`.

---

### Memory Management Direction

The kernel memory model evolves in three phases (details in
`docs/architecture/07-memory-management.md`):

**Phase 1 — intent-based allocation API.**
Replace all ad-hoc `pfa_alloc_pages_below(X)` calls with typed intent: `MM_ALLOC_STACK`,
`MM_ALLOC_PGTABLE`, `MM_ALLOC_DMA32`, `MM_ALLOC_GENERIC`. Only paths with a
genuine hardware constraint should request low physical zones.

**Phase 2 — decouple kernel-internal allocations from DMA32.**
Kernel stacks and page tables must not compete with device DMA for low memory.
This is the root cause of artificial exhaustion under high-spawn workloads.

**Phase 3 — kernel virtual reachability upgrade.**
Either a full direct-map (physmap) of RAM into kernel virtual space, or a
map-on-demand `kmap` layer. Either approach removes the physical-ceiling that
currently limits the allocator's effective view of available memory.

**DMA specifically.**
The borrow-buffer DMA lifecycle (`dma_map_borrow`/`dma_sync_borrow`/
`dma_unmap_borrow`) is the stable driver-facing interface. Hardware IOMMU
(VT-d/AMD-Vi) is a future backend substitution: IOMMU domains replace the
current physical-window enforcement without changing the driver API. The driver
does not need to change when IOMMU is enabled.

---

### Runtime and Packaging Direction

**wasm3 is the supported runtime.** The integration model is process-local
instances with kernel-owned per-process chunked heap allocation. Runtime
instances are created and destroyed with processes; there is no shared runtime
state between processes.

**WASMOS-APP is the unit of deployment.** The format is intentionally simple and
self-describing: header, name, entry export, capability list, memory hints,
payload. The process manager can load any WASMOS-APP payload without external
metadata. Changes to the format are acceptable; self-description is the
invariant.

**Multi-language support is first-class.** The shim pattern (C, Zig, Go, Rust,
AssemblyScript all exporting `wasmos_main` through a language-native entry) is
the stable contract. New language support follows the same pattern. The runtime
ABI is what the kernel sees; the language surface is what the developer sees.

**`make_wasmos_app` is fail-closed.** Unknown capability names, malformed
manifests, and invalid flags are pack errors. Capability correctness is enforced
at build time, not discovered at runtime.

---

### Graphics and Display Direction

The graphics architecture enforces strict layering:

- **Framebuffer driver** owns hardware and mode setting only. It maps pages,
  drains the console ring, and responds to mode-change IPC. It does not know
  about windows or surfaces.
- **gfx-compositor** owns all display policy: window layout, z-order, surface
  composition, input routing. It accesses hardware only through borrowed
  framebuffer handles and IPC — never directly.
- **IPC carries control; shared memory carries pixels.** The compositor writes
  into kernel-managed shared buffers and issues damage notification through IPC.
  Pixel data never flows through IPC messages.
- **font-service** owns all text rendering and glyph rasterization. No component
  embeds an independent font renderer.

The direction for the application programming model: apps manage content
surfaces and receive events through `GFX_IPC_POLL_EVENT`. `libui` provides
component-tree abstractions over the raw compositor IPC. Custom display paths
that bypass the compositor are not a supported pattern.

---

### Networking Direction

Networking follows the same microkernel split as every other hardware subsystem:

- **`virtio-net` driver** owns transport: PCI probe, virtqueue management,
  RX/TX, IRQ. No protocol logic, no socket semantics.
- **`net-stack` service** owns protocol state: ARP, IPv4/IPv6, ICMP, UDP, TCP
  via lwIP. No hardware access; communicates with the driver exclusively through
  IPC.
- **Clients use socket-style IPC** against the `net.stack` endpoint. The kernel
  has no knowledge of sockets.

The rollout is correctness-first: copy path for packet data, no offload,
static IP for the initial baseline. Optimization (zero-copy RX via borrow-handle
forwarding, DMA for packet buffers, DHCP, multiple stack instances) comes after
the baseline end-to-end path is proven. See
`docs/architecture/20-networking-virtio-net-and-stack.md`.

---

### Threading Direction

In-kernel threading is production-complete for single-core. The threading model:
- Threads within one process share a context (`context_id`) and address space.
- Each thread has its own kernel stack and scheduler identity.
- Blocking IPC, join, and sleep block only the calling thread.

SMP is a deliberate non-goal for now. Multi-core support requires kernel-wide
locking discipline that would substantially increase complexity before the
single-core model is fully exercised. The single-core threading model must be
solid first.

---

### Structural Invariants

These properties are architectural anchors. Changes may break any interface
freely, but they must preserve these:

1. **The boot chain embeds no driver logic.** `BOOTX64.EFI` loads files and
   fills `boot_info_t`. It does not start services or make policy decisions.
   `sysinit` starts late user processes from boot config only — it is not a
   second bootstrap coordinator.

2. **The kernel does not match drivers to devices.** `device-manager` owns
   that policy. The kernel knows about capabilities and endpoints; it does not
   know about PCI vendor IDs or device classes.

3. **IPC is the only integration path between components.** No shared global
   state across service boundaries, no function pointers exchanged outside the
   native-driver ABI, no implicit coupling through shared memory without
   explicit IPC coordination establishing ownership.

4. **Capability grants are declared at spawn, not discovered at runtime.** A
   driver's hardware access footprint is fully described by its capability
   profile. A driver cannot acquire new hardware capabilities after spawn
   without going through `device-manager`.

5. **`run-qemu-test` is always green.** This is the single non-optional gate.
   Every change that touches boot or service startup passes this target before
   commit.
