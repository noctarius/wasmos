## Current System Summary

WASMOS is a minimal x86_64 UEFI-booting microkernel OS hosting a wasm3 WASM
runtime. The system boots into a working user-space stack with an interactive
multi-tty shell, software compositor, PCI-driven driver loading, a FAT
filesystem, and a ring3 isolation baseline.

---

### Boot Flow

- `BOOTX64.EFI` loads `kernel.elf` and `initfs.img` from the ESP, collects the
  UEFI memory map, materializes bootstrap module entries, and exits boot
  services.
- The kernel performs a two-stage bootstrap: low `_start` builds a minimal
  page-table handoff and jumps to higher-half `_start_high`, which clears `.bss`
  and calls `kmain`.
- `kmain` initializes paging, physical memory, exceptions, the scheduler clock
  (PIT or LAPIC timer depending on `WASMOS_IRQ_MODE`), the interrupt controller,
  IPC, and the process manager, then starts the first user-space process.
- A kernel-owned `init` process spawns `device-manager`, waits for FAT
  readiness, then loads `sysinit` from disk via the process manager.
- `sysinit` reads a binary boot-config blob (generated from `scripts/initfs.toml`
  at build time) and starts the configured post-FAT services and user processes.

The early display path uses the kernel framebuffer (from UEFI GOP) for
pre-FAT diagnostics and panic rendering. The native framebuffer driver is loaded
from FAT by `device-manager` after storage is online, at which point it takes
over display output by draining a kernel-managed shared console ring.

---

### Kernel Subsystems

**Paging**
x86_64 4-level paging with a higher-half kernel alias at `0xFFFFFFFF80000000`.
Per-process user CR3 roots with fault-driven mapping of process virtual regions,
guard pages, and stack canaries. User-root verification enforces low-slot
stripping and a constrained kernel transition footprint before CPL3 entry.
User mappings enforce W^X; out-of-slot and permission violations are denied.
User-pointer copy layer (`mm_copy_from_user`/`mm_copy_to_user`) performs
user-range permission checks and temporary CR3-switch-around-copy with a kernel
bounce buffer, so kernel accesses always happen under kernel CR3.

**Physical Memory**
Frame allocator seeded from the UEFI memory map. Several paths still use
hardcoded physical-ceiling constraints; migration to intent-based allocation
(zone-aware, with only true DMA paths constrained to low addresses) is in
progress. Per-context memory regions, MM contexts, and capability state are
list-backed with no fixed `MM_MAX_*` slot limits.
See `docs/architecture/06-memory-management.md`.

**Interrupts**
Three build-time interrupt controller modes selected via `WASMOS_IRQ_MODE`:
legacy 8259 PIC + PIT (mode 0, default), LAPIC-only with LAPIC timer (mode 1),
or LAPIC + I/O APIC with full ISA IRQ routing (mode 2). All modes use the same
IDT layout (vectors 32–47 for IRQs, 0x80 for syscall) and the same IPC-based
IRQ route table. In LAPIC/IOAPIC modes the LAPIC timer fires at vector 32,
replacing the PIT; the I/O APIC is discovered via MADT and programs 16 ISA RTEs
(all initially masked) so drivers can receive interrupts through the existing
`irq_register()` / `irq_ack()` path. Exception stubs cover all standard x86
vectors. Unrecoverable user-mode faults terminate only the faulting process
(exit code -11); kernel-mode faults remain fatal with a framebuffer panic screen
showing exception registers, process identity, and crash metadata.

**IPC Transport**
Fixed-layout 8-field messages (`type`, `source`, `destination`, `request_id`,
`arg0`–`arg3`). Bounded queues protected by spinlocks; endpoint lifecycle is
tied to process reaping. Small control traffic stays in-message; bulk payloads
use buffer-borrow shared handles (`WASMOS_BUFFER_KIND_FS`, etc.) with explicit
grant/release semantics and DMA-mapping extensions.

**Scheduler**
Cooperative with preemption via IRQ0 (PIT or LAPIC timer). Single-core. A ring3-safe timer
trampoline rewrites CPL3 frames (redirecting return RIP to the scheduler
trampoline and rewriting CS to kernel selector) so preemption re-enters ring0
cleanly before context switch. Separate kernel stacks per process with TSS `rsp0`
updated on each dispatch.

**Syscall Boundary**
`int 0x80` DPL3 gate with a minimal dispatcher: `nop`, `getpid`, `exit`,
`yield`, `wait`, `ipc_notify`, `ipc_call`, and thread syscalls. Argument width
checks reject lossy truncation on all current 32-bit field arguments.

**Shared Memory**
Kernel-managed shared-memory registry exposed via `shmem_create/map/unmap`
hostcalls (WASM) and equivalent native-driver ABI hooks, backed by the same
kernel objects. An auto-mapping variant (`shmem_map_auto`) returns a
process-local linear-memory offset from a managed tail window.

**DMA**
Borrow-buffer DMA lifecycle (`dma_map_borrow`, `dma_sync_borrow`,
`dma_unmap_borrow`) with per-driver capability windows (physical range, byte
limit, direction). Storage path integration is complete (ATA attempts a
borrow-based DMA path, falls back to PIO/copy on deny). Packet-side DMA for
networking is deferred to a later phase.
See `docs/architecture/12-dma-transfers.md`.

**Slab Allocator**
A fixed-size slab scaffold (`kalloc_small`/`kfree_small`) is available as an
optional kernel allocation path for incremental migration off ad-hoc static
patterns.

---

### Capabilities and Security Policy

A per-context resource capability registry tracks five kinds:
`io.port`, `irq.route`, `mmio.map`, `dma.buffer`, `system.control`.

Kernel enforces capability checks at every relevant hostcall/syscall entry.
WASMOS-APP metadata supports multi-capability grants per payload; `make_wasmos_app`
validates capability names and flags at pack time (fail-closed). IRQ routing
requires both `irq.route` capability and a per-app IRQ-line allowlist (default
deny). `policy_authorize(context, action, arg0)` centralizes privileged-operation
checks in one kernel path.

---

### Process Model and IPC

The process manager (`proc` endpoint) owns spawn, wait, kill, and status. Native
ELF payloads wrapped in WASMOS-APP as `FLAG_NATIVE|FLAG_DRIVER|FLAG_SERVICE` are
loaded via a kernel function-table ABI. PM bookkeeping is list-backed with
on-demand growth — no fixed slot caps for apps, waits, or services.

A service registry (`register`/`lookup` over `proc`) provides endpoint
discovery. Services send `PROC_IPC_NOTIFY_READY` when initialization is
complete; synchronous spawn variants block the caller until the child signals
ready or a timeout expires.

`libsys` (WASM and native variants) provides intent-keyed request/reply
tracking and type-dispatched handlers for unsolicited traffic, giving services a
single-endpoint event loop without response-stealing or duplicated receive
patterns.

Processes run in states `READY`, `RUNNING`, `BLOCKED`, `ZOMBIE`. Per-context
buffer borrow/release provides bulk data transfer across processes.

---

### Device Discovery and Driver Management

`device-manager` is the central service for hardware discovery and driver
lifecycle. It:
- starts `pci-bus` and waits for PCI scan completion before storage bootstrap
- starts `acpi-bus` for ISA/ACPI device records (serial, keyboard, mouse, RTC)
- drives storage bootstrap, then post-FAT driver loading, via udev-style rules

`pci-bus` enumerates PCI config space in user space and publishes normalized
device records to `device-manager`.

**Rule system** — two policy roots in load order:
1. `/init/devmgr/rules/default.rules` — bootstrap policy from initfs
2. `/boot/system/devmgr/rules/default.rules` — runtime override policy from FAT

Rules use udev-style syntax (`SUBSYSTEM`, `ATTR{...}`, `ENV{MOUNT}`, `RUN+=`).
`SUBSYSTEM=="pci"` rules match by class/subclass/prog_if/vendor/device;
`SUBSYSTEM=="block"` rules match normalized block-unit records and assign mount
aliases; `SUBSYSTEM=="boot"` forces unconditional spawns. Runtime-override rules
are filtered so bootstrap storage drivers (`ata`, `fs-fat`) remain owned by
initfs rules only.

The planned evolution toward full MINIX-style device lifecycle (hotplug,
supervised driver restart, capability manifests) is tracked in
`docs/architecture/15-drivers-and-services.md`.

---

### Storage and Filesystem

- **`ata`**: PIO ATA block driver; registers the `block` endpoint. Supports
  identify and sector reads. A borrow-based DMA path is attempted on each
  transfer, with automatic fallback to PIO/copy on capability deny.
- **`fs-fat`**: FAT12/16/32 driver over the `block` endpoint; registers the `fs`
  endpoint. Supports open/read/seek/stat/readdir, multi-cluster chain traversal,
  LFN directory entries, file writes (overwrite, truncate, append, `O_CREAT`),
  file unlink with cluster-chain reclaim, and directory create/remove.
  Language shims (C libc, Rust, Zig, Go, AssemblyScript) expose matching write,
  append, unlink, mkdir, and rmdir helpers.
- **`fs-init`**: separate initfs backend (`fs.init`) that serves initfs entries
  as normal `open`/`read`/`readdir` file content, so bootstrap policy files
  under `/init` are file-backed rather than hardcoded.
- **`fs-manager`**: namespace router (`fs.vfs`); `fs-fat` backends register
  under it. `/boot` and `/user` are the primary mount aliases; `/user` maps a
  second FAT drive served from `userfs/` in QEMU. Client state is heap-backed
  with no fixed slot cap.

---

### Display, Graphics, and Input

- **Framebuffer driver** (native C): maps framebuffer pages from UEFI GOP into
  the driver process context. Exposes text-control IPC, capability/mode-query
  IPC, and constrained runtime mode switching (PCI/Bochs-VBE variant; UEFI
  variant reports mode-switch as unsupported). Drains the kernel console ring;
  services control IPC ahead of ring drain to avoid switch starvation.
- **VT service** (WASM): four tty slots with active-tty selection via
  `VT_IPC_SWITCH_TTY`. Owns keyboard input routing end-to-end; supports raw and
  canonical line-discipline modes (Backspace, Ctrl+U, Ctrl+C, history via
  Up/Down). Renders a CSI/SGR subset per tty. Queries runtime framebuffer text
  geometry at startup and allocates per-tty cell storage dynamically. Writer
  registration and switch-generation tokens prevent stale-generation writes from
  rendering on a switched tty. `Ctrl+Shift+F1–F4` hotkeys switch directly to
  `tty0–tty3`. `tty0` is the system serial/console-ring live-tail; `tty1+` are
  VT-managed framebuffer sessions.
- **gfx-compositor** (native Zig): software compositor. Manages windows with
  per-window kernel-backed shared buffers, z-order composition, software cursor
  overlay, and window chrome (title bar, close, maximize/restore). Supports
  drag-to-move and corner resize. Delivers typed events to clients via
  `GFX_IPC_POLL_EVENT` (FOCUS, KEY, POINTER, CLOSE_REQUEST, RESIZE). Input
  subscriptions (keyboard, mouse) include runtime recovery for late-started
  drivers. Damaged regions are composited into an internal backbuffer before
  a single scanout copy, reducing visible flicker.
- **font-service** (native Zig): loads TTF fonts from `/boot/system/fonts/`,
  serves open/metrics/glyph-raster IPC with text-run measurement and
  client-buffer rasterization. Used by compositor for title-run caching and by
  `libui` for component text.
- **Keyboard driver** (WASM): PS/2 scan-code decoding; subscriber model
  (`KBD_IPC_SUBSCRIBE_REQ`/`KBD_IPC_KEY_NOTIFY`). Extended ANSI escape
  sequences for arrows and nav/edit keys.
- **Mouse driver** (WASM): PS/2 packet decoding; subscriber model
  (`MOUSE_IPC_SUBSCRIBE_REQ`/`MOUSE_IPC_MOVE_NOTIFY`).
- **RTC driver** (WASM): read/set time IPC; spawned by ACPI match rule.

---

### CLI and User-Space Services

- **`cli`**: interactive shell receiving its VT endpoint from PM wiring. Starts
  on `tty1`; sends output through `VT_IPC_WRITE_REQ`. Up to three concurrent
  instances (`tty1`–`tty3`). Commands include `ls`, `cat`, `cd`, `ps`, `echo`,
  `tty`, `kmaps`, and `kmaps all`. History navigation via Up/Down arrows.
- **`libui`** (WASM): component tree (`Panel`, `Label`, `Button`, `Checkbox`,
  `TextInput`, `ScrollView`, `ListView`) with TTF text rendering via
  `font-service`, pointer focus, key-input editing, clipped viewport rendering,
  and drag scrolling. All component, text, and list-item storage is heap-backed.
- **`gfx-smoke`** app: manual compositor validation at `/boot/apps/gfx_smoke.wap`.
  Covers two concurrent windows, close-event teardown, and a `libui` component
  demo. Not auto-spawned by `sysinit`.
- **`virtio-serial`** driver (WASM): PCI-matched driver for `virtio.serial`
  (device IDs 0x1003/0x1043, vendor 0x1AF4). Provides PCI discovery and
  register-access IPC. Serves as the transport pattern baseline for higher-level
  virtio consumers (including the planned `virtio-net`).

---

### Ring3 Isolation

Ring3 execution is implemented and validated end-to-end:
- CPL3 entry via `iretq` with user CS/SS selectors; kernel resume via `iretq`
  for ring0 contexts.
- Syscall `ipc_call` reply correlation survives out-of-order message arrival
  (bounded per-process pending queue) and authenticates reply source endpoint
  and owner context before accept.
- All CPL3 exception types are contained to the faulting process: `#PF`, `#GP`,
  `#UD`, `#DE`, `#DB`, `#OF`, `#NM`, `#SS`, `#AC`.
- Low-slot stripping enforced before CPL3 entry; user-root paging verifier checks
  allowed PML4 slots and constrained higher-half window footprint.
- Strict ring3 test target (`run-qemu-ring3-test`) exercises: syscall paths,
  IPC deny/allow, fault injection across exception types, preemption stress, PM
  owner-deny inject paths, and native ABI smoke.

See `docs/architecture/11-ring3-isolation-and-separation.md` for details and
remaining hardening work.

---

### Threading

In-kernel thread support is production-complete for the current single-core scope:
- `THREAD_CREATE`, `THREAD_EXIT`, `THREAD_JOIN`, `THREAD_YIELD`, `THREAD_SLEEP`
  syscalls.
- Per-thread kernel stacks; scheduler tracks thread identity separately from
  process identity.
- Blocking IPC operations block only the calling thread.
- SMP support is future work.

See `docs/architecture/08-threading-and-lifecycle.md`.

---

### Networking

Networking is in the design phase. `virtio-serial` proves the PCI-matched WASM
driver transport pattern. No networking driver or stack service is implemented
yet. The full design — `virtio-net` driver, user-space lwIP stack service, socket
IPC contract, IPv4/IPv6, multi-address, and multi-stack-instance model — is in
`docs/architecture/22-networking-virtio-net-and-stack.md`.

---

### Known Deferred Items

- `fs-fat` update modes (`r+`/`w+`/`a+`) and non-ASCII LFN creation.
- Intermittent framebuffer prompt duplication artifact during rapid tty switching
  (not reproducible in recent runs; deferred until stable repro is available).
- Full IOMMU (VT-d/AMD-Vi) for DMA isolation (capability-window enforcement is
  the current substitute).
- Hotplug and driver supervision/reincarnation.
- SMP scheduling.
