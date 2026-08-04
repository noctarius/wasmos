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
- The socket TX/RX rings are now the zero-copy data plane the design intended
  (docs/architecture/22): a WASM app overlays its own ring xfer-buffers into
  linear memory with the new `xfer_buffer_map`/`xfer_buffer_unmap` hostcalls
  (wired in both runtimes + the WARP AOT stub table) and drives them with
  `ringbuf.h` in place, instead of copy-based `xfer_buffer_read/write` poking.
  The overlay is a mapping only: the xfer-buffer's owner frees the backing, never
  the overlay. On WARP the mapped overlay holds a phys refcount (`pfa_pin_pages`
  at `xfer_buffer_map`, released at `xfer_buffer_unmap`) so that if a process
  exits or **traps** with the overlay still mapped, the linear-memory slot
  decommit's page free is a harmless refcount decrement rather than a double-free
  against the owner's later release/reap (previously this crashed the kernel with
  a `pfa double-free` when e.g. `curl` trapped mid-connect). This mirrors the
  existing region-overlay pin. The shared `wasmos_net_tcp_connect/send/recv/close` helper
  (`wasmos/net.h`) implements a TCP stream socket on top; `examples/c/net_tcp_echo`
  runs on it (validated by `test_net_stack_tcp_echo_e2e`).
- net-stack drives stream sockets through lwIP's `altcp` layer (`LWIP_ALTCP`)
  rather than raw `tcp_*`, so plaintext TCP (`altcp_tcp`) and TLS (`altcp_tls`)
  share one code path.
- Native services have a real growable heap: the `driver_api` exposes
  `vm_map`/`vm_unmap` (anonymous kernel pages returned as higher-half pointers,
  usable directly since native services run supervisor), and
  `src/libsys/native/heap_native.c` layers a slab allocator (size-class slabs +
  per-mapping large allocations) providing standard `malloc/free/calloc/realloc`.
  Each native service links its own copy (per-service state, single-threaded).
  This exists because the only libc malloc is WASM-specific and the kernel slab
  (`kmem`) is small-object-only (<=128 B). `heap_corruption_detected` logs and
  `proc_exit`s. (ABI bumped 11->12.) Reap reclamation of heap pages is a TODO
  (native services are long-lived).
- TLS client (milestone C): net-stack embeds mbedTLS 3.6 (freestanding
  config in `src/services/net_stack/net_stack_mbedtls_config.h`; `mbedtls_calloc/free`
  are bound at compile time via the `MBEDTLS_PLATFORM_*_MACRO` forms to the native
  slab allocator so the TLS heap grows on demand — the full Mozilla CA bundle
  (119 certs, ~180 KB PEM) parses fine. The macro forms are load-bearing: they
  also make lwIP's `altcp_tls` layer skip its `mbedtls_platform_set_calloc_free`
  override, which would otherwise redirect all mbedTLS allocation into lwIP's
  fixed `MEM_SIZE` (64 KiB) heap and fail the bundle parse after ~20 certs with
  `X509_ALLOC_FAILED`. Entropy comes from the `hrng` pool via
  `mbedtls_hardware_poll`) behind lwIP's `altcp_tls`. A stream socket opened with `NET_SOCKET_OPEN_FLAG_TLS`
  is created with `altcp_tls_new`; the socket send/recv/close path is otherwise
  unchanged. `wasmos_net_tls_connect(..., sni)` (libc `wasmos/net.h`) and
  `curl https://` use it. This is a TLS 1.2 ECDHE handshake with **full server
  certificate chain + hostname verification** (`ALTCP_MBEDTLS_AUTHMODE =
  MBEDTLS_SSL_VERIFY_REQUIRED`):
  - **Trust store.** net-stack loads a PEM CA bundle from
    `/boot/system/net/certificates/ca-certs.pem` at startup via an async FS read
    (mirroring the interfaces loader), keeps it in a heap buffer for the process
    life, and builds the shared client config with it
    (`altcp_tls_create_config_client(ca_bytes, ca_len)`). PEM input is passed
    NUL-terminated with `ca_len = bytes + 1` (mbedTLS PEM requirement;
    `MBEDTLS_PEM_PARSE_C`/`MBEDTLS_BASE64_C` are enabled for this). If the file is
    missing/unreadable the config is never built and TLS opens fail — there is no
    silent fall back to no-verify (`[net-stack] tls: no CA trust store`).
  - **Hostname.** The client passes the server hostname (or IP literal) in a new
    `sni[256]`/`sni_len` field of `net_socket_open_descriptor_v1_t`; net-stack calls
    `mbedtls_ssl_set_hostname()` on the TLS pcb before the handshake so the server
    certificate CN/SAN is checked (and SNI is sent). A TLS open with no SNI is
    refused (verification without a name would be a MITM hole).
  - **Real trust bundle.** `scripts/fetch-ca-certs.sh` downloads and SHA-256-pins a
    dated curl.se Mozilla CA bundle into `scripts/system/net/certificates/ca-certs.pem`;
    the ESP assembly copies it to the guest path above (empty placeholder when not
    fetched, so the build never needs the network).
  - Certificate **date validity is not checked** (`MBEDTLS_HAVE_TIME`/`HAVE_TIME_DATE`
    stay off — no RTC is wired to mbedTLS yet); wiring RTC time is a follow-up.
    Large TLS transfers can also still stall on RX-ring backpressure (the app must
    drain fast enough) — RX-ring flow-control hardening is a separate follow-up.
  Validated hermetically by `test_net_stack_https_verify_e2e` (positive: a CA-signed
  server with `SAN=IP:10.0.2.2` verifies and the body is fetched; negative: a rogue
  self-signed server is rejected and no body is printed) and by the verifying
  `test_net_stack_https_e2e`. The lwIP 2.2.1 mbedTLS glue is a 2.x-era subtree bridged
  to 3.6 without editing `libs/` (shim headers + a force-included compat header under
  `src/services/net_stack/`).
- `/system/utils/curl` (`curl <host>[:port][/path] [-o <file>]`) is a minimal
  HTTP/1.0 GET client on that helper: it resolves the host (DNS, or an IPv4
  literal directly), fetches the URL, strips the response headers, and writes the
  body to stdout or a file (libc `open`/`write`). `test_net_stack_curl_e2e`
  exercises both against a local HTTP server reached via the SLIRP gateway.
  Note: guest-to-guest requests (an in-guest HTTP server) need `LWIP_NETIF_LOOPBACK`
  and net-stack loopback polling — a follow-up.
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
- Name resolution is exposed via `NET_IPC_RESOLVE`: the caller borrows the
  hostname read-only and net-stack drives `dns_gethostbyname()`, answering
  immediately for cached names and deferring the reply (from lwIP's DNS callback)
  otherwise, so a slow lookup never blocks the reactor. A static local host list
  maps `localhost` to `127.0.0.1` with no network round-trip. Clients use the
  shared helpers `wasmos_net_resolve()` (WASM/libc, `wasmos/net.h`) and
  `wasmos_sys_net_resolve_native()` (native libsys); the `/system/utils/host`
  tool (`host <name>`) resolves and prints an address.
- `net-stack` registers its PUBLIC endpoint as `net.stack`; client
  socket/ifaddr requests are dispatched there (registering the control endpoint
  had silently dropped them via the async event loop).
- `NET_IPC_IFADDR_ADD/DEL/LIST`, `NET_IPC_IF_SET_STATE`, and `NET_IPC_DHCP_SET`
  are implemented; the `/system/utils/ip` tool (`ip addr show|add|del`,
  `ip dev <name> up|down`, `ip dhcp <name> on|off`, `ip dns show|set|del`)
  inspects and edits interface addressing and the resolver at runtime.
  `ip dns set <ip> [<ip2>]` replaces the resolver list (via `NET_IPC_DNS_SET`,
  overriding DHCP), `ip dns del <ip>` removes one, and `ip dns show`
  (`NET_IPC_DNS_LIST`) reports the current servers. `ip dhcp <name> on` clears any static address and
  (re)starts the lwIP DHCP client; `off` stops it and leaves the current address
  in place. lwIP is IPv4-only with a single address per netif, so static
  addressing and DHCP are mutually exclusive on an interface (no address aliases).

### Build, Configuration, and Validation

- Default configuration: wasm3 runtime, ring-3 isolation, single CPU. WARP is
  selected with `-DWASMOS_WASM_RUNTIME_WARP=ON`; SMP is separately gated by
  `WASMOS_SMP` and requires IOAPIC.
- WARP QEMU CPU model: the run/test QEMU commands pass `-cpu max`
  (`WASMOS_QEMU_CPU_ARGS` in CMake; also in `scripts/qemu_test_framework.py`).
  WARP's single-pass JIT emits modern x86-64 instructions unconditionally (e.g.
  `POPCNT`/`LZCNT`/`TZCNT` for wasm `i32.popcnt`/`clz`/`ctz`) with no CPUID
  guard, so the default `qemu64` model faults them with `#UD`.
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
  bring-up, per-CPU state, per-CPU ready queues with work-stealing, and
  hardening for cross-CPU wake/reap/context races.
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
- Error and status codes are single-sourced in `abi/errors.yaml` and generated for
  C, Rust, Go, Zig and AssemblyScript; `gen_abi_errors.py --check` guards the
  checked-in output against IDL drift. Both axes are negative on error: the
  transport `wasmos_status_t` and the packed `(domain, code)` operation errors,
  where a code is the negative of `(domain << 16) | local_code`. A code is
  returned, compared and decoded as-is — `WASMOS_ERR_MAKE` / `_DOMAIN_OF` /
  `_CODE_OF` are the only places the sign is applied or removed, and
  `wasmos_frame_t`'s unsigned 16-bit fields are the wire encoding of a chain
  frame. Migrated onto it so far: SHMEM, FS and PROC (their `*_ERR_*` enums are
  gone), plus every bare `-1` that left a service in an IPC reply code arg. Bare
  `-1` survives in internal helper returns, where the `quality` lint flags it
  advisorily.
- The per-subsystem negative-int status vocabularies are gone: `XFER_BUFFER_ERR_*`,
  `NET_STATUS_*`, `GFX_STATUS_*`, `FONT_STATUS_*`, `RTC_STATUS_*`,
  `HRNG_STATUS_*` and `VT_SWITCH_ERR_*` are now the `xfer_buffer`, `net`, `gfx`,
  `font`, `rtc`, `hrng` and `vt` domains in `abi/errors.yaml`, and the headers
  that declared them include the generated `wasmos_status.h` instead. The
  duplicated status declarations (`font_ipc.h` ×2, `rtc_ipc.h` ×3, plus local
  copies in `rtc.ts`, `libui.ts` and `tetris.rs`) are collapsed onto the IDL; the
  headers remain duplicated only for opcode/struct content, which belongs to the
  opcodes IDL. `PM_SPAWN_INTERNAL_ERR_*` is deliberately internal and stays, as
  does the transport `IPC_ERR_*` axis.
- AssemblyScript consumes the generated status ABI: the AS app helper and the rtc
  driver build stage `abi/generated/assemblyscript/wasmos_status.ts`, which
  previously existed but was wired into no build. `tetris.rs` still declares its
  own success constant — a standalone single-file rustc crate with no module
  staging.
  The 4-frame cause chain is generated in every language but has no call sites
  yet — wrapping is unused, and an IPC reply can carry at most two frames in
  `arg0..arg3`, so a deeper chain needs a transfer buffer.

## Filesystems and Storage

- `fs-manager` is the VFS endpoint and routes `/init`, `/boot`, and `/user`.
  `fs-init` serves initfs; FAT backends mount block volumes for `/boot` and
  optional `/user`.
- `fs-fat` is a single-threaded, non-blocking reactor: queued operation
  contexts are resumable stackless coroutines, while one active operation uses
  the shared 8 KiB block/DMA buffer. It supports FAT12/16 and LFN lookup across
  multi-cluster directories (FAT32 is detected at mount but its cluster
  read/write is unimplemented), reports `FS_ERR_*`, and binds to its requested
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
- The `vt` service is the I/O multiplexer (`docs/architecture/19`). Phases 0–4
  are shipped: idle-spin fixes, serial RX via IRQ4 into the serial-bound slot,
  push-based CLI input, a single loadable-keymap decoder, and (phase 4) kernel
  klog into vt-1. klog rides a VT-owned SPSC ring (`wasmos/ringbuf.h`) overlaid
  on a `BUFFER_KIND_TRANSFER` xfer-buffer — the socket-ring transport, not raw
  shmem — registered with the kernel via `klog_register_ring`; `serial_write`
  publishes into it additively (legacy `console_ring` + fbpci drain retained for
  early-boot on-screen klog, retired in phase 5). The VT blocks on
  `wasmos_ipc_select_one` and drains the ring on each wake (a timed select set
  stranded serial input under WARP; there is no ring doorbell yet). WARP notes:
  the overlay needs the VT's `INITIAL_MEMORY`/`heap_pages` tuned so the window
  lands in declared linear memory below the 2 MiB low-guard (a cheap page-fault,
  not a multi-MiB commit); a new hostcall needs the numbered `warp_ring3_dispatch`
  case plus the AOT tool's symbol table, and `--initial/max-memory` changes are
  content-cached under `.cache/warp_aot`.
- Phase 5 (in progress): tty switching repaints the target slot with a single
  shared-buffer grid blit (`FBTEXT_IPC_BLIT_ATTACH`/`BLIT_GRID`, cell grid in a
  VT-owned xfer-buffer granted READ to the framebuffer driver) instead of a
  per-cell `CELL_WRITE` IPC loop. The per-cell loop stormed the driver's queue
  and, under SMP, starved it — an ~80 s switch wedge; the blit is one IPC.
  Verified by `tests/test_vt_tty_switch.py`. The compositor now owns the
  framebuffer only while vt-0 is visible: it switches to vt-0 once (first window),
  never auto-grabs again, and draws only while the vt reports vt-0 visible
  (`VT_IPC_VIS_NOTIFY`, sent on every switch; "hidden" is sent before the vt
  repaints the text slot so the compositor stops first). Switching is user-driven
  (`Ctrl+Shift+Fn` / `tty N`). Still to do in phase 5: default-visible vt-1 +
  retiring the fbpci console-ring drain, the serial-bound-slot selector, and lazy
  per-slot CLI spawn.

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
  `NO_CHROME`, `INVISIBLE`, `PASSTHROUGH_ZERO`, `NO_ACTIVATE`, `NO_CONTENT`,
  `NO_TASK_LIST`, and related flags).
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
- `examples/rust/tetris` is a graphical, double-buffered Rust game for the gfx
  compositor: it talks the GFX IPC directly (create window, alloc BGRA32 shared
  buffer, present), renders into an app-owned back buffer, and reads keyboard via
  the compositor event endpoint (WASD + space; the compositor now forwards
  `ascii | (scancode << 8)`, so arrow/function keys are delivered — whether the
  Tetris app itself reads the scancode byte is a separate app concern; arrow
  keys were previously unusable because only ASCII was forwarded). A start menu offers Single
  Player / Be Host / Join Session; single-player is fully functional. The
  two-player path streams a fixed board snapshot over net-stack TCP (client via
  the `net.h` connect helper, server via a hand-rolled listen/accept in
  `net_shim.c`), with line clears sending garbage rows. It is NOT auto-started by
  sysinit; spawn it from the CLI (`spawn /boot/apps/tetris`). Two system gaps it
  originally exposed are now fixed: keyboard focus arbitration (the CLI reports
  itself background and stops consuming keys while the compositor owns tty0), and
  the two-player host accept pairing (the host handshake is a non-blocking
  doorbell path and the QEMU commands pass `-cpu max` so WARP's `POPCNT`-based
  ring pow2 check no longer `#UD`s). A load bug to note: the WARP shmem mapper
  places a mapped window just above currently-committed linear memory, so a large
  app-owned back buffer must be touched/committed before `shmem_map_auto` or the
  shared window overlaps it (Tetris commits `BACK` before mapping).

## Current Gaps and Guardrails

- Shared WARP linear-memory updates need a real cross-CPU TLB shootdown; the
  current fault-path retry is an interim SMP safeguard.
- WARP still has a few hostcall refinements pending (SMP sync, shmem auto-map
  growth, PAT, irq.configure split), no working multithreaded WASM, and an
  internal shim used to access a vendored runtime pointer. Do not modify
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
