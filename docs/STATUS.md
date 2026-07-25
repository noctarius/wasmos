# Current Status

This is a compact implementation snapshot for agents, not a changelog. Use
`git log` for history, `docs/ARCHITECTURE.md` for the document map, and the
linked feature documents for rationale and rollout plans.

## Snapshot and Validation

### Networking

- `make_initfs` explicitly depends on the packed `net_stack.wap`, so net-stack
  changes rebuild the initfs payload that boot actually executes.
- `net-stack` uses a dedicated virtio-net reply endpoint. RX delivery drains
  notification-reported queued frames immediately, while empty polls are paced
  at a 12 ms timer cadence to avoid a request/reply busy loop.
- When its coroutine runtime has no runnable work, net-stack now blocks on a
  select set covering its public, control, and driver-reply endpoints. The
  service pump invokes this idle wait on the kernel-thread stack (never a
  suspended coroutine stack), with a bounded timeout so lwIP timers and RX
  polling continue to advance. The native-driver ABI exposes the corresponding
  select hooks; the kernel build explicitly depends on that ABI header so a
  header change cannot leave `native_driver.o` stale.
- `virtio-net` now publishes `net.ifc` and reports link changes with
  `NETDRV_IPC_LINK_NOTIFY`. `net-stack` consumes class enumeration/events,
  retaining name lookup only as compatibility fallback.
- A `NETDRV_IPC_LINK_NOTIFY` updates the existing lwIP `netif` in place
  (`netif_set_link_up`/`_down`) and emits an observable `[net-stack] link
  up|down` line; it never re-registers or rebinds the interface. The test
  framework can drive this end-to-end with `QemuSession.set_link()` (QEMU
  monitor `set_link`, requires `enable_monitor`); `test_net_stack_link_notify_e2e`
  toggles the link down/up and asserts the same instance survives without a
  re-register or bring-up banner.
- The current binding lifecycle is explicit: discovered → buffers granted →
  link queried → netif up. A bounded four-frame TX queue prevents a single
  driver request from making lwIP output fail immediately.
- Interface storage is a fixed eight-slot table; each slot owns its stable
  lwIP `netif` object and provider identity rather than sharing a global netif.
- UDP ring records now include IPv4 address/port metadata. Bound UDP sockets
  can provide a destination record for unconnected sendto-style transmission;
  received records retain their source endpoint.
- Interface addressing is declarative: net-stack reads
  `/boot/system/net/interfaces` (a minimal `/etc/network/interfaces` subset,
  `iface <name> inet <dhcp|static>`) when the interface comes up. DHCP is
  enabled (`LWIP_DHCP`); static applies `netif_set_addr`, dhcp calls
  `dhcp_start`. Policy is strict: a missing/unreadable file or a DHCP no-lease
  leaves the interface link-up but unconfigured (bounded retry absorbs the
  storage-mount delay). The `... ready` banner and gateway ARP fire from the
  netif status callback once an address is actually assigned.
- DNS is enabled (`LWIP_DNS`); the DHCP client installs the leased resolver
  (option 6) automatically. An optional ifcfg `dns-nameservers <ip> [<ip2>]`
  line (up to two servers) overrides that: net-stack re-applies it from the
  netif status callback once an address is assigned, so it wins over DHCP; with
  static addressing it is the only resolver source. Omit it to keep DHCP's.
- `net-stack` registers its PUBLIC endpoint as `net.stack`; client
  socket/ifaddr requests are dispatched there (registering the control endpoint
  had silently dropped them via the async event loop).
- `NET_IPC_IFADDR_ADD/DEL/LIST`, `NET_IPC_IF_SET_STATE`, and `NET_IPC_DHCP_SET`
  are implemented; the `/system/utils/ip` tool (`ip addr show|add|del`,
  `ip dev <name> up|down`, `ip dhcp <name> on|off`) inspects and edits interface
  addressing at runtime. `ip dhcp <name> on` clears any static address and
  (re)starts the lwIP DHCP client; `off` stops it and leaves the current address
  in place. lwIP is IPv4-only with a single address per netif, so static
  addressing and DHCP are mutually exclusive on an interface (no address aliases).

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

- Native `libsys` now includes a caller-storage, single-worker stackful
  coroutine core and local future/promise state. The target backend is x86-64
  SysV; an AArch64 AAPCS64 backend runs the same runtime behavior in native
  ARM64 host tests. It is currently linked by the native net-stack package and
  exposes matching C and Zig wrappers for spawn, async-task start, cooperative
  yield, await/resolve/reject, and join.
  Caller-owned `future_then` registrations schedule separate success/error
  callbacks through the runtime rather than invoking them inline, and return a
  caller-owned child future for value-transforming, rejection-propagating
  chains. Caller-owned `race`/`all` groups and variadic C macros provide
  first-outcome and all-success aggregation; once the group future settles the
  runtime unlinks the remaining source continuations, so group storage only
  needs to stay live until the group future settles, not until every input does.
  The host unit suite exercises both the C runtime and native Zig wrapper,
  including `asyncStart`, chained callbacks, slice-based `race`/`all`, and
  invalid slice contracts.
  Native `wasmos_sys_native_ipc_future_t` now adapts one non-blocking event-loop
  intent into a caller-owned future, copying its correlated reply before
  resolution; protocol callbacks reject error replies, while local cancellation
  discards late replies without cancelling transport work. Net-stack uses this
  path for its `virtio.net` service lookup and initial `NETDRV_IPC_LINK_GET`:
  native coroutines await their replies before installing the driver endpoint
  and binding the lwIP netif. Each bounded interface slot owns the caller
  storage for its link future/coroutine/stack, lwIP netif, and driver RX/TX
  transfer-buffer state. The link request is correlated by a second event loop
  on the dedicated driver-reply endpoint. RX/link notifications and RX/TX
  state-machine replies remain handler/default-dispatch paths. Address, admin,
  and DHCP control operations select an interface index and keep their state in
  that interface slot; the selector-less boot config still targets default.
  Its ELF entry is
  now libsys's generic `async_initialize`, which runs the service's
  `wasmos_async_main` callback in the root coroutine. Native kernel console
  callbacks stage output in higher-half kernel storage so a low driver-owned
  coroutine stack is never interpreted as a kernel physical alias. The normal
  QEMU halt smoke now requires the net-stack lwIP and service-registration
  banners, covering this root-coroutine-to-kernel-callback boundary. It has no
  timers, generic future cancellation, CQ wiring, or multi-worker scheduling.
- WASM `libsys` now includes the corresponding caller-storage stackless C
  coroutine/future/promise core. Its explicit resume-function state machines
  provide `async_start`, yield, await, join, deferred `then`, and `race`/`all`
  without trying to save a WASM C stack. Host unit coverage validates its
  scheduler and future contracts directly; an x86-64-host WARP fixture target
  compiles and runs the same core as wasm32. The shared
  `wasmos_sys_wasm_ipc_future_t` now promotes one non-blocking event-loop
  intent into a caller-owned future: replies are copied before resolution,
  protocol callbacks can reject them, send failures reject immediately, and
  local cancellation stops only reply tracking. Rust, Zig, and Go expose the
  corresponding event-loop/IPC-future records. AssemblyScript support remains
  deferred.
  The existing synchronous filesystem shims are unchanged; Go, Rust, and Zig
  now also expose an additive non-blocking FS-request entry point over that
  bridge. Its caller owns the request and referenced transfer buffer until its
  future settles.
  The Go typed async FS write stages its payload through the operation's own
  fixed-array storage before entering C: TinyGo delivers a null pointer when the
  address of a caller-supplied slice element (`&slice[0]`) is passed through a
  `//go:linkname` C function, so the source must be an array field for the
  pointer to marshal correctly. A single async write is bounded by one transfer
  buffer.
  Rust is now the first exception: its `coroutine` module links the C core and
  exposes method-based `Runtime`, `Future`, `Promise`, `Coroutine`,
  `Continuation`, and `FutureGroup` bindings; host Rust tests execute those
  methods against the same C implementation. Zig and Go now have matching
  wrappers. Zig exposes the `wasmos.coroutine`
  method wrapper and links the same C object into its WASM applications; its
  host wrapper test runs against the C core. Go exposes the same
  caller-storage API and its custom TinyGo target compiles and links
  `coroutine_wasm.c`; Go callback entries are wasm table addresses because Go
  has no portable C function-pointer value.
- The Go, Rust, and Zig hello examples all run as C-owned async applications
  (`RunAsyncApp` / `run_async_app` / `runAsyncApp` over
  `wasmos_sys_wasm_async_run`) that express their filesystem workflow as a typed
  FS promise chain (`open/read/write/close/unlink/stat_async` + `.then` /
  `.catch`).  The Rust and Zig builds link `service_runtime_wasm.c`, and unlike
  TinyGo both pass buffer pointers straight through the C boundary and use their
  own function pointers directly as continuation callbacks (no staging copy, no
  trampoline); their promise operations come from a fixed leak pool because the
  WASM targets are `no_std` / freestanding with no heap.
- Completion ports are documented as a design proposal only: the planned
  kernel-owned bounded CQ, notification-doorbell, and generation-tagged
  operation-token model has no implementation yet. It is intended to provide
  batched asynchronous completions for the future/promise runtime, beginning
  with high-rate networking operations.

- wasm3 is the default interpreter. WARP is the optional JIT/AOT backend and
  follows the ring-3 execution model; internal modules can fall back to JIT if
  an embedded AOT payload cannot load.
- WARP compiler allocations no longer consume the fixed global kernel slab:
  its small-allocation pool grows from physical pages on demand and releases a
  completely unused page. This keeps JIT compilation independent of service
  metadata and transfer-buffer queue depth.
- The general kernel small-object slab (`kalloc_small`) grows on demand as
  well: when a size class's static free list is exhausted it carves a fresh
  direct-mapped physical frame (allocated below the higher-half window) into
  chunks and links them onto the free list, so kernel metadata (IPC select-set
  poll watchers, futexes, container nodes) is bounded by physical memory rather
  than a fixed count. A WASM application that registered a kernel select-set
  late in boot previously exhausted the fixed 64-byte class; its endpoint then
  carried no poll watcher, so a reply delivered to it never woke the waiter and
  the application blocked forever.
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
  that route. Stream (TCP) sockets connect asynchronously: `NET_IPC_CONNECT`
  starts the handshake, and the reply is deferred until the lwIP `connected`
  (or `err`) callback fires. TX-ring bytes stream through `tcp_write`/
  `tcp_output` with `tcp_sndbuf` backpressure (peeked, consumed only once
  accepted, resumed from the `sent` callback); inbound segments copy into the
  RX ring and acknowledge via `tcp_recved`, refusing with `ERR_MEM` when the
  ring is full so lwIP retains and redelivers. `NET_IPC_CLOSE` closes gracefully
  after detaching callbacks. The SLIRP TCP echo test covers connect + stream +
  echo. Server-side TCP is wired too: `NET_IPC_LISTEN` turns a bound stream
  socket into a listening pcb, and `NET_IPC_ACCEPT` posts a client-owned ring
  pair as an accept slot; the lwIP accept callback pairs an inbound connection
  with the earliest posted slot, answers the deferred ACCEPT with the accepted
  socket id, and rejects (RST) when no slot is posted. Each posted ACCEPT is one
  accept slot, so a server pre-posts several to accept several connections. A
  SLIRP hostfwd e2e test drives host->guest connect + accept + echo. `ringbuf.h`
  and the
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
- Networking Phase 2 (ARP/ICMP/UDP) and Phase 3 (TCP client connect/stream/close
  and server listen/accept/echo over rings) are validated end-to-end. Still
  pending: TCP timeout/retransmit hardening (`sys_check_timeouts` is only
  advanced from the idle loop), and the IPv6/multi-address/multi-instance and
  DMA fast-path phases.
- Maintain the boot entry contract, C ABI boundaries, and runtime-wrapper
  parity. Record meaningful future baseline changes here as concise subsystem
  updates; keep detailed design changes in `docs/architecture/`.
