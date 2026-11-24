## Capability and Policy System

> **Documentation status: Implemented reference with deferred IOMMU work.**

This document describes the kernel capability model: the capability kinds,
per-context storage, the spawn profile and who may request one, capability
enforcement at each action site, and the IRQ-route policy table.

**Sources**: `src/kernel/capability.c`, `src/kernel/policy.c`,
`src/kernel/include/capability.h`, `src/kernel/include/policy.h`

---

### Overview

Capabilities are per-context boolean grants tracked in a kernel-side list.
A process cannot use a hardware resource (I/O ports, MMIO, IRQ lines, DMA
buffers, system-control operations) unless the kernel has granted it the
corresponding capability. Capability checks are enforced at the hostcall
layer before any hardware access occurs.

There are two levels of capability:

1. **Presence check** — `capability_has(context_id, kind)`: does the context
   hold the capability at all?
2. **Spawn profile** — per-context constraints optionally applied after the
   presence check: register windows, the interrupts the context may route,
   DMA direction, DMA byte limit, and DMA physical windows.

The kernel context (id 0) is given all capabilities at `capability_init()`.
All other contexts start with no capabilities.

---

### Capability Kinds

```c
typedef enum {
    CAP_IO_PORT           = 0,   // mask bit 0: access x86 I/O ports
    CAP_IRQ_ROUTE         = 1,   // mask bit 1: route hardware IRQ lines to an endpoint
    CAP_MMIO_MAP          = 2,   // mask bit 2: map MMIO physical regions
    CAP_DMA_BUFFER        = 3,   // mask bit 3: use PM DMA buffer hostcalls
    CAP_SYSTEM_CONTROL    = 4,   // mask bit 4: system halt, reboot, power-off
    CAP_SUBSYSTEM_REGISTER= 5,   // mask bit 5: register broker subsystems + exec-format handlers
    CAP_SVC_CLASS_REGISTER= 6,   // mask bit 6: register a service under a virtual class
    CAP_CONSOLE_OWNER     = 7,   // mask bit 7: register the console ring
    CAP_SPAWN_GRANT       = 8,   // mask bit 8: put hardware grants on a spawned child
    CAP_SVC_CONTROL       = 9,   // mask bit 9: submit, kill, declare the boot set (supervisor)
    CAP_PCI_CONFIG        = 10,  // mask bit 10: PCI config space (pci_config_read/write)
    CAP_RTC_EFI           = 11,  // mask bit 11: the UEFI clock (rtc_efi_get/rtc_efi_set)
} capability_kind_t;

#define CAPABILITY_KIND_COUNT 12u
#define CAP_ALL_MASK  ((1u << 12) - 1u)   // every kind
#define CAPABILITY_DMA_WINDOW_LIMIT 16u
```

Capability names used in `linker.metadata` capability declarations map to
these kinds via `capability_grant_name`:

| Metadata name          | Kind granted             |
|------------------------|--------------------------|
| `"io.port"`            | `CAP_IO_PORT`            |
| `"irq.route"`          | `CAP_IRQ_ROUTE`          |
| `"mmio.map"`           | `CAP_MMIO_MAP`           |
| `"dma.buffer"`         | `CAP_DMA_BUFFER`         |
| `"system.control"`     | `CAP_SYSTEM_CONTROL`     |
| `"subsystem.register"` | `CAP_SUBSYSTEM_REGISTER` |
| `"svc.class"`          | `CAP_SVC_CLASS_REGISTER` |
| `"console.owner"`      | `CAP_CONSOLE_OWNER`      |
| `"spawn.grant"`        | `CAP_SPAWN_GRANT`        |
| `"svc.control"`        | `CAP_SVC_CONTROL`        |
| `"pci.config"`         | `CAP_PCI_CONFIG`         |
| `"rtc.efi"`            | `CAP_RTC_EFI`            |

The kind values are ABI: `abi/constants.yaml` publishes them as `WASMOS_CAP_*`,
and `capability.h` asserts that the enum matches. Any unrecognized name returns
-1 and grants nothing. `CAP_SVC_CLASS_REGISTER`
gates registering a service under a virtual class (class-based discovery,
[Process and IPC](09-process-and-ipc.md)); the plain name registration path is
ungated. `CAP_CONSOLE_OWNER` gates `klog_register_ring`: the console ring
receives every tty's output and the kernel log, and its registrant owns the console UART (COM1 on x86_64, the PL011 on aarch64)
([Virtual Terminal](19-virtual-terminal.md)); only the vt declares it.

`CAP_SPAWN_GRANT` and `CAP_SVC_CONTROL` gate requests rather than hardware;
see "Requester Checks" below. `CAP_PCI_CONFIG` gates the
`pci_config_read` / `pci_config_write` host calls; only pci-bus declares it.
Without it, a process may still READ the config space of the one function it
owns -- the one its spawn profile names, until its reap retires it -- which is
how a virtio-modern driver finds its register blocks; writing stays
pci-bus-only (`pci_config_guest_read`, `src/kernel/pci_config.c`).
`CAP_RTC_EFI` gates the `rtc_efi_get` / `rtc_efi_set` host calls, which run
the firmware's GetTime and SetTime (`src/kernel/efi_rtc.c`); only the rtc_efi
driver declares it. It is its own kind, not `system.control`, so the clock
driver cannot also halt or reboot the machine.

---

### Per-Context State

```c
typedef struct {
    uint32_t context_id;
    uint8_t  configured;               // 1 once any capability has been granted
    uint8_t  spawn_profile_configured; // 1 once capability_set_spawn_profile called
    uint32_t region_count;
    capability_region_t regions[CAPABILITY_REGION_LIMIT]; // register windows, by region index
    uint32_t irq_count;
    uint32_t irqs[WASMOS_IRQ_GRANT_LIMIT]; // interrupt identifiers the context may route
    uint32_t dma_direction_flags;      // WASMOS_DMA_DIR_* bitmask
    uint32_t dma_max_bytes;
    uint32_t dma_window_count;
    wasmos_dma_window_t dma_windows[CAPABILITY_DMA_WINDOW_LIMIT];
    uint32_t mask;                     // active capability bits
} capability_context_state_t;
```

These records are stored in `g_cap_ctx`, a list backed by the array-chunk
allocator (`LIST_IMPL_ARRAY_CHUNK`, initial capacity 16). `capability_init()`
pre-allocates the kernel context record (id 0) with `mask = CAP_ALL_MASK`.

Lookup is a linear scan; context IDs are assigned once at spawn and are
stable for the lifetime of the process.

---

### Granting Capabilities

`capability_grant_name(context_id, name, name_len, flags)`:
- Called from the process manager during spawn, after reading the app manifest.
- Maps the name string to a mask bit.
- ORs the bit into `ctx->mask` and sets `ctx->configured = 1`.
- Fails (returns -1) for unknown capability names.

`capability_set_spawn_profile(context_id, cap_flags, region_count, regions, irq_count, irqs, dma_direction_flags, dma_max_bytes, dma_window_count, dma_windows)`:
- Called during spawn when the requester supplies a capability descriptor
  (`wasmos_spawn_caps_v2_t`, the only form a spawn grants hardware access
  with).
- `irqs` lists interrupt identifiers: the architecture's own name for an
  interrupt, the value the `irq_route_ipc`/`irq_ack`/`irq_unroute`/
  `irq_configure` host calls take as `irq_line` -- an ISA line (0..15) on
  x86_64, a GIC SPI INTID (32..1019) on aarch64. At most
  `WASMOS_IRQ_GRANT_LIMIT` (4), and only with bit 2 (`irq`); a list without
  the bit refuses the profile, like a region without the bit of its kind.
- Stores fine-grained constraints for later per-operation enforcement.
- `cap_flags` is a `DEVMGR_CAP_*` mask.
- `regions` are the register windows, in region-index order (see Register
  Windows below). Each needs the bit of its kind: bit 0 (`io.port`) for an I/O
  window, bit 1 (`mmio.map`) for a memory-mapped one.
- `cap_flags` bit 2: enable IRQ mask restriction.
- `cap_flags` bit 3: enable DMA direction/range restriction.
- If DMA is enabled, `dma_window_count` must be `> 0` and `≤
  CAPABILITY_DMA_WINDOW_LIMIT`; the window array is copied in full.
- Over-declared or malformed input is refused whole, never truncated.

### Register Windows

A driver addresses its device's registers as `(region, offset)` through the
`io_region_in*/out*` host calls and never names an absolute port or physical
address. `region` is an index into the profile's windows in the order the
driver declared them (`[[regions]]`, doc 13), so a window's position IS its
identity: a profile is never reordered or shortened, and a window the kernel
cannot honour exactly refuses the whole spawn rather than being dropped, since
dropping one would renumber every window after it.

A window (`wasmos_region_grant_t`) is one of two kinds:

| Kind                       | `base`            | `length` | Access                         |
|----------------------------|-------------------|----------|--------------------------------|
| `WASMOS_REGION_GRANT_IO`   | first I/O port    | ports    | one `in`/`out` of the width    |
| `WASMOS_REGION_GRANT_MMIO` | physical address  | bytes    | one load/store of the width    |

A memory-mapped window is page-aligned and outside system RAM. The process
manager maps it into the kernel (`device_window_map`, doc 06) before it
installs the profile, and the host call reaches the register through that
mapping, uncached; the driver holds no mapping of its own. An access to a
memory-mapped window must be naturally aligned (`WASMOS_ERR_IO_MISALIGNED`).

I/O ports are an x86 notion. Neutral kernel code reaches them only through
`arch/arch_io.h`; on an architecture without a port space every port access --
an I/O window, and the raw `io_in*` / `io_out*` / `io_wait` host calls -- is
refused with `WASMOS_ERR_IO_NO_PORT_SPACE`, so a driver there is given
memory-mapped windows only.

`capability_region_resolve` is the single resolution point: it bounds-checks
the WHOLE access against the window and answers the port or kernel VA, or a
packed `WASMOS_ERR_IO_*` code naming the refusal (no windows, no such region,
out of window, misaligned).

---

### Requester Checks

A spawn profile is authority the *requester* chooses: the manifest grants a
kind, the profile picks the I/O windows, IRQ lines and DMA windows -- including
DMA windows over arbitrary physical memory. So who may request one is checked:

- **Process manager.** Every spawn that carries a profile
  (`PROC_IPC_SPAWN_CAPS_V2`, and `PROC_IPC_SPAWN_WATCHED` with a capability
  descriptor; the packed forms that carried a grant in argument words are
  retired) asks `pm_caps_grant_allowed` before the child is created. A profile with any
  `cap_flags` bit set requires the requester to hold `spawn.grant`; otherwise
  the request is refused with `GRANT_NOT_AUTHORIZED` (`proc_pm`, or
  `proc_spawn` for a watched spawn) and nothing is spawned. A profile with no
  bits set only narrows the child and is open to everyone. Holders:
  device-manager and the supervisor.
- **Supervisor.** Submitting, killing, retracting, stopping and starting a unit
  and declaring the boot set require `svc.control` (`WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED`); listing is
  open. A submission carrying a capability descriptor also requires
  `spawn.grant` (`WASMOS_ERR_SUPERVISOR_GRANT_NOT_AUTHORIZED`): the supervisor
  spawns the unit under its own name, so it relays grants only from a submitter
  the process manager would accept them from directly. Holders of
  `svc.control`: sysinit, device-manager and the `svc` utility.

A user-space server learns what a sender holds with the
`ipc_endpoint_has_cap(endpoint, cap)` host call, asked about the request's
source endpoint: the transport refuses a source the sender does not own, so its
owner is the sender. The answer is about the owner when asked; a sender that
has exited answers `WASMOS_NOENT`.

A capability is held because the module's manifest declares it, and nothing
checks that the image is entitled to declare it: any `.wap` on any path --
`/boot`, `/user` and `/wfs` are writable -- can declare `spawn.grant`,
`svc.control` or `system.control` and be spawned with them by any process. So
the requester checks keep a process from exercising an authority it does not
hold, but not from spawning an image that does: a privileged utility such as
`svc` is a deputy any process can start with arguments of its choosing. Closing
that needs a capability policy over images (`docs/TASKS.md`).

---

### Device Quiesce

A spawn profile may name the PCI function its device sits at
(`DEVMGR_CAP_PCI_FUNCTION`, `wasmos_spawn_dma_caps_t.pci_function`; only the
descriptor forms can carry it, and the packed spawn forms refuse the bit).
Naming one is a grant like any other cap bit, so it needs `spawn.grant`. The
kernel records the process as the function's driver (`pci_function_attach`)
and, when the process ends, quiesces that function (`pci_quiesce_context`, `src/kernel/pci_config.c`):
Bus Master Enable cleared, INTx Disable set, the MSI and MSI-X enable bits
cleared, then a read-back so the writes have reached the device. This runs
first in the reap -- before the spawner's `PROC_IPC_CHILD_EXITED` is posted and
before the context's IRQ routes, MSI vectors, DMA regions and borrows are
released -- so no frame the device could still write is recycled while it can,
and no supervisor restarts a driver for a device that is still mastering. It
logs `[pci] quiesced <bb>:<dd>.<f>: bus mastering and MSI off for pid <n>`.

The device's own state is not reset: its queues still name the old frames, so
a new driver resets the device before it turns bus mastering on again (an MSI
bind through pci-bus does that). A driver that takes no MSI stops its own DMA
engine and then asks pci-bus for bus mastering (`PCI_IPC_BUS_MASTER_REQ`; ata
does). A driver that falls back to INTx resets the device and asks pci-bus to
arm the function for its pin (`PCI_IPC_INTX_ENABLE_REQ`: bus mastering on,
INTx Disable cleared), and puts no work on the device until the answer
(virtio-blk, virtio-rng, virtio-net). A refusal, or a request that cannot be
sent, ends the driver after it resets the device: the function cannot be
armed, and the supervisor's restart is the retry.

Config space is reached through the kernel only: `pci_config_read` /
`pci_config_write` (`pci.config`) hold the same lock as the quiesce across the
0xCF8/0xCFC address/data pair.

A function is *unmanaged* until a spawn profile names it, *owned* by that
process while it lives, and *retired* once its reap has quiesced it, until a
new driver names it. pci-bus arms a function -- MSI and MSI-X enable, MSI
address and data, bus mastering (with an MSI bind, `PCI_IPC_BUS_MASTER_REQ` or
`PCI_IPC_INTX_ENABLE_REQ`), INTx enable (`PCI_IPC_INTX_ENABLE_REQ`)
-- only on behalf of the requesting driver:
`pci_function_check(endpoint, bdf)` before it touches the binding, and
`pci_config_write_for(endpoint, bdf, ...)` for each arming write, which the
kernel performs only while the endpoint's owner drives the function, judged
under the quiesce's lock. A bind a driver sent before it died therefore cannot
re-arm the device after the reap, and no other process can drive an owned
function (`WASMOS_ERR_MSI_NOT_FUNCTION_DRIVER`). A function no profile named
stays open to its first claimant, as before. When a binding passes from a dead
driver to its replacement, pci-bus masks the dead driver's MSI-X entries first,
since their vectors went back to the kernel at its reap.

---

### Capability Checks

`capability_has(context_id, kind)` — basic presence check. Returns 1 if the
mask bit is set, 0 otherwise.

`capability_io_port_allowed(context_id, port)` — returns 1 if the port lies
inside one of the spawn profile's I/O windows. Returns 0 if no profile is
configured (permissive in that case, since presence is checked separately).

`capability_region_resolve(context_id, region, offset, width, out)` — see
Register Windows above.

`capability_irq_line_allowed(context_id, irq_line)` — returns 1 if the spawn
profile's interrupt list names `irq_line`. Which identifiers exist is the
architecture's: its `irq_register` refuses one it does not route.

`capability_mmio_allowed(context_id)` — returns 1 if spawn profile is
configured and `CAP_MMIO_MAP` bit is set.

`capability_dma_direction_allowed(context_id, direction_flags)` — returns 1
if `cap_flags` bit 3 was set during spawn profile configuration and the
requested flags are a subset of `ctx->dma_direction_flags`.

`capability_dma_range_allowed(context_id, base, length)` — returns 1 if the
range `[base, base+length)` is fully contained within any one of the
registered DMA windows.

`capability_dma_max_bytes(context_id)` — returns the maximum DMA transfer
size allowed, or 0 if no profile.

---

### Policy Enforcement

**Source**: `src/kernel/policy.c`

`policy_authorize(context_id, action, arg0)` is the single entry point called
by hostcall handlers before performing privileged operations:

```c
typedef enum {
    POLICY_ACTION_IO_PORT,        // arg0 = port number
    POLICY_ACTION_MMIO_MAP,       // arg0 unused
    POLICY_ACTION_DMA_BUFFER,     // arg0 unused
    POLICY_ACTION_IRQ_CONTROL,    // arg0 unused (capability presence only)
    POLICY_ACTION_SYSTEM_CONTROL, // arg0 unused
    POLICY_ACTION_IRQ_ROUTE,      // arg0 = irq_line
} policy_action_t;
```

| Action           | Check sequence                                                                           |
|------------------|------------------------------------------------------------------------------------------|
| `IO_PORT`        | 1. `capability_has(CAP_IO_PORT)` 2. If spawn profile: `capability_io_port_allowed(port)` |
| `MMIO_MAP`       | 1. `capability_has(CAP_MMIO_MAP)` 2. If spawn profile: `capability_mmio_allowed()`       |
| `DMA_BUFFER`     | `capability_has(CAP_DMA_BUFFER)` only                                                    |
| `IRQ_CONTROL`    | `capability_has(CAP_IRQ_ROUTE)` only                                                     |
| `SYSTEM_CONTROL` | `capability_has(CAP_SYSTEM_CONTROL)` only                                                |
| `IRQ_ROUTE`      | 1. `policy_authorize(IRQ_CONTROL)` 2. `policy_irq_route_allows(context_id, irq_line)`    |

`IO_PORT` and `MMIO_MAP` permit access with no spawn profile (capability
presence is sufficient when the app has not declared fine-grained constraints).

#### IRQ Route Policy

`policy_irq_route_allows()` is two-tier:

1. If the context has a spawn profile, use `capability_irq_line_allowed` (the
   profile's interrupt list is the policy).
2. Otherwise, fall back to the static name-based whitelist:

```c
static const irq_route_policy_t g_irq_route_policy[] = {
    { "ata",             2u, {14u, 15u} },  // ATA primary/secondary
    { "irq-route-allow", 1u, {1u, 0u}   },  // test app for IRQ 1
};
```

A process whose name is not in the whitelist and has no spawn profile cannot
route any IRQ line, even if it holds `CAP_IRQ_ROUTE`. This is an explicit
default-deny policy for user-space IRQ routing.

The kernel context bypasses all policy checks (`context_id == IPC_CONTEXT_KERNEL`
short-circuits to return 1).

---

### Enforcement Points

| Hostcall                               | Check                                               |
|----------------------------------------|-----------------------------------------------------|
| `wasmos_io_in8/out8` (I/O port access) | `POLICY_ACTION_IO_PORT` with port number            |
| `wasmos_io_out32/in32`                 | `POLICY_ACTION_IO_PORT`                             |
| `wasmos_mmio_map`                      | `POLICY_ACTION_MMIO_MAP`                            |
| `wasmos_dma_map_borrow`                | `POLICY_ACTION_DMA_BUFFER` + direction/range checks |
| `wasmos_irq_route_ipc`                 | `POLICY_ACTION_IRQ_ROUTE` with IRQ line             |
| `wasmos_irq_ack`                       | Verifies `owner_context_id` in route table          |
| `wasmos_proc_exit` / halt              | `POLICY_ACTION_SYSTEM_CONTROL`                      |

---

### Structural Invariants

1. **Kernel context is omnipotent.** Context id 0 holds every capability bit
   at init and bypasses the IRQ policy table.

2. **Spawn profile is optional but binding once set.** Without a profile,
   only capability presence is checked (permissive). Once set, the profile
   enforces strict per-operation limits that cannot be widened by the process.

3. **IRQ routing requires two passes.** `CAP_IRQ_ROUTE` grants the right to
   route IRQs in principle; `policy_irq_route_allows` then checks which
   specific lines are permitted. Both must pass.

4. **DMA capability is presence-only at the policy layer.** Fine-grained
   DMA direction, range, and size constraints are enforced by the DMA hostcall
   layer itself (`xfer_buffer_dma_map`) using capability queries,
   not by `policy_authorize`.

5. **All capability state lives in the kernel.** WASM processes cannot read
   or modify capability records. The capability list is in kernel memory and
   is never exposed to WASM linear memory.
