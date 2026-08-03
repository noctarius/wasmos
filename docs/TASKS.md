# Active Tasks

This file is the agent-facing work index: it lists unfinished, actionable work
by subsystem. It is not a changelog and does not repeat completed baseline
work. See `STATUS.md` for the current implementation snapshot, `git log` for
history, and `ARCHITECTURE.md` for the complete document map.

## How to Use This File

- Treat each top-level section as an independent workstream. Read its linked
  architecture document before changing the subsystem.
- Keep tasks concrete and remove them when their done condition is met; record
  the resulting baseline in `STATUS.md`.
- Preserve ABI parity across wasm3, WARP, native, libc, and libsys variants.
- QEMU integration targets share `build/esp`; run them sequentially.
- Code citations (`file:line`) point at a live `TODO`/`FIXME` marker or the
  exact site of the gap; verify the line before acting (code moves).

## Kernel, Memory, and Isolation

Source: `architecture/06-memory-management.md`,
`architecture/10-capability-and-policy.md`,
`architecture/11-ring3-isolation-and-separation.md`, and
`architecture/28-smp.md`.

- [ ] Replace the WARP shared-linear-memory local fault retry with cross-CPU
  TLB-shootdown IPIs before any live page-table reclaim under concurrent APs
  (`src/kernel/arch/x86_64/cpu_x86_64.c:554` `TODO(smp-tlb)`,
  `src/kernel/warp/ring3_trampolines.c:166`).
- [ ] Move native `.wap` services from ring 0 to the ring-3 native execution
  path (syscall-backed `libsys_native` primitives + capability enforcement).
  Unblocks isolating `gfx-compositor`, `font-service`, and `net-stack`
  (`architecture/11`:380-414).
- [ ] Finish ring-3 hardening TODOs: drop `PML4[0]` from the kernel root once
  bootstrap no longer needs the low slot (`src/kernel/paging.c:338`
  `TODO(ring3-phase3)`); add process-local exception handling beyond the
  current `-11` kill (`src/kernel/arch/x86_64/cpu_x86_64.c:484`
  `TODO(ring3-phase5)`); handle stack allocations that can exceed the 512 MB
  higher-half window (`src/kernel/process.c:131` `TODO(ring3-phase2)`).
- [ ] Tighten the temporarily-broadened page-fault reason back to
  `EXEC_VIOLATION`-only once the ring-3 fault tests are stable
  (`src/kernel/arch/x86_64/cpu_x86_64.c:578`).
- [ ] Introduce allocation intents (`STACK`, `PGTABLE`, `DMA32`, `GENERIC`),
  then remove the low-memory DMA constraint from kernel stacks and page tables
  (`src/kernel/paging.c:31`; `architecture/06`:455-497).
- [ ] Decide and implement kernel reachability beyond the current 512 MB low
  window: full physmap or a bounded `kmap` cache (`architecture/06`).
- [ ] Route pointer-bearing syscall/IPC entry paths through
  `mm_user_range_permitted` (`src/kernel/memory.c:1204`).
- [ ] Replace the 64-bit-bitmap linmem-slot ceiling with a growable pool so
  concurrent linear-memory slots are bounded by memory, not 64
  (`src/kernel/linmem_slots.c:15` `TODO(linmem-pool)`).
- [ ] Move the process table off its static array onto kernel list storage
  (`src/kernel/process.c:36` `FIXME(process-list)`).
- [ ] Replace the global shared-region cap, make context region sizing
  configurable, and add committed/resident (RSS) memory accounting for `ps`
  (`src/kernel/process.c:2492` `TODO(memory-rss)`; `architecture/06`:507-512).
- [ ] Wire the kernel-thread trampoline into PM launch policy and delete the
  legacy trampoline (`src/kernel/process.c:1503,1771`).
- [ ] Fix the `dma_map_borrow` capability-enforcement divergence: the WARP
  wrapper (`src/kernel/warp/link.cpp`) omits the DMA-capability + max-bytes/range
  check that the wasm3 wrapper (`src/kernel/wasm3/link.c`) enforces, so the WARP
  path is weaker. Bring WARP to parity. Found during the host-call ABI inventory.
- [ ] Fix the `warp_ring3_dispatch` `proc_info_stats` ctx bug: the case passes
  `ctx5` (== `a4`, the `stats` param) as the kernel ctx, but a 5-param host call
  needs ctx in `a5` (R9) — there is no `ctx6`, so the hand-written case silently
  reused `ctx5`. Ring-3 `proc_info_stats` therefore gets a garbage ctx (a user
  offset) → wrong `warp_mem` resolution. The generated dispatch computes
  `ctx = a<arity>` and fixes it; fix lands when the ring-3 dispatch is swapped in.
  Found during host-call dispatch codegen (`src/kernel/warp/link.cpp:3130`).
- [ ] Extend DMA isolation to an IOMMU domain model (VT-d/AMD-Vi) and add
  non-coherent cache-maintenance hooks before targeting non-coherent hardware
  (`architecture/12`:88,618,625).
- [ ] Harden the boot and native ELF loaders with checked arithmetic for
  program-header offsets, segment file/virtual ranges, boot-info layout totals,
  and cursor advances; reject values that overflow allocation sizes or boot ABI
  fields before copying.

## Scheduler, Threads, and IPC

Source: `architecture/07-scheduling-and-preemption.md`,
`architecture/08-threading-and-lifecycle.md`,
`architecture/09-process-and-ipc.md`, `architecture/29-threadable-scheduler.md`,
`architecture/30-ipc-direct-switch.md`, `architecture/32-coroutines-futures-promises.md`,
and `architecture/33-completion-ports.md`.

- [ ] Add scheduler/process observability: committed-memory-aware process
  reporting (feeds `memory-rss`), scheduler latency/stall counters, and useful
  per-process metrics.
- [ ] Define a fairness/budget policy now that the priority bands are actually
  wired (`pm_sched_prio_for_flags` → `process_set_main_prio`); measure first and
  keep the existing preemption and SMP regression gates.
- [ ] Surface futex to userspace. The kernel primitive exists (`futex_wait/wake`
  in `src/kernel/futex.c`, WASM hostcalls at `src/kernel/wasm3/link.c:3800`) but
  is absent from libc `api.h` and the native `int 0x80` path; user mutexes still
  yield-spin (`src/libsys/wasm/include/wasmos/mutex.h:44`,
  `src/libsys/native/include/wasmos/libsys_native.h:296`,
  `src/libc/include/wasmos/mutex.h:44`). Add the declarations + native syscall
  and make the user mutex consume it.
- [ ] Promote libsys event-loop intents into the shared future/promise contract
  with one receive pump per endpoint and request-id/generation cancellation.
- [ ] Remove synchronous request/reply IPC from libc, libsys, native wrappers,
  and remaining service call sites (nested `ipc_select_one` reply-waits). The
  future/promise bridge has landed and net-stack uses it, but the
  fs-manager↔device-manager sync-round-trip deadlock hazard remains
  (`architecture/09` synchronous-IPC section; `src/services/fs_manager/fs_manager.c:608`).
- [ ] Publish `POLL_EV_IN` on the notification path so NOTIFICATION endpoints
  are visible to `ipc_select_wait`: `ipc_notify_from` does not call
  `poll_notify` (`src/kernel/ipc.c:363-385`). Prerequisite for completion ports.
- [ ] Remove the legacy `process_block_on_ipc` shim once all callers move to the
  select/idle-wait path (`src/kernel/process.h:225`, `src/kernel/process.c:1588`).
- [ ] Reconcile `architecture/30-ipc-direct-switch.md` (fully unimplemented; its
  header flags conflict with the futures direction) with the futures model, or
  formally drop it. Do not add a direct-switch API that reintroduces nested
  blocking IPC.
- [ ] Define a PM-mediated cooperative lifecycle-control protocol over IPC:
  capability-gated shutdown/cancel, acknowledgement, deadline-based escalation
  to `process_kill`, and event-loop safe points. No POSIX signal handlers or
  arbitrary asynchronous thread interruption.
- [ ] Add asynchronous, capability-gated process-death watches for supervisors
  and integrate them with the lifecycle-control event path.
- [ ] Normalize request-id validity across WASM and native `libsys` (signed vs
  unsigned wire representation and the reserved invalid value).
- [ ] Make console-backed libc `read`/`write` reject or chunk counts beyond the
  `int32_t` ABI limit and return the actual byte count from the console backend.
- [ ] Add the AssemblyScript async/coroutine wrappers over the shared
  future/promise contract (native and WASM cores plus Rust/Go/Zig are done;
  AssemblyScript remains deferred).
- [ ] Defer true WASM parallelism and hard coroutine preemption until runtime
  locking/reentrancy has a dedicated design and validation plan.
- [ ] Implement completion ports (`architecture/33`, design proposal only): a
  kernel-owned bounded CQ with notification doorbells and generation-tagged
  operation tokens, as a batched completion source for the future/promise
  runtime and high-rate networking.

## Runtime, Packaging, and Service Discovery

Source: `architecture/13-runtime-and-packaging.md`,
`architecture/14-libsys-and-service-runtime.md`, and
`architecture/15-drivers-and-services.md`.

- [ ] Close the remaining WARP refinement TODOs (host-call coverage itself is
  broad): synchronise symbol lookups/alloc under SMP (`src/kernel/warp/link.cpp:90`
  `TODO(smp-warp)`, `src/kernel/warp/shim.cpp:579` `FIXME(smp-warp)`); reserve
  shmem auto-map windows against real heap growth (`link.cpp:1988`); write-combining
  PAT for framebuffer/scanout (`link.cpp:2145`); enable W^X once kernel paging
  supports per-4K remap (`src/kernel/warp/posix_kernel.c:97`). Provide a supported
  alternative to the vendored-runtime-pointer shim without modifying `libs/warp`.
- [ ] Make WARP multithreaded WASM either functional (locate the owner module
  bytes so VM threads run under WARP — `src/kernel/warp_driver.cpp:952`
  `module_bytes = nullptr`) or explicitly unavailable at the API boundary.
- [ ] Fix (or confirm resolved) the WARP ring-3 delegated-executor argv
  coherence bug: `wasmos_script`'s first xfer-buffer read of argv can read as
  zero because the kernel higher-half alias and the ring-3 user-VA view diverge
  for that page (wasm3 is unaffected). Verify against a repro before asserting
  fixed (`architecture/13`:549-551; STATUS known non-green path).
- [ ] Restore prior linear-memory PTEs on wasm3 `xfer_buffer`/`shmem` unmap
  instead of only dropping shared refcounts (`src/kernel/wasm3/link.c:1553,1563,2504,2552`
  `FIXME(xfer-unmap)`/`FIXME(shmem-map-auto)`).
- [ ] Replace the wasm3 parent-name spawn heuristic with explicit per-process
  identity (`src/kernel/wasm3/link.c:589` `FIXME`).
- [ ] Track native VM `malloc`/`free` per-PID so a reaped process's native heap
  pages are reclaimed (`src/kernel/native_driver.c:597` `TODO(nd-vm)`; native
  services are currently long-lived, so this leaks).
- [ ] Complete executable-broker handoff: route non-builtin subsystem handler
  kinds to a userland broker instead of returning `-1` (`src/kernel/wasmos_app.c:797`),
  ensure delegated argv/transfer-buffer reads are coherent, define failure
  handling, and add end-to-end broker tests.
- [ ] Finish service-class discovery lifecycle: enumeration,
  add/remove/death notifications, capability-gated registration, and consumer
  migration where class lookup removes hardwired provider names.
- [ ] Add driver/service supervision, restart/reincarnation, and controlled
  capability revoke/reissue on restart.

## ABI, Code Generation, and Error Handling

Source: `architecture/34-abi-idl-and-error-model.md`.

Phase order: 1 (error foundation, done) → 2 (host calls) → 3 (opcodes) → 4
(migrate call sites onto the generated surfaces). The error call-site migration
is deliberately last: it rewrites service/driver code that Phases 2/3 also
rewrite, so doing it after the generated host-call/opcode surfaces exist touches
each site once. Error domains ride those surfaces (`SHMEM_ERR_*` are host-call
returns; `FS_ERR_*`/`PROC_*` ride IPC opcodes), so the migration depends on them.

- [x] Phase 1 (error foundation) — COMPLETE: `abi/errors.yaml` IDL +
  `scripts/gen_abi_errors.py` generator + the per-language value ABI under
  `abi/generated/<lang>/wasmos_status.{h,rs,go,zig,ts}` — transport
  `wasmos_status_t`, generated domain registry (stable ids), packed
  `(domain, code)` constants seeded from the legacy taxonomy, decode lookups
  (`wasmos_strerror`), the fixed 8-byte frame / 40-byte error object, and the
  chain helpers (`wrap`/`unwrap`/`root`/`is`/`as`), each verified to compile
  (Rust/Go/C also run-tested) with its native toolchain. Generated files sit
  outside the `src/` format/lint scope. `quality` gained a `--check` re-gen
  guard and an advisory bare-`return -1;` inventory (services/drivers, ~24
  sites). Not yet wired into the OS — wiring is Phase 4.
- [x] Phase 2a: `abi/hostcalls.yaml` (all 117 host calls + the id-less wasm3-only
  `env.strlen`) + `scripts/gen_abi_hostcalls.py` generating the `HC_*` id enum,
  with Model validation (ids unique, dense `0..N-1`, ordered; `reserved` slots for
  retirements) and `--verify-source` proving the IDL's `(symbol, id)` set matches
  the live `src/kernel/include/warp_ring3.h` exactly. Wired into `quality`
  (`--check` + `--verify-source`). Param **kinds** captured but not yet emitted.
- [x] Phase 2b: generate the WARP `WASMOS_SYMBOLS(LINK)` table
  (`abi/generated/c/wasmos_symbols_warp.inc`) and the wasm3 link table
  (`wasmos_link_wasm3.inc`, m3 sig strings derived from param kinds + the
  `wasm3: i32` overrides for the 10 pointers wasm3 passes as a raw offset). Both
  `--verify-source`-proven to reproduce the live `link.cpp`/`link.c` exactly
  (117 host calls; ids, names, wrapper fn names, and sigs all match), wired into
  `quality`. `warp_fn`/`wasm3_fn` are derived (only the 5 `ipc_select_*` family
  fns need an explicit `wasm3_fn` override). Still parallel artifacts — not wired.
- [x] Phase 2c (AOT): generate the WARP AOT symbol table
  (`wasmos_symbols_aot.inc`, `WASMOS_AOT_SYMBOLS(LINK)`) — each entry a
  `stub_i<arity>`; validated against `warp_aot.cpp`. The AOT table is
  name-resolved (not position-coupled — only ring-3 dispatch is), so the
  generator emits the full host-call set and `--verify-source` asserts the live
  table is a subset (it currently omits `env.abort`/`wasi.proc_exit`; the
  generated set completes it, a safe additive change to validate at swap time).
- [x] Phase 2c (ring-3 dispatch): generate `warp_ring3_dispatch_table()`
  (`wasmos_ring3_dispatch.inc`) — a self-contained inline function
  (frame-decode stays hand-written, dispatch is generated). ctx is the computed
  `a<arity>`, which fixes the `proc_info_stats` bug. Compile-checked clean under
  `clang++ -Wall -Wextra -Werror` against arity-derived wrapper decls (proves
  every case's arg count matches its wrapper); `--verify-source` also confirms
  the per-id wrapper fn matches the live switch.
- [x] First swap (proves the loop): `wasm3_link_wasmos` (`src/kernel/wasm3/link.c`)
  now `#include`s the generated `wasmos_link_wasm3.inc` and expands
  `WASMOS_WASM3_LINKS(X)` instead of the hand-written `rc |= wasm3_link_raw(...)`
  cascade (129 lines retired). `abi/generated/c` added to `CFLAGS_KERNEL`. Live in
  the default wasm3 build: `run-qemu-test` boots through to CLI + calculator +
  halt, and `run-kernel-unit-tests` pass.
- [x] WARP kernel surfaces swapped in: the HC enum (`warp_ring3.h`),
  `WASMOS_SYMBOLS` (`link.cpp`), and the ring-3 dispatch
  (`warp_ring3_dispatch` now decodes the frame and calls the generated
  `warp_ring3_dispatch_table`). Validated by a WARP `run-qemu-test` (boots to
  CLI + calculator + halt) and the default wasm3 `run-qemu-test` (the enum header
  is shared). The `proc_info_stats` ctx fix is now live in the WARP kernel.
- [x] AOT tool table swapped in: `src/tools/warp_aot/warp_aot.cpp` now
  `#include`s the generated `wasmos_symbols_aot.inc` and expands
  `WASMOS_AOT_SYMBOLS(DYNAMIC_LINK)` (return-matched `stub_i<N>`/`stub_v<N>`).
  All kernel-side + AOT host-call tables are now generated from the IDL. WARP
  `run-qemu-test` boots with "using AOT binary" (payloads rebind against the
  generated table).
- [x] Phase 2c (client stubs): generate the guest import bindings for the four
  languages that hand-roll them — `abi/generated/{rust,go,zig,assemblyscript}/wasmos_imports.*`
  — every `wasmos`-module host call (incl. aliases) as its raw wasm ABI signature
  (all params `i32`, `i32` return), idioms matched to the in-tree examples
  (Rust `#[link(wasm_import_module)]`, Go `//go:wasmimport`, Zig `pub extern
  "wasmos" … callconv(.c)`, AS `@external`). Each compile-verified with its real
  toolchain (`zig ast-check`, `rustc --target wasm32 -Dwarnings`, `go vet`
  `GOOS=wasip1`, `asc`) and wired into the `quality` `--check` guard. C is
  deliberately NOT regenerated: `src/libc/include/wasmos/api.h` is a
  hand-ergonomic surface (typed pointers, struct params like
  `wasmos_physmem_stats_t*`, `_host` fn-name conventions, doc comments) whose
  types are not in the language-neutral IDL — regenerating it would either break
  ~112 call sites or force a full C type/fn-name/doc vocabulary into the IDL.
  Instead `--verify-source` now guards it: every `WASMOS_WASM_IMPORT("wasmos", …)`
  decl in `src/libc`/`src/libsys` must name a real IDL host call with a matching
  arity, so `api.h` can never silently drift (found + removed a duplicate
  `ipc_select_*` decl block; the native-vtable-only `mutex_try_lock`/`mutex_unlock`
  pair is allow-listed pending the futex migration). The wasi/env-module calls
  are toolchain-provided, not ours to declare. `--verify-source` is therefore
  repurposed as the permanent hand-written-C-surface guard (not retired); the
  swapped kernel tables self-skip, and `--check` guards the generated files.
  (`dma_map_borrow` is a wrapper-body divergence, tracked separately under
  Kernel — not a table swap. An ABI-version `static_assert` was dropped as
  meaningless client-side: the guest has no second source of truth to assert the
  count/version against — that check only makes sense kernel-side, where
  `HC_COUNT` is itself generated.)
- [ ] Phase 3: add `abi/opcodes.yaml` + generator producing the
  `wasmos_driver_abi.h` opcode enum, a runtime `opcode → name` table (feeds
  diagnostics), and the doc opcode tables; optionally typed future-returning
  request/reply stubs carrying the transfer-buffer ownership contract.
- [ ] Phase 4 (after 2 and 3): migrate the tree onto the packed error model —
  the legacy `PROC_SPAWN_ERR_*`/`PROC_PM_ERR_*`/`FS_ERR_*` (IPC opcodes) and
  `SHMEM_ERR_*` (host calls) definitions and call sites move onto the generated
  `(domain, code)` reply status; wire the pass-through-with-provenance
  propagation policy (wrap = append a frame, only at deliberate abstraction
  seams); clear the bare-`return -1;` backlog; then flip the advisory `-1` gate
  to a hard failure (allow-listing genuine POSIX-ABI boundaries). One subsystem
  at a time.
- [ ] Extend the `quality` re-gen guard to the host-call and opcode generators
  as they land (the errors guard already exists), so generated output can never
  silently drift from the IDL.

## Filesystems and Storage

Source: `architecture/18-filesystem-stack.md` and
`architecture/12-dma-transfers.md`.

- [ ] Implement FAT32 cluster read/write in the FAT-table layer: FAT32 is
  detected at mount (`fat_geom.c:92`) but `fat_fatent_read`/`fat_fatent_write`
  return `FS_ERR_CORRUPT` and `fat_chain_next` decodes only FAT12/16 and stores
  clusters as `uint16_t` (truncation) (`src/drivers/fs_fat/fat_alloc.c:43-44,83-84,148-154`).
  Done when a FAT32 `/user` volume mounts and round-trips a file.
- [ ] Apply the non-blocking reactor model to `fs-init` (currently a blocking
  dispatcher with no SEEK/STAT — `src/drivers/fs_init/fs_init.c:498-569`) and
  preserve the transfer-buffer ownership contract through all VFS relay paths.
- [ ] Re-enable ATA bus-master DMA under the owner-push ABI: `ata_dma_prepare`
  is stubbed to `WASMOS_DMA_STATUS_DENY` so every op is PIO
  (`src/drivers/ata/ata.c:248-264`). Carry the client `borrow_id` in the block
  IPC and map via `dma_map_borrow`, then drive the PRDT/descriptor path.
- [ ] Complete initfs zero-copy mapping with an explicit entry-offset ABI and
  correct revoke/lifetime behavior (still copy-based today).
- [ ] Extend LFN creation beyond ASCII: new-file LFN entries currently store
  `?`-mapped ASCII, not UTF-16 (`src/drivers/fs_fat/fat_name.c:175`; read-side
  LFN already works).
- [ ] Fix FAT12/16 `..` self-reference and cross-cluster-boundary parent
  assumptions (`src/drivers/fs_fat/fat_dir.c:771,775`).
- [ ] Port the reactor open-file table into `fat_file` so `fat_dir` reads it
  there rather than the stubbed path (`src/drivers/fs_fat/fat_dir.c:339`,
  `fat_dir.h:54`).
- [ ] Refetch fs-manager boot metadata out-of-band (push/idle-step) to remove
  the nested synchronous `DEVMGR_QUERY_MOUNT_REQ` deadlock hazard during
  class discovery (`src/services/fs_manager/fs_manager.c:608`).
- [ ] Guard FAT file-capacity growth against `uint32_t` overflow and reject
  writes not representable by the on-disk/open-file size fields.
- [ ] Expand FAT coverage deliberately: FAT32 update modes and behavioral tests
  for each added contract.
- [ ] Evaluate additional filesystems and dynamic mount lifecycle only after the
  existing VFS/backends have clear mount, ownership, and recovery semantics.

## Device Drivers and Input

Source: `architecture/16-device-manager-and-bus-enumeration.md`,
`architecture/17-console-io-and-character-device.md`, and
`architecture/21-virtual-input-testing-via-virtio-serial.md`.

- [ ] Implement virtio-serial queue setup plus data/control-plane byte-stream
  IPC; discovery/register access alone cannot transport host data
  (`src/drivers/virtio_serial/virtio_serial.c:160` `TODO(virtio-serial-transport)`).
- [ ] Build the `virt-input` service and host bridge after virtio-serial data
  transport exists; inject keyboard/mouse events through the normal compositor
  IPC path and add sequential QEMU UI automation tests (no source exists yet;
  `architecture/21`).
- [ ] Wire parsed device-manager rules into runtime bind/unbind/mount policy;
  spawn already works but the rule actions are still informational
  (`src/services/device_manager/device_manager.c:127`).
- [ ] Split a dedicated `irq.configure` capability from `irq.route` for
  level/active-low configuration (`src/services/pci_bus/linker.metadata:25`;
  kernel side `src/kernel/warp/link.cpp:2414`).
- [ ] Preserve each driver module's declared IRQ capability mask in
  device-manager metadata instead of granting the fixed IRQ 14/15 pair.
- [ ] Remove the now-dead DMA-window defaulting in the individual
  `PROC_IPC_SPAWN_*_CAPS` handlers (`process_manager_spawn.c`); DMA windows are
  now installed from the driver's `dma.buffer` manifest capability
  (`capability_grant_name`), not the spawner.
- [ ] Consolidate the per-file `#define PAGE_SIZE 0x1000` copies
  (physmem.c/process.c/memory.c/native_driver.c/capability.c/…) into one shared
  header.
- [ ] Replace the fixed `DEVMGR_RULE_TEXT_CAP` rules-file read buffer (now 4096)
  with an `FS_IPC_STAT_REQ`-sized (or streaming/chunked) read so the rules file
  has no size limit; a fixed buffer silently truncates trailing rules.
- [ ] Add hotplug/event publication and future bus providers (USB/virtual)
  through the normalized device-record contract.

## Networking

Source: `architecture/22-networking-virtio-net-and-stack.md`.

Baseline done and e2e-validated: virtio-net driver, netif bring-up, UDP,
TCP client (connect/stream/close) and server (listen/accept/echo) over rings,
DHCP/static addressing, DNS (`NET_IPC_RESOLVE`), and a verifying TLS 1.2 client.
Remaining:

- [ ] Harden TCP timeout/retransmit and the close path: drive
  `sys_check_timeouts()` from a dedicated timer source rather than only the idle
  loop, with retransmit/close e2e tests (`src/services/net_stack/net_stack.c:2646`;
  STATUS `Current Gaps`).
- [ ] Add IPv6 / NDP / ICMPv6 / dual-stack, multiple addresses per interface,
  and isolated multi-stack instances (`src/services/net_stack/lwipopts.h:41`
  `LWIP_IPV6 0`).
- [ ] Validate TLS certificate dates: wire an RTC time source into mbedTLS
  (`MBEDTLS_HAVE_TIME`/`HAVE_TIME_DATE`) so validity windows are checked.
- [ ] Harden TLS/large-transfer RX-ring backpressure so big bodies do not stall
  on a full RX ring (app-side flow control).
- [ ] Enable guest-to-guest loopback (`LWIP_NETIF_LOOPBACK` + net-stack loopback
  polling) so an in-guest server is reachable.
- [ ] Complete PCI INTx polarity/trigger configuration, then migrate virtio-net
  to per-vq MSI-X so RX interrupts re-deliver per notification and the
  timed-poll workaround drops to a plain blocking wait
  (`src/drivers/virtio_net/virtio_net.c:631,871` `TODO(msi-x)`).
- [ ] Define the net owner-push wire protocol so TX/RX carry an explicit
  client `buffer_id`/grant instead of overloading `msg.arg0`/`arg1`
  (`src/drivers/virtio_net/virtio_net.c:479,718,752,775,900` `FIXME(owner-push)`).
- [ ] Add a multi-interface ifcfg selector so boot config can target non-default
  interfaces (`src/services/net_stack/net_stack.c:977` `FIXME(multinet-ifcfg)`).
- [ ] Evaluate a packet DMA fast path that removes the RX copy (only after the
  negative-path/restart/link-down test coverage lands).
- [ ] Minor: expand the net-stack lwIP diagnostic `vprintf`
  (`net_stack.c:316`) and source net-stack's clock directly from a native
  driver-api millisecond hook (`src/services/net_stack/port.c:34`).

## Graphics, VT, and User Space

Source: `architecture/19-virtual-terminal.md`,
`architecture/20-graphics-framebuffer-and-compositor.md`,
`architecture/23-cli-and-user-space.md`, and
`architecture/24-environment-scopes-and-inheritance.md`.

VT I/O-multiplexer phase 5 (remaining; phases 0–4 shipped):

- [ ] Make vt-1 default-visible at boot (`src/services/vt/vt_main.c:21`,
  `g_active_tty = 0`).
- [ ] Retire the framebuffer-PCI console-ring drain so the framebuffer is a pure
  blit surface (`src/drivers/framebuffer_pci/framebuffer_pci_native.c:41,233,336`).
- [ ] Add the serial-bound-slot selector (`VT_IPC_BIND_SERIAL_REQ`, undefined;
  `g_serial_tty` is fixed at 1 — `vt_main.c:24`).
- [ ] Lazy per-slot CLI spawn: have the VT spawn `cli.wap` pinned to a slot on
  first switch and drop `start cli.wap` from `sysinit.rc`.

Other graphics/VT/UI:

- [ ] Reclaim old libui font shared-memory objects when text buffers grow
  (`src/libui/include/libui.h:471`) and the compositor's title-glyph shmem IDs
  on growth (`src/services/gfx_compositor/gfx_compositor.zig:2007`).
- [ ] Make `libui`, font-service, and compositor allocation/index arithmetic
  overflow-safe: validate dimensions before multiplication, compute buffer
  indexes in `usize`, and cap growth before capacity doubling.
- [ ] Add an explicit mode-set / update-framebuffer-info path before the VBE
  reprogram after ExitBootServices
  (`src/drivers/framebuffer/framebuffer_native.c:159`,
  `src/drivers/framebuffer_pci/framebuffer_pci_native.c:275`).
- [ ] Make VT cell/reply/replay writes robust under framebuffer backpressure;
  they are best-effort today and drop on persistent queue-full
  (`src/services/vt/vt_main.c:244,287,892`).
- [ ] Reproduce and fix the deferred rapid-TTY-switch prompt
  duplication/misalignment (`src/services/vt/vt_main.c:1755`); keep deferred
  until a stable repro exists.
- [ ] Add a real WASM link step to the AssemblyScript `libui` build (currently a
  stub — `src/libui/assemblyscript/libui.ts:133`).
- [ ] Add script-engine diagnostics for unclosed `if` blocks (the EOF
  `total_depth > 0` warn path is a no-op) and preserve the documented `script`
  vs `source` environment-scope semantics.
- [ ] Expand VT behavior only from an explicit compatibility need: richer ANSI,
  UTF-8, scrollback, or input APIs should each include focused behavioral tests.

## Validation and Documentation

Source: `architecture/25-diagnostics-status.md`,
`architecture/26-repo-map-and-validation.md`, and
`architecture/27-python-test-framework.md`.

- [ ] Add behavioral regression coverage with every new subsystem contract;
  reject source-text assertions.
- [ ] Add focused stress/negative tests for TLB shootdown, service restart,
  futures cancellation, virtio-serial transport, networking link-down/restart,
  and new DMA paths.
- [ ] Add graphical input-injection tests only after virtio-serial transport is
  usable; use distinct sockets and never run QEMU sessions in parallel.
- [ ] Extend `scripts/kconfig_to_cmake.py:37` symbol map as more CMake cache
  settings migrate to Kconfig, and make `scripts/quality.sh:133` clang-tidy lint
  C++ sources (missing `--extra-arg` flags).
- [ ] Keep architecture documents authoritative for design, `STATUS.md` concise
  for current behavior, and this file limited to unfinished work.
