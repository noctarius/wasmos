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
- `virtio-net` takes its device identity solely from the startup args written
  by the device manager (`pci= vendor= device= io= irq=`). The driver carries
  no PCI config-space scan; a spawn without a valid identity fails immediately
  with `WASMOS_ERR_DRIVER_NO_DEVICE_IDENTITY` rather than binding to a device
  it found itself.
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

### Interrupts

- Message-signalled interrupts are implemented end to end and are the default
  for both virtio devices. Vectors 48–63 (`MSI_VECTOR_BASE`, 16 slots) have ISR
  stubs and IDT gates, installed only where a LAPIC can receive the message
  write; `msi_alloc` refuses in pure-8259 mode.
- Authority is split three ways: the kernel owns the vector namespace and binds
  a vector only to an endpoint the caller owns; pci-bus owns configuration space
  and programs the device; the driver maps its own queues onto the entries. No
  "route on behalf of" path exists, so the endpoint-ownership check is intact.
- `pci-bus` is now a resident service (name `"pci"`, opcodes `0xd00`–`0xdff`)
  rather than a one-shot scanner, because it is the only holder of the
  0xCF8/0xCFC window. Its request loop blocks on `wasmos_ipc_select_one`; moving
  it to the coroutine runtime is a follow-up needed once it must originate
  requests while serving (hot-plug).
- `mmio_write32` is the one primitive that lets a bus driver write a device
  register (`wasmos_phys_map` copies rather than maps). It requires `mmio.map`
  and refuses any address overlapping usable RAM, so it cannot reach system
  memory.
- `virtio-net` uses three vectors (RX, TX, config) and its idle wait is a plain
  blocking wait; the timed RX drain survives only on the INTx fallback path.
  `virtio-rng` uses one. Both set `INTX_DISABLE`, so QEMU's shared IRQ 11 now
  has no user — the sharer/ack/deadline machinery in the kernel remains for ISA
  and any future INTx device, but nothing in the default boot exercises it.
- `ata` completion is interrupt-driven (IRQ 14 + `nIEN` cleared in Device
  Control, which nothing had ever written, so device interrupts had been masked
  at the drive all along). The sector waits no longer spin on the status
  register. Interrupt use is an optimisation, never a dependency: a
  routed-but-silent line is abandoned after a bounded number of empty sleeps and
  the driver reverts to polling.
- `ata` reads use bus-master IDE DMA when a PRD table can be backed
  (`wasmos_block_buffer_phys`), and PIO otherwise; the choice is reported once at
  startup rather than per request, because a per-request "dma fallback" line read
  like an intermittent failure when the fallback is neither intermittent nor a
  failure. The read direction is zero-copy: `BLOCK_IPC_READ_ZC_REQ` carries the
  client's `borrow_id`, the driver maps it with `dma_map_borrow`
  (`WASMOS_DMA_DIR_FROM_DEVICE`), lets the controller write those pages, then
  syncs and unmaps — on the failure path too, since the controller may have
  written part of the range before erroring out. A borrow that cannot be mapped
  degrades to a staged copy through the driver's own block buffer, and the two
  outcomes log distinguishably ("direct DMA into client buffer" vs "staged copy
  into client buffer") because only one of them is actually copy-free. Writes
  have no zero-copy path: `BLOCK_IPC_WRITE_REQ` names the driver's own buffer, so
  there is no client borrow to map (`TODO(zero-copy writes)` in
  `src/drivers/ata/ata.c`).
- Validated on `wasmos_defconfig` (WARP+SMP, 9/11 runs), `wasm3_smp_defconfig`,
  and `wasm3_single_defconfig` (3/3). The two failures were a
  `scheduler: no runnable thread` panic during early kernel self-tests, before
  pci-bus or either driver starts — see the scheduler race, not this path.

### Host Calls and Capabilities

- The three `dma_*` host calls decide nothing per runtime. `hostcall_dma.c`
  holds the whole policy decision — argument signs, `POLICY_ACTION_DMA_BUFFER`,
  the granted direction, the per-mapping byte budget, and the window the
  resulting physical address must fall inside — and both shims
  (`wasm3/link_dma.c`, `warp/link_dma.cpp`) only marshal into it. They are
  separate translation units from the monolithic `link.c`/`link.cpp` for the
  same reason `link_ipc.*` is: neither monolith compiles for a host test.
- That split is what closed a real divergence. The checks used to be written
  once per runtime and WARP's copy had none of them, so a WARP guest holding any
  transfer-buffer borrow could DMA in an ungranted direction, over an unbounded
  length, to a physical address outside every window its manifest declared;
  `dma_sync_borrow` and `dma_unmap_borrow` skipped the `dma.buffer` gate
  outright. `test_hostcall_dma` runs 28 scenarios through both shims and asserts
  each value AND the two against each other, so a future check added to one side
  only fails the parity assertion.
- Guest-visible capability behaviour must not depend on the engine. Where a
  difference is unavoidable, the scenario table carries an explicit `divergent`
  flag that asserts the two still differ, so converging them forces the row to
  be reclassified rather than left describing a fixed problem. No DMA row is
  currently divergent.

### Process Lifecycle

- `process_set_ready` and `process_set_running` refuse a transition whose owner
  is already `exiting` or ZOMBIE; they do not panic on it. Both return a status
  (1 = transitioned, 0 = raced), every caller gates its `sched_enqueue_thread`
  on that status, and each refusal is counted
  (`SCHED_DEBUG_SET_READY_EXITING`, `SCHED_DEBUG_SET_RUNNING_EXITING`) with a
  power-of-two rate-limited report. Refusing is the contract, not a fallback:
  no caller holds anything that excludes a concurrent kill or exit, so a
  sibling-requeue racing its owner's teardown is reachable from every call site.
- Two of those four enqueue sites — the `PROCESS_RUN_EXITED` and
  `PROCESS_RUN_THREAD_EXITED` sibling-requeues in `process_schedule_once_impl` —
  used to enqueue unconditionally. The panic was hiding that: the process died
  before reaching the enqueue, so "enqueue a thread under a dying process" was
  unreachable in practice and unguarded in code.
- `process.c` is host-testable behind `WASMOS_PROCESS_TEST_SEAMS`, which replaces
  its six inline-asm sites, the `KERNEL_HIGHER_HALF_BASE` alias helper and the
  saved-context rip/rsp validator — the arch facts a lifecycle question does not
  depend on — and exposes the two lifecycle transitions as `process_test_set_ready`
  / `process_test_set_running`. `tests/unit/test_process_lifecycle.c` has two
  layers over that. Contract cases drive each transition directly and own the
  per-branch coverage; they state the interleaving as a starting state because
  every production caller filters its target's state first, so the window is
  unreachable from outside `process.c`. A pthread soak (one scheduler loop per CPU,
  a killer on the last) at 2, 4 and 8 CPUs then proves the real interleaving is
  survived, asserting the refusal counters' SUM is non-zero so a run that never
  entered the window fails instead of passing vacuously. That assertion runs from
  width 4 up and is reported rather than asserted at width 2, which spawns no
  dispatcher thread of its own (the soak thread is the only dispatcher and also
  owns every spawn, kill and reap) and produced 0-9 refusals against 76-110 at the
  wider arms. With the guards restored the suite aborts at every width.
- A dispatch holds raw pointers to a thread SLOT and a process SLOT across
  `process_schedule_once_impl` — through the switch AND through the result
  handling that follows — and reads `time_slice_ticks`, `kstack_top` and
  `worker_entry` only after dropping the queue lock. `thread_t::dispatch_ref` is
  what makes those slots un-recyclable for that whole window: a tri-state
  single-owner claim (`THREAD_SLOT_FREE`/`DISPATCH`/`FROZEN`) that the dispatcher,
  `thread_reset_slot` and `sched_sweep_owed_enqueues` all contest by CAS from
  FREE, so exactly one owns the slot and the losers back off.
- Every path that takes that claim hands it back, including the ones that refuse
  to act. `thread_reset_slot` CASes FREE -> FROZEN and then re-reads the thread's
  state, refusing to tear down a RUNNING thread and refusing again if the state CAS
  loses; both restore FREE before returning. FROZEN is unrecoverable if leaked --
  every contender CASes from FREE, so a leaked FROZEN makes both the retry the
  refusal promises and every later dispatch of that thread fail the claim rather
  than the check, costing a slot out of a fixed-size table and stranding the thread
  it declined to free. Reachable from `thread_reap_owner_pass`, which resets every
  slot of the owner whatever its state.
- The dispatch's own claim failure is the mirror case and is COUNTED, not silent:
  `SCHED_DEBUG_DISPATCH_DROPPED_SLOT_LOST` when the CAS loses and
  `SCHED_DEBUG_DISPATCH_DROPPED_STEAL_REAPED` when a stolen thread's owner is gone.
  Both exits run after a picker has already unlinked the thread and released
  `on_rq`, so they drop a thread that is off every run queue -- correct for a slot
  being torn down, a strand for a live owner's thread, and indistinguishable after
  the fact without the count. The `slot claim lost` report carries the OBSERVED
  claim value, which separates a racing dispatch (DISPATCH) from a reaper (FROZEN).
- A CAS, not a test-then-act, because the two sides share no lock: the dispatcher
  claims after `cpu_sched_pick_next` has dropped the queue lock, and the reaper
  holds only the thread table lock. `process_reap_claim` additionally refuses while
  any thread of the process is claimed, via one locked pass
  (`thread_owner_has_active_dispatch`) — the ordinal accessors drop the table lock
  between calls, so an index walk can have entries shift under it and miss the very
  thread it is checking for.
- A CLAIM, not a state test, and that is the load-bearing part. The thread's state
  legitimately changes several times inside the window (READY → RUNNING → ZOMBIE
  for a thread that exits), so every state-based guard covers only a piece of it
  and the recycle lands in the rest. Four such guards were tried first and each
  left a residue; they remain as cheap early rejects (`NEW->RUNNING` is not a legal
  process transition, and the dispatcher re-validates `(tid, owner_pid)` after its
  claim) but the claim is what closes the window.
- Promotions to READY go through `thread_wake_if_blocked`, and its result is
  deliberately IGNORED. Only the DEMOTION is avoided, and what it costs is
  specific: `THREAD_STATE_RUNNING` *is* the exclusive dispatch claim
  (`cpu_sched_claim_for_dispatch` is a READY→RUNNING CAS), so writing READY over
  it re-arms the claim and a second CPU can win it on a thread that is already
  executing. Between that claim and the publication of
  `cpu_local()->current_thread` the RUNNING state is the only record that the
  thread is spoken for, so `sched_enqueue_thread`'s holder scan cannot see it and
  only its `state != READY` skip keeps an executing thread out of a ready queue.
  Pinned by "the dispatch claim survived the promotion" in
  `tests/unit/test_process_lifecycle.c`, which fails four ways against an
  unconditional promotion. `thread_set_state` is not used for the promotion
  because a BLOCKED target must have `block_reason` cleared with the state, which
  a bare state CAS does not do, and a READY thread still carrying the reason it
  blocked for is put straight back to sleep by the wait paths.
- Reporting the promotion's result to the caller is WRONG and was reverted twice
  for the same reason, so it is worth stating as a rule: `process_set_ready`
  answers "may this owner's thread be made runnable" (an owner question, about
  `exiting`/ZOMBIE), never "did this call change the thread's state". Gating
  `sched_wake_claim_enqueue` on the latter drops wakes aimed at a thread that is
  executing right now — that handshake exists precisely to hand such an enqueue to
  the target's own completion path. Both regressions wedged the boot with the CLI
  never seeing typed input, and both passed every local gate.
- A wake that defers its enqueue leaves a CLAIM, never only a READY mark.
  `sched_wake_thread`'s arm for "the completion path owns the enqueue" publishes
  `sched_owe_enqueue` alongside the mark, because that ownership holds only until
  the completion path makes its decision: it clears `blocking_transition`, takes
  the token, reads the state, sees BLOCKED and correctly declines to enqueue a
  blocked thread. A mark landing after that read is seen by nobody, and the thread
  is then READY on no run queue with no token and no debt -- unrecoverable, since
  `sched_sweep_owed_enqueues` is gated on the global debt counter that a thread
  with no debt never enters. The enqueue-current path in `cpu_sched_enqueue` has
  always published a claim for the same reason.
- A claim consumer validates BEFORE taking the claim, never after.
  `sched_take_owed_enqueue` is documented as consuming the claim "returning 1 to
  the single caller that owns the enqueue", so a caller that takes ownership and
  then declines has absorbed a wake: the debt is gone, `g_enqueue_owed_count` has
  been decremented, and `sched_sweep_owed_enqueues` -- gated on that counter -- can
  no longer find the thread. `sched_settle_deferred_enqueue` therefore reads the
  state first; it runs the instant a dispatch ends, where transient states are
  most likely. The sweep still takes before validating, and it runs on EVERY
  iteration of the scheduler loop rather than only on an idle CPU, so it retires a
  merely-BLOCKED thread's debt on the next iteration of the same loop -- keeping
  the claim in the settle path is correct but is not by itself a fix for a lost
  hand-off.
- The stall dump carries the two fields that diagnosed that: `owed=` on every
  thread line, and `ready_by=` (resolved to a symbol) for a stranded thread, which
  names whoever last promoted it. `SCHED_DEBUG_DISPATCH_LEFT_STRANDED` reports a
  dispatch ending with its thread READY, unqueued, owed nothing and its owner
  live, on EVERY exit rather than only the aborting ones. It samples at
  `dispatch_done` and so over-reports a promote-then-enqueue crossing CPUs; the
  authoritative signal is a `[diag]!` strand persisting across all dump samples
  with `disp` frozen.
- `ready_by=` names the promoter but structurally cannot say why the thread is on
  no run queue, so the dump also carries per-thread run-queue forensics for a
  strand: `enq=` (the outcome of the last enqueue attempt), `links=` (times the
  thread was actually linked), `unlink=` (who last released `on_rq`) and `enq_by=`
  (the call site of that attempt, carried in through `cpu_sched_enqueue_from`).
  They separate the only two histories that reach a stranded thread: it was linked
  and a picker's caller dropped it (`links>0`, `unlink=pick_next`/`steal`), or it
  was never linked because a guard declined (`enq=skip:*`, naming which guard;
  `links=0`, which the first history cannot reach). Diagnostic only -- relaxed
  atomics, read only by the dump, scrubbed with the slot.
- Every scheduler tripwire's running total is printed on one line
  (`[diag] sched counters:`), including the zeros. The per-event reports rate-limit
  to powers of two, so an absent log line is not evidence the event did not fire;
  the counters are the only honest negative and several wrong conclusions have been
  drawn from the log's silence.
- `proc->exiting`, `proc->thread_count`, `proc->live_thread_count` and the
  scheduler's progress diagnostics are read and written cross-CPU, so each has one
  atomic protocol rather than a mix. `live_thread_count` reaching zero gates
  `process_mark_exited`, so a lost update marks a process exited early or never;
  the `if (c > 0) c--` those sites used was both non-atomic and a check-then-act,
  and is now a saturating CAS loop.
- A refused free is reported, not discarded. `thread_reap` returns whether the slot
  was released and `thread_reap_owner` returns how many it could not, retrying the
  short window where a CPU claims a thread of an already-ZOMBIE owner and then
  loses at `process_set_running`. The detached-thread reap in the result handler is
  deferred past the claim release, because it targets the very thread that dispatch
  holds. A refused process reap records `reap_requested`, and the dispatch that
  caused it retries after releasing — armed as soon as `proc` is known, so early
  exits do not skip it.
- `test_process_lifecycle` therefore serialises nothing: spawn, kill and reap all
  run concurrently with every dispatcher. 0/15 runs abort per width at 8 and 16
  CPUs and 0/20 at 2, 4 and 8, with `spawn_retries=0`, including five concurrent
  instances on a 10-core host. Disabling both lifetime guards returns 2/12 aborts
  at 16 CPUs — they are load-bearing, but now as defence in depth over exposure the
  CAS claim and the promotion fixes independently closed.

### Build, Configuration, and Validation

- Default configuration: **WARP** runtime, single CPU. WARP is the default
  because a WARP guest runs at CPL=3 and is preempted like any other thread,
  while the wasm3 interpreter and its guest both run at CPL=0 where a guest loop
  with no host call in it holds its CPU until it returns (architecture/11,
  *Which Workloads Reach Ring 3*) -- one heavy app stalls the desktop. wasm3
  remains fully supported and is the reference implementation the two runtimes
  are read against. Pin a runtime with
  `-DWASMOS_DOTCONFIG=configs/{wasm3,warp}_{single,smp}_defconfig`; a bare
  `cmake -S . -B build` seeds from `configs/wasmos_defconfig`, which selects no
  runtime and so falls to the code default. `-DWASMOS_WASM_RUNTIME_WARP=ON`
  alone is unreliable (kconfig imports afterwards and FORCEs the cache) — see
  `skills/wasmos-build-and-run`. SMP is separately gated by `WASMOS_SMP` and
  requires IOAPIC.
- **There is no "ring-3 isolation" configuration.** The only ring-3 Kconfig
  options are `WASMOS_RING3_SMOKE` and `WASMOS_RING3_THREAD_LIFECYCLE_SMOKE`,
  both test probes and both `=n` in the shipped defconfigs. Ring 3 is entered
  per workload, not per build: WARP guests run at CPL=3, while the wasm3
  interpreter and the guest it interprets both run at CPL=0. See
  `architecture/11` *Which Workloads Reach Ring 3* for the entry paths and what
  follows from them (notably that a wasm3 guest is never timer-preempted).
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
  hardening for cross-CPU wake/reap/context races. Dispatch takes an exclusive
  READY->RUNNING claim on the thread it is about to resume
  (`cpu_sched_claim_for_dispatch`), which is what keeps two CPUs off one
  `process_context_t` in the window between the pick and `current_thread`.
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
- Startup data is supplied through spawn-info buffers, not legacy entry args,
  in all five guest languages: C, Zig, AssemblyScript, Rust and Go each read the
  `wasmos_spawn_info_t` header (PM endpoint, tty, module count/index) and the
  argv blob from the buffer. Guest entry points take no arguments at all: the
  four entry-arg registers were removed from every `wasmos_main` and
  `initialize`, and native ABI 14 leaves `driver_api` as the only parameter of a
  native entry.
  Path spawning first recognizes `.wap`; executable-format brokers can return
  a validated `.wap` launch plan for other formats.
- Service discovery supports named services and class instances. Multi-instance
  providers must use unique class instances and unique concrete PM names.
- Hardware IRQ lines support multiple handlers. A line keeps up to
  `IRQ_SHARERS_MAX` sharers, dispatches to all of them, and reopens only when
  every sharer has acked — required because PCI INTx is wire-OR'd (QEMU puts
  virtio-net and virtio-rng on IRQ 11 together). Registration adds a sharer
  instead of replacing one, and process teardown releases a dying driver's
  routes. Two escapes bound the failure modes: acks past a deadline are
  force-completed so one wedged driver cannot disable a shared device, and a
  per-tick dispatch budget throttles a line whose assertion no sharer clears,
  logging `[irq] dispatch budget exhausted ... line=`.
- virtio-net and virtio-rng are both registered sharers of IRQ 11 and each
  services its own device from the IRQ event (reading the virtio ISR to de-assert,
  then acking to unmask). virtio-rng previously routed no interrupt and
  acknowledged the device from a `rng_fill` poll loop — and not at all on its
  timeout path — so the shared line stayed asserted between poll ticks, re-fired on
  every unmask, and livelocked the single-CPU wasm3 config. Its completion wait is
  now the routed IRQ event on a dedicated endpoint (so it cannot swallow pending
  HRNG requests), with a timed wait left only as a safety net for a lost
  interrupt. It also acks on every main-loop pass, not just while a fill is in
  flight: on a shared line, withholding an ack keeps the line masked for the
  co-sharer too. `build-wasm3-single` went from ~2/5 failures to 8/8 with the
  throttle no longer firing at all — the storm is gone at the source rather than
  contained.
- Select-set readiness is level-triggered. `ipc_select_wait` scans the watched
  endpoints' queues and notification counters before consulting the `ready_ep`
  latch, so the latch is only a wake hint and the queues are the authority. Two
  cases that previously stranded a message and parked its owner for good are now
  reported: a send that lands before `ipc_select_add` registers the watcher (the
  window a service opens between announcing itself ready and building its
  reactor's set), and two signals collapsing into the single latch slot. One
  endpoint is reported per wait, rotating from a per-set cursor, so a
  permanently readable endpoint cannot starve the rest of the set. A reactor
  that waits, consumes the endpoint it is handed, and waits again is therefore
  correct without re-polling its endpoints by hand.
- IRQ routing errors are packed `WASMOS_ERR_IRQ_*` codes (`BAD_LINE`,
  `NOT_AUTHORIZED`, `BAD_ENDPOINT`, `LINE_FULL`, `NOT_A_SHARER`), so a driver can
  tell a capability denial from a full line.
- The CLI's VT traffic runs through a single owned receive pump
  (`wasmos_sys_event_loop`): replies match pending requests by request id, pushes
  match registered handlers by type (`VT_IPC_INPUT_NOTIFY`), and anything
  unclaimed reaches a default handler that logs instead of dropping. It replaced
  per-call drain loops that consumed a message and discarded it when it was not
  the reply they wanted, which lost typed characters outright: a read that timed
  out left its request registered, and the next read discarded that reply on an
  id mismatch — `halt` at the prompt arriving as `alt`. Measured on wasm3:
  1-in-5 halt-test failures before (identical at the pre-change baseline),
  20/20 passes after. The CLI's PM/FS paths still use blocking
  `ipc_select_one`, so it stays blind to input while a command runs.
- The ABI is single-sourced in four IDL files under `abi/` — `errors.yaml`,
  `hostcalls.yaml`, `opcodes.yaml` and `constants.yaml` — and every consumer of
  them is generated, not hand-written: the `HC_*` id enum, the WARP
  `WASMOS_SYMBOLS` link table,
  the wasm3 link table, the WARP AOT symbol table, the ring-3 dispatch table,
  the per-subsystem opcode enums, and guest import bindings for all five
  languages. Each is compiled into the live kernel, both runtimes and the AOT
  tool rather than kept as a parallel artifact. `quality` runs every generator
  with `--check` (output matches the IDL) and `--verify-source` (the IDL matches
  what the tree declares), so neither side can drift. Opcodes are
  endpoint-scoped, so values repeat across subsystems by design and the
  diagnostic lookup takes a subsystem id.
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
- The host-call surface itself is now fully migrated: both runtimes' link layers
  (`src/kernel/wasm3/link.c`, `src/kernel/warp/link.cpp`) return zero bare `-1`.
  Five domains carry it -- `kernel` (the shared boundary and authorization facts:
  NO_CALLER, BAD_POINTER, COPY_FAILED, NOT_AUTHORIZED, TOO_LARGE, UNALIGNED,
  NO_WINDOW, MAP_FAILED), `block`, `thread`, `env` and `framebuffer` -- while
  initfs reuses `fs` (plus a new `fs.NO_IMAGE`), boot needs nothing beyond
  `WASMOS_NOENT`, and generic argument shape stays on the transport axis. A
  subsystem domain holds only what is genuinely its own; anything a second
  subsystem would also need lives in `kernel`.
- Two host-call returns changed meaning during that migration, because `-1` had
  been used for something that is not a failure: `input_read` reports
  `WASMOS_AGAIN` when no key is queued, and `console_write` treats a zero-length
  write as a no-op success.
- The value-return rule (`src/kernel/hostcall_value.c`): a host call carries its
  result and its errors on one signed `i32`, so a success value with bit 31 set
  is read as an error. `hostcall_value_check` refuses a value that should have
  been small; `hostcall_value_counter` keeps a monotonic counter positive and
  wrapping at 2^31 so deltas survive. Applied to `block_buffer_phys`,
  `dma_map_borrow` and `sched_ticks` (which went negative at ~99.4 days of
  uptime at 250 Hz). Where no encoding can help, the call takes an out-parameter
  instead, as `io_region_in32` always had. The whole `io_in8`/`io_in16`/`io_in32`
  family now has that shape: `in32` because a 32-bit port read uses the full
  range, `in8` and `in16` because although a byte or a word cannot collide with a
  negative code, no caller read the sign -- each masked it off, so
  `io.NOT_AUTHORIZED` arrived as the `0xFF`/`0xFFFF` an absent device reads back
  and a denied capability was indistinguishable from missing hardware. The width
  of a value is therefore not the whole test; whether the caller can act on the
  distinction is. No host call carries a guest-chosen full-range value on the
  shared i32 any more: `thread_join` was the last, and the WASM threading family
  it belonged to has since been removed outright (below).
- The WASM threading host calls `thread_create`, `thread_exit`, `thread_join` and
  `thread_detach` are **removed**. They were wired in both runtimes but had no
  caller in any of the five guest languages, and the runtime designs are why:
  a WASM instance serializes under `runtime_lock`, so guests get concurrency, not
  parallelism, and the concurrency story is coroutines + futures
  (docs/architecture/32 §52). `thread_yield` and `thread_gettid` STAY — the guest
  reentrant mutex spins on them. The kernel-side VM-thread machinery they were
  the only users of (`wasm_driver_spawn_vm_thread` and the per-runtime thread-slot
  tables and entry trampolines) is deleted with them.

  Their ids were deleted rather than reserved, and the whole host-call id space
  renumbered densely. That shifts every later ring-3 syscall number and AOT
  rebind position, which is safe here only because there are no out-of-tree
  guests: everything is rebuilt from source and `.cache/warp_aot` is cleared in
  the same change. The IDL header now states that rule instead of the previous
  append-only one.
- The native driver API (`wasmos_native_driver.h`, ABI 13) had the same defect on
  its own surface and was converted with it: `io_in8`/`io_in16` report the value
  through `out`, and `io_out8`/`io_out16` report an outcome rather than dropping
  a refused write silently.
- Three kernel components were extracted during the migration because the two
  runtimes had each written the logic out by hand and the copies had drifted:
  `block_buffer.c` (bounds arithmetic for the block bounce buffer),
  `hostcall_buffer.c` (filling a caller-supplied name buffer) and `kenv.c` (the
  environment store, previously duplicated per runtime). Each replaced a real
  defect, not just duplication -- see the git history for the three.
- The per-subsystem negative-int status vocabularies are gone: `XFER_BUFFER_ERR_*`,
  `NET_STATUS_*`, `GFX_STATUS_*`, `FONT_STATUS_*`, `RTC_STATUS_*`,
  `HRNG_STATUS_*` and `VT_SWITCH_ERR_*` are now the `xfer_buffer`, `net`, `gfx`,
  `font`, `rtc`, `hrng` and `vt` domains in `abi/errors.yaml`, and the headers
  that declared them include the generated `wasmos_status.h` instead. The
  duplicated status declarations (`font_ipc.h` ×2, `rtc_ipc.h` ×3, plus local
  copies in `rtc.ts`, `libui.ts` and `tetris.rs`) are collapsed onto the IDL; the
  headers remain duplicated only for opcode/struct content, which belongs to the
  opcodes IDL. Also migrated: `WASMOS_DMA_STATUS_*` -> the `dma` domain (a
  host-call edge both runtimes return to guests), and libsys' random-helper
  statuses onto `hrng`. What deliberately stays: `PM_SPAWN_INTERNAL_ERR_*`
  (internal by name), `WAMOS_SCRIPT_ERR_*` / `SCRIPT_BROKER_ERR_*` (service
  startup/exit statuses, not reply codes), and the transport `IPC_ERR_*` axis,
  which duplicates `wasmos_status_t` and is tracked separately.
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
  the shared 8 KiB block/DMA buffer. It supports FAT12/16/32 and LFN, reports
  `FS_ERR_*`, and binds to its requested block-device unit.
- Long file names are UTF-8 at the API and UTF-16 on disk, in both directions.
  Reading gathers UTF-16 units positionally (LFN entries arrive highest-ordinal
  first, each carrying a fixed slice, which variable-length UTF-8 cannot do) and
  converts once at the end; creating decodes the name to UTF-16 and sizes the
  entry chain by UNIT count, not byte count. Malformed UTF-8, overlong
  encodings and unpaired surrogates are refused rather than stored. A name whose
  UTF-8 form exceeds the accumulator falls back to the entry's 8.3 short name —
  a name that can be opened — rather than being truncated.
- Every READ-side directory scan walks the whole cluster chain: lookup,
  short-name collision, the emptiness check `rmdir` depends on, the readdir
  stream, and `chdir` — which reached that by delegating to `fat_find_in_dir`
  instead of carrying a second scan of its own, deleting more code than it
  added. Each is bounded by the volume's cluster count, so a corrupt cyclic
  chain fails `WASMOS_ERR_FS_CORRUPT` rather than hanging the reactor — an entry
  budget is deliberately not what terminates the walk.
- FAT32 is served on the read path, not merely detected: 28-bit FAT entries
  (the reserved top nibble is preserved on write, never treated as part of a
  cluster number), the start cluster split across dirent bytes 26..27 and
  20..21, and a ROOT that is an ordinary cluster chain rather than a fixed
  region. `fat_root_origin` is the single place that knows which of those two a
  volume has, so resolve, readdir and chdir no longer open-code it. Cluster
  numbers are `uint32_t` throughout; a 16-bit field would have silently
  addressed the wrong sector rather than erroring. Directory-level mutation is
  covered too (create, mkdir, rmdir, chain append); file DATA read/write on
  FAT32 is not, because it runs through the client transfer-buffer path.
- FAT interop is checked against real tools, not only against this repository's
  own reading of the format. `cmake --build build --target run-fat-image-tests`
  formats FAT16, FAT16+LFN and FAT32 volumes with the platform's own
  `mkfs.vfat`/`newfs_msdos`, has the driver mount, read and MODIFY each, then
  runs `fsck.vfat`/`fsck_msdos` over the result and mounts it on the host to
  confirm the entries the driver created are visible to the operating system.
  It skips when no formatter is installed, so it is a separate target rather
  than part of `run-kernel-unit-tests`.

  The mutations are: a new empty file; a new file with content; a file spanning
  several clusters; an OVERWRITE of a file the formatter wrote, shrinking it; a
  new directory with a file inside it; and an unlink of one of the formatter's
  files. The host then compares CONTENT byte for byte — including the
  multi-cluster file against a position-dependent pattern, so a shifted or
  truncated copy fails — and confirms the unlinked file is gone while untouched
  files still read correctly.

  Judge that target by its output, not its exit status: `fsck -n` reports a
  fault and still exits 0, so the script greps the report. Two real defects came
  out of its first run — a `..` entry written with a non-zero start cluster
  under a FAT12/16 root, and FSInfo free-space drift (still open, `TASKS.md`).

  Writes go through `fat_op_write` itself. That entry point used to be
  unreachable from a 64-bit host because it passes the client buffer's address
  to `wasmos_xfer_buffer_read`, and while that parameter was declared `int32_t`
  a host pointer did not survive it. The `dst`/`src` params now carry a
  `c_type` of `void*`/`const void*` in `abi/hostcalls.yaml`, which changes no
  wire format — a pointer is 32 bits on wasm32, so the import is still an i32 —
  and makes the same code host-drivable. Prefer that over `addr_cast(int32_t,
  …)` for any new buffer-address parameter.

  Opens go through `fat_op_open`, so the `O_CREAT`/`O_TRUNC`/`O_APPEND`
  handling runs — a slot built by hand skips it entirely. The truncating case
  is checked as a real shrink: a 64-byte file reopened with `O_TRUNC` and
  rewritten with 19 bytes must be 19 bytes on the host, not 19 new bytes in
  front of 45 stale ones. Note `O_TRUNC` resets the size without freeing the
  cluster chain, so a truncated file keeps its capacity; `fsck` accepts that.

  Rename is exercised too — a rename within a directory and a move into another
  — with the host confirming the content arrives under the new name and the old
  path is gone.

  Reads go through `fat_op_read` itself, including its zero-copy whole-sector
  passthrough and the bounce-through-the-staging-sector path a partial sector
  takes: the harness models the borrow passthrough by treating the image as the
  block server and its transfer buffer as the client's, which is the same shape
  from the driver's side. File data therefore round-trips on FAT32 as well as
  FAT16.
- The write side addresses directory slots CHAIN-RELATIVELY. An entry index
  counts across the whole directory and is resolved to a physical sector by
  `fat_dir_entry_locate`, which walks the chain; `dir_lba + index /
  entries_per_sector` was valid only inside one contiguous run. That is what
  lets a create or rename use a free slot in any cluster, lets a run of free
  slots straddle a cluster boundary, and lets the LFN back-walk in
  `fat_delete_dir_entry_chain` cross back into the previous cluster.
  `fat_dir_entry_info_t` therefore carries both forms: the physical triple the
  open-file table records, and the chain-relative index the writer and deleter
  address.
- Directories GROW. When every cluster is full, `fat_find_free_dir_slots`
  allocates a cluster, marks it end-of-chain, zeroes it and only then links it
  on — so an interruption leaves an unreferenced cluster rather than a directory
  whose last cluster holds garbage.
- `ls` orders its listing; the filesystem does not. The backend streams entries
  in on-disk slot order, which is not even insertion order — a freed slot is
  reused, so a file created after a deletion appears in the hole. FAT specifies
  no ordering and POSIX `readdir()` guarantees none, and the backend emits
  entries as it walks the cluster chain, so sorting there would mean buffering a
  whole directory before the first byte. The CLI collects and sorts instead
  (`src/services/cli/cli_ls_order.c`, split out for unit testing like
  `fs_manager_path.c`): case-insensitive, with digit runs compared by value so
  `f9` precedes `f10`, and a trailing `/` treated as the directory marker rather
  than part of the name.

  The collector is bounded (192 entries / 4 KiB of names). A directory past that
  is printed IN FULL in on-disk order with a visible note, never truncated and
  never half-sorted — an omitted file is worse than an awkward order. `cat`
  still streams straight through; a file's bytes are never buffered or
  reordered.
- Rename/move exists end to end: libc `rename()` -> `FS_IPC_RENAME_REQ` (both
  paths in one transfer buffer, source at offset 0 and destination after its
  NUL) -> fs-manager routing -> `fat_rename_path`. Only the directory entry
  moves; the cluster chain is never read or copied, so a rename costs the same
  regardless of file size. The new entry is written BEFORE the old is
  tombstoned, so an interruption leaves the chain reachable under two names
  rather than none. A directory that changes parents has its `..` rewritten
  under the same 0-when-parent-is-root rule `mkdir` uses.

  An existing destination FILE is replaced, as POSIX requires: the destination's
  entry is pointed at the source's data, the source name is dropped, and only
  then is the replaced chain released — so an interruption leaves an orphaned
  chain rather than a lost file. Replacing a DIRECTORY is refused, because
  freeing its contents would be a recursive delete wearing a rename's clothes.
  The driver-level entry point still defaults to refusing; `replace` is opt-in
  there and set by the IPC path, so a mistyped internal call cannot destroy
  data. An open source or destination is refused (`WASMOS_ERR_FS_BUSY`) because
  a descriptor records where its directory entry lives, and cross-mount renames
  are refused by fs-manager.
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
  `GFX_EVENT_KEY` carries `ascii | (scancode << 8)`, so key handlers decode it
  with `ui_key_char()` / `ui_key_scancode()` rather than using the packed code
  as a character; `tests/unit/test_libui_key_decode.c` pins that contract.
- `gfx_smoke` exercises multiple windows, close-event teardown, and libui;
  `menu_bar` exercises popup/window interactions. Both are spawned after the
  compositor is ready when present in `sysinit`.

## Applications and Language Support

- `.wap` packages cover WASM and native apps, services, and drivers. C, C++,
  Zig, Go, Rust, and AssemblyScript examples are supported through shared libc
  and runtime-specific libsys wrappers.
- An app's link-time memory lives in its manifest's `[link]` section
  (`stack_size`/`initial_memory`/`max_memory`, bytes), read at configure time by
  `wasmos_add_wasm_c_app_target`; passing the old
  `STACK_SIZE`/`INITIAL_MEMORY`/`MAX_MEMORY` arguments is a configure error naming
  the manifest. So one file describes an app: what it needs from the kernel
  (`[resources]`) and how its own linear memory is laid out (`[link]`), each number
  next to the reason for it. All 59 modules were byte-identical in size across the
  migration, and `tests/test_link_memory_manifest.py` checks every manifest that
  declares `[link]` against what its module actually declares — 21 modules across
  C, Zig, AssemblyScript and Rust, all four of which read the section, so it is the
  single check that the four toolchains agree about what a manifest means. The
  section is in bytes in every language; `asc` takes pages, and the conversion is in
  one place where a non-page-multiple is an error rather than a silent round.
- The in-tree build compiles through the SDK drivers in **every** language: no WASM
  target invokes a compiler directly any more. The C helper calls `wasmos-clang`,
  the Zig helper `wasmos-zig`, the AssemblyScript helper `wasmos-asc`, and the Rust
  and Go examples `wasmos-rustc` and `wasmos-tinygo`; each helper passes only its
  sources, output and manifest. So the flags live in one place per language instead
  of two, and the copy the tree exercises is the one an outside developer gets.
  Parameters that duplicated the manifest are gone and are now configure errors:
  `EXPORT`, `STARTUP_SHIM`, `NO_BUILTIN` (C), `LIBC_SRC`/`INCLUDE_DIRS` (Zig),
  `ENTRY_NAME`/`INITIAL_MEMORY_PAGES` (AssemblyScript). Two mechanisms went with
  them: the `wasmos-tinygo.json.in` target template and the Rust shim-object rules.
  Verified against a pre-switch tree: 60 of 62 C modules, all 6 AssemblyScript
  modules and all 5 Zig modules byte-identical; Go moved 4 bytes and the two Rust
  modules ~20, the latter because they now take the driver's small shadow stack
  (`examples/rust/hello` moved its data from 1 MB to `0x2000`).
- It also links `crt1.o`, `libc.a` and `libsys.a` from the staged sysroot instead of
  recompiling libc into each of the ~59 modules, so there is one libc build and
  one link line rather than two that can drift. `llvm-ar` is therefore required to
  build anything. Only entry shims are still compiled per target (an entry symbol
  must be present whether or not anything references it, which an archive will not
  guarantee). One behavioural consequence: a symbol defined by both an application
  and libc is no longer a duplicate-symbol error — the application's definition
  silently wins.
- A staged SDK (`cmake --build build --target wasmos-sdk`) repackages the C
  toolchain for use outside the repository: a relocatable sysroot with `crt1.o`,
  `libc.a` and `libsys.a`, and a `wasmos-clang` driver that supplies the triple,
  sysroot, wasm linker defaults and the `.wap` packaging step, so
  `wasmos-clang hello.c -o hello` produces a runnable package with no other flags.
  `wasmos-zig` does the same for Zig — staging the runtime shims flat so
  `@import("wasmos.zig")` resolves, always passing the mandatory 8 KiB shadow
  stack, and refusing to emit a module that fails the user-VA layout check — and
  `wasmos-asc` for AssemblyScript, staging the whole AS runtime flat beside the app
  because `asc` has no include path, with the coroutine transform and
  `--runtime stub` applied unconditionally; and `wasmos-rustc` for Rust, staging
  the binding as a sibling module and overriding rustc's 1 MB default shadow stack
  for the same layout reason as Zig. `examples/{c,zig,assemblyscript,rust}/sdk_hello`
  are built that way by every build and run in the guest by
  `tests/test_sdk_hello.py`, which checks console output, a real `argv[1]`, and an
  `open`/`read` that reaches the filesystem service over IPC; `tests/test_sdk_abi.py` asserts the module's import
  and export shape without booting. Stage 1 keeps LLVM's own
  `wasm32-unknown-unknown` target and puts the WASMOS knowledge in the driver,
  which reports `wasm32-unknown-wasmos`. The CMake integration
  (`share/cmake/WASMOS/WASMOSToolchain.cmake` plus a `Platform/WASMOS.cmake`)
  builds an out-of-tree project to a running `.wap`, and `wasmos-clang++` builds
  freestanding C++ (verified in the guest by hand, not yet in a battery). Not yet
  present: a wasm32 `libc++` (so no `<vector>`) and a native
  `wasm32-unknown-wasmos` LLVM triple. compiler-rt builtins are absent but measured
  to be needed only for `__int128` (eight symbols) — 64-bit arithmetic and float
  conversions are native wasm instructions — and `tests/test_sdk_arithmetic.py`
  pins that boundary in both directions. See `docs/toolchain.md`.
- `crt1` builds a real `argc`/`argv` for `wasmos_main` apps:
  `wasmos_startup_argv()` (`src/libc/src/spawn_info.c`) tokenizes the spawn-info
  argument string, `argv[0]` is an empty program-name slot so `argv[1]` is the
  first argument, and an argument that does not fit the buffer whole is dropped
  rather than truncated (`tests/unit/test_libc_startup_argv.c`). C only so far —
  the Rust, Go, Zig and AssemblyScript entry shims still pass an empty argument
  list.
- The C wasm link no longer passes `--allow-undefined`. It was never needed — no
  module in the tree carries an undeclared import — and it turned a missing source
  file into a module that loads and traps at the call site instead of a link
  error.
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
