# Current Status

This is a compact implementation snapshot for agents, not a changelog. Use
`git log` for history, `docs/ARCHITECTURE.md` for the document map, and the
linked feature documents for rationale and rollout plans.

## Snapshot and Validation

### Networking

- `virtio-net` now publishes `net.ifc` and reports link changes with
  `NETDRV_IPC_LINK_NOTIFY`. `net-stack` consumes class enumeration/events,
  retaining name lookup only as compatibility fallback.
- The current binding lifecycle is explicit: discovered → buffers granted →
  link queried → netif up. A bounded four-frame TX queue prevents a single
  driver request from making lwIP output fail immediately.
- Interface storage is a fixed eight-slot table; each slot owns its stable
  lwIP `netif` object and provider identity rather than sharing a global netif.
- UDP ring records now include IPv4 address/port metadata. Bound UDP sockets
  can provide a destination record for unconnected sendto-style transmission;
  received records retain their source endpoint.

- Default configuration: wasm3 runtime, ring-3 isolation, single CPU. WARP is
  selected with `-DWASMOS_WASM_RUNTIME_WARP=ON`; SMP is separately gated by
  `WASMOS_SMP` and requires IOAPIC.
- Recent baseline: wasm3 and WARP+SMP boot through `init` to the CLI/halt path;
  the WARP build uses AOT payloads for internal non-native modules and JIT for
  examples and graphical apps.
- Primary regression gates: `run-kernel-unit-tests`, `run-qemu-test`,
  `run-qemu-cli-test`, `run-qemu-ring3-test`, and (when applicable)
  `run-qemu-ring3-threading-test`. Never run QEMU integration targets in
  parallel because they share `build/esp`.
- Developer checks: `fmt`, `fmt-check`, `lint`, and `quality` are CMake targets
  backed by `scripts/quality.sh`; only first-party source roots are in scope.

## Boot and Kernel

- Boot handoff is stable: `BOOTX64.EFI` loads `kernel.elf` and `initfs.img`,
  exits boot services, then enters `_start` and `kmain(boot_info_t *)`.
- The kernel provides paging, preemptive thread scheduling, process lifecycle,
  IPC, capabilities, transfer buffers, shared memory, DMA policy, and panic
  diagnostics. Fatal exception reports retain the fault-time frame chain and
  resolve in-kernel symbols; `scripts/decode_kernel_panic.py` adds host-side
  file/line resolution.
- Scheduling is thread-centric. Ring-3 thread creation/join/detach/yield/exit
  and cooperative user-space reentrant mutexes are implemented. SMP has AP
  bring-up, per-CPU state, a shared ready queue, and hardening for cross-CPU
  wake/reap/context races.
- Process-manager state and core MM registries use dynamic/list-backed storage
  rather than small fixed process/context/region tables.

## Runtime, Isolation, and IPC

- wasm3 is the default interpreter. WARP is the optional JIT/AOT backend and
  follows the ring-3 execution model; internal modules can fall back to JIT if
  an embedded AOT payload cannot load.
- Both runtimes use reserve-and-commit linear memory and the same user virtual
  address model. Linear-memory metadata is rebound to live backing before
  pointer-validating hostcalls; memory is reclaimed correctly on process reap.
- The transfer-buffer object model is canonical: objects have an owner,
  explicit borrow/grant lifecycle, and `(buffer_id, ptr, len, offset)` ABI.
  Keep libc/libsys wrappers and all runtime variants in sync when changing it.
- Startup data is supplied through spawn-info buffers, not legacy entry args.
  Path spawning first recognizes `.wap`; executable-format brokers can return
  a validated `.wap` launch plan for other formats.
- Service discovery supports named services and class instances. Multi-instance
  providers must use unique class instances and unique concrete PM names.

## Filesystems and Storage

- `fs-manager` is the VFS endpoint and routes `/init`, `/boot`, and `/user`.
  `fs-init` serves initfs; FAT backends mount block volumes for `/boot` and
  optional `/user`.
- `fs-fat` is a single-threaded, non-blocking reactor: queued operation
  contexts are resumable stackless coroutines, while one active operation uses
  the shared 8 KiB block/DMA buffer. It supports FAT12/16 and LFN lookup across
  multi-cluster directories, reports `FS_ERR_*`, and binds to its requested
  block-device unit.
- `block_buffer_map` overlays a caller block buffer into linear memory so FAT
  I/O normally avoids staging copies. Bounds checks limit legacy copy/write
  calls to the live block slot.
- Current limitations: ATA remains PIO-only; `fs-init` has not yet adopted the
  reactor model; initfs whole-blob mapping is still copy-based.

## Services and System Startup

- Startup order is `init` -> `fs-manager`/`fs-init` -> `device-manager` ->
  `sysinit`. Readiness gating prevents dependent boot steps from racing ahead.
- `device-manager` consumes PCI/ACPI inventory and bootstrap/runtime rules,
  spawning drivers with matched capabilities and optional startup identity.
- `chardev` is a normal initfs WASM driver started by a boot rule and registered
  through the service registry; it is no longer kernel-embedded.
- `sysinit` drives `.rc` workflows, environment scopes, `script`/`source`, and
  starts the compositor-dependent graphical apps before the CLI when enabled.
- Native services use explicit cancellation for stack-backed synchronous IPC
  waits. `font-service` provides TTF measurement/rasterization for compositor
  and libui clients.

## Drivers and Hardware

- PCI and ACPI bus services enumerate devices for policy-driven startup. Active
  device coverage includes ATA, FAT, framebuffer, PS/2 keyboard/mouse, serial,
  RTC, `virtio-serial`, `virtio-rng`, and `virtio-net`.
- Capability policy covers I/O ports, IRQs, shared memory, and DMA in both
  runtimes. Driver-owned pinned DMA regions and a transport-neutral `vring`
  core support virtqueues.
- `virtio-net` initializes RX/TX queues, routes its IRQ, exchanges an ARP
  self-probe through QEMU SLIRP, and supports pull plus notification-hinted RX
  delivery. The current INTx electrical configuration is incomplete, so
  consumers must still poll defensively after notification.
- `virtio-rng` registers the `hrng` service class and fills caller-owned
  transfer buffers. WASM and native `libsys` expose callback-based byte-array,
  raw-`uint32_t`, and `[0, 1)` float requests through their event loops.
- `net-stack` is a native lwIP baseline: it discovers `virtio.net`, reads MAC
  and link state, configures `eth0` as `10.0.2.15/24` with gateway `10.0.2.2`,
  and bridges Ethernet frames through driver-granted transfer buffers. RX uses
  poll plus notification hints, and the idle loop advances lwIP timeouts. It
  starts from the device-manager boot rule, registers `net.stack`, and discovers
  `virtio.net`/`hrng` through asynchronous control-endpoint requests while
  draining socket IPC. It seeds `LWIP_RAND()` from the `hrng` service when
  available and maps the persistent TX/RX
  descriptor for its IPv4 UDP/TCP PCB control plane. The versioned ring-backed socket-pool core (`socket.c`)
  validates the persistent TX/RX grant descriptor, attaches rings, and
  exercises lifecycle transitions in a host unit test. Connected UDP sockets
  now drain TX datagram records into `udp_sendto` and write receive callbacks
  back into the RX ring with `NET_IPC_RX_NOTIFY`; the SLIRP UDP echo test covers
  that route. `ringbuf.h` and the
  wasm3/WARP pinned shared-memory mapping baseline are ready for the planned
  per-socket shared-memory data plane; that mapping must not be revalidated as
  a networking prerequisite unless its implementation changes.

## Graphics and User Interface

- The graphics stack comprises framebuffer driver, software compositor,
  shared-buffer windows, input routing, clipping/damage redraw, window chrome,
  cursor, and system/menu bars. Window flags are composable (`TOPMOST`,
  `NO_CHROME`, `INVISIBLE`, `NO_ACTIVATE`, `NO_CONTENT`, and related flags).
- `libui` has one canonical tree at `src/libui/`. Its component base owns common
  tree state; component vtables own type-specific layout, render, event, popup,
  and destruction behavior. Existing consumers are `gfx_smoke` and `menu_bar`.
- `gfx_smoke` exercises multiple windows, close-event teardown, and libui;
  `menu_bar` exercises popup/window interactions. Both are spawned after the
  compositor is ready when present in `sysinit`.

## Applications and Language Support

- `.wap` packages cover WASM and native apps, services, and drivers. C, C++,
  Zig, Go, Rust, and AssemblyScript examples are supported through shared libc
  and runtime-specific libsys wrappers.
- CLI path spawns retain transfer buffers until the matching PM response;
  foreground/background launches and broker handoff use the same ownership
  contract.
- Rust and Go shims use the transfer-buffer object ABI. Minimal WARP WASI
  compatibility includes `proc_exit` and `random_get` for TinyGo workloads.

## Current Gaps and Guardrails

- Shared WARP linear-memory updates need a real cross-CPU TLB shootdown; the
  current fault-path retry is an interim SMP safeguard.
- WARP still has incomplete hostcall coverage, no working multithreaded WASM,
  and an internal shim used to access a vendored runtime pointer. Do not modify
  `libs/warp` or `libs/wasm3` directly.
- Complete PCI INTx polarity/trigger configuration before treating RX
  notifications as reliable push delivery.
- Networking Phase 2 remains pending: end-to-end ARP/ICMP/UDP service
  validation, socket data callbacks, and shared-memory ring wiring.
- Maintain the boot entry contract, C ABI boundaries, and runtime-wrapper
  parity. Record meaningful future baseline changes here as concise subsystem
  updates; keep detailed design changes in `docs/architecture/`.
