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

## Kernel, Memory, and Isolation

Source: `architecture/06-memory-management.md`,
`architecture/10-capability-and-policy.md`,
`architecture/11-ring3-isolation-and-separation.md`, and
`architecture/28-smp.md`.

- [ ] Replace the WARP shared-linear-memory local fault retry with cross-CPU
  TLB shootdown for mapping updates.
- [ ] Move native `.wap` services from ring 0 to the ring-3 native execution
  path, including syscall-backed `libsys_native` primitives and capability
  enforcement. This unblocks isolation of `gfx-compositor`, `font-service`,
  and `net-stack`.
- [ ] Complete remaining ring-3 hardening TODOs: move framebuffer/serial
  privileged paths behind explicit boundaries, finish CPU/process cleanup, and
  retain fault-containment coverage.
- [ ] Introduce allocation intents (`STACK`, `PGTABLE`, `DMA32`, `GENERIC`),
  then remove low-memory DMA constraints from kernel stacks and page tables.
- [ ] Decide and implement kernel reachability beyond the current low physical
  window: full physmap or bounded `kmap` cache.
- [ ] Replace the global shared-region cap, make context region sizing
  configurable where appropriate, and add committed/resident memory accounting
  for `ps`.
- [ ] Extend DMA isolation from capability windows to an IOMMU domain model
  when VT-d/AMD-Vi support is introduced; add non-coherent cache-maintenance
  hooks before targeting non-coherent hardware.
- [ ] Harden boot and native ELF loaders with checked arithmetic for program
  header offsets, segment file/virtual ranges, boot-info layout totals, and
  cursor advances; reject values that overflow allocation sizes or boot ABI
  fields before copying.

## Scheduler, Threads, and IPC

Source: `architecture/07-scheduling-and-preemption.md`,
`architecture/08-threading-and-lifecycle.md`,
`architecture/09-process-and-ipc.md`, `architecture/29-threadable-scheduler.md`,
`architecture/30-ipc-direct-switch.md`, and
`architecture/32-coroutines-futures-promises.md`.

- [ ] Add scheduler/process observability: committed-memory-aware process
  reporting, scheduler latency/stall counters, and useful per-process metrics.
- [ ] Define priority/budget policy after measuring the current scheduler;
  retain the existing preemption and SMP regression gates.
- [ ] Promote libsys event-loop intents into the shared future/promise contract,
  with one receive pump per endpoint and request-id/generation cancellation.
  This replaces nested synchronous request/reply waits.
- [ ] Remove synchronous IPC from libc, libsys, native wrappers, and service
  call sites. Replace blocking request/reply helpers and nested receive loops
  with non-blocking sends resolved by the shared future/promise event pump.
- [ ] Surface futex wait/wake in every user ABI: WASM libc imports and native
  `int 0x80`/`libsys_native` paths. Keep physical-address/shmem semantics and
  runtime variants aligned.
- [ ] Build the native stackful coroutine runtime after futex exposure: guarded
  stacks, assembly context switch, worker queues, cancellation, joins, and
  timers in `libsys/native`.
- [ ] Add the WASM coroutine layer over the same future/promise contract:
  cooperative fibers first, then language-native stackless async wrappers for
  Rust, Go, Zig, and AssemblyScript. Do not assume WARP/wasm3 guest stacks can
  use the native stack switch.
- [ ] Defer true WASM parallelism and hard coroutine preemption until runtime
  locking/reentrancy has a dedicated design and validation plan.
- [ ] Reconcile `architecture/30-ipc-direct-switch.md` with the newer futures
  direction before implementing its synchronous fast-path phases. Do not add a
  direct-switch API that reintroduces nested blocking IPC.
- [ ] Define a PM-mediated cooperative lifecycle-control protocol over IPC:
  capability-gated shutdown/cancel requests, acknowledgement, deadline-based
  escalation to `process_kill`, and event-loop safe points. Do not add POSIX
  signal handlers or arbitrary asynchronous thread interruption.
- [ ] Add asynchronous, capability-gated process-death watches for supervisors
  and integrate notifications with the lifecycle-control event path.
- [ ] Normalize request-id validity across WASM and native `libsys`, including
  the signed/unsigned wire representation and the reserved invalid value.
- [ ] Make console-backed libc `read` and `write` reject or chunk counts beyond
  the `int32_t` ABI limit and return the actual byte count reported by the
  console backend.

## Runtime, Packaging, and Service Discovery

Source: `architecture/13-runtime-and-packaging.md`,
`architecture/14-libsys-and-service-runtime.md`, and
`architecture/15-drivers-and-services.md`.

- [ ] Close remaining WARP hostcall gaps and provide a supported alternative to
  the local runtime-pointer access shim without modifying `libs/warp`.
- [ ] Make WARP multithreaded WASM either functional with explicit runtime
  synchronization or explicitly unavailable at the API boundary.
- [ ] Complete executable-broker handoff: ensure delegated argv/transfer-buffer
  reads are coherent, define failure handling, and add end-to-end broker tests.
- [ ] Finish service-class discovery lifecycle behavior: enumeration,
  add/remove/death notifications, capability-gated registration, and consumer
  migration where class lookup removes hardwired provider names.
- [ ] Add driver/service supervision, restart/reincarnation, and controlled
  capability revoke/reissue on restart.
- [ ] Extend device-manager from bootstrap sequencing to lifecycle management:
  normalized provider events, hotplug, and policy-driven reconciliation.

## Filesystems and Storage

Source: `architecture/18-filesystem-stack.md` and
`architecture/12-dma-transfers.md`.

- [ ] Apply the non-blocking reactor model to `fs-init` and preserve the
  transfer-buffer ownership contract through all VFS relay paths.
- [ ] Replace ATA PIO with a real bus-master DMA engine (PRDT/descriptor path)
  once the DMA/IOMMU work provides a safe foundation.
- [ ] Complete initfs zero-copy mapping only with an explicit entry-offset ABI
  and correct revoke/lifetime behavior.
- [ ] Expand FAT coverage deliberately: FAT32, update modes, non-ASCII LFN
  creation, and behavioral tests for each added contract.
- [ ] Guard FAT file-capacity growth against `uint32_t` overflow and reject
  writes that cannot be represented by the on-disk and open-file size fields.
- [ ] Evaluate additional filesystems and dynamic mount lifecycle only after the
  existing VFS/backends have clear mount, ownership, and recovery semantics.

## Device Drivers and Input

Source: `architecture/16-device-manager-and-bus-enumeration.md`,
`architecture/17-console-io-and-character-device.md`, and
`architecture/21-virtual-input-testing-via-virtio-serial.md`.

- [ ] Implement virtio-serial queue setup plus data/control-plane byte-stream
  IPC. Discovery/register access alone cannot transport host data.
- [ ] Build the `virt-input` service and host bridge after virtio-serial data
  transport exists; inject keyboard/mouse events through the normal compositor
  IPC path and add sequential QEMU UI automation tests.
- [ ] Resolve PCI INTx polarity/trigger configuration so `virtio-net` RX
  notifications become reliable push events rather than polling hints.
- [ ] Preserve each driver module's declared IRQ capability mask in
  device-manager metadata instead of granting the fixed IRQ 14/15 pair.
- [ ] Remove the now-dead DMA-window defaulting still set in the individual
  `PROC_IPC_SPAWN_*_CAPS` handlers (`process_manager_spawn.c`); DMA windows are
  now installed from the driver's dma.buffer manifest capability
  (`capability_grant_name`), not the spawner.
- [ ] Consolidate the per-file `#define PAGE_SIZE 0x1000` copies
  (physmem.c/process.c/memory.c/native_driver.c/capability.c/…) into one shared
  header.
- [ ] Replace the fixed `DEVMGR_RULE_TEXT_CAP` rules-file read buffer with a
  `FS_IPC_STAT_REQ`-sized (or streaming/chunked) read so the rules file has no
  size limit at all. A fixed buffer silently truncates and drops trailing rules
  once the file exceeds the cap; 4096 is only an interim bump from 1024.
- [ ] Detect console-ring producer laps in the framebuffer consumer and advance
  the read cursor to a bounded live position before replaying overwritten data.
- [ ] Add hotplug/event publication and future bus providers (USB/virtual)
  through the normalized device-record contract.

## Networking

Source: `architecture/22-networking-virtio-net-and-stack.md`.

- [ ] Phase 2: turn the native lwIP scaffold into `net-stack`: netif glue,
  driver control plane, ARP/IPv4/ICMP/UDP, socket IPC, and UDP echo validation.
- [ ] Implement the shared-memory socket ring data plane with explicit transfer
  buffer grants, mapping lifetime, doorbells, and consumer-side validation.
- [ ] Phase 3: add TCP connect/listen/accept/send/recv/close and a periodic
  `sys_check_timeouts()` wake source, with TCP echo and retry/close tests.
- [ ] Phase 4: add IPv6, NDP/ICMPv6, dual-stack socket behavior, multiple
  addresses per interface, and isolated stack instances.
- [ ] Phase 5: add negative-path/restart/link-down tests, diagnostics, and only
  then evaluate a packet DMA fast path that removes the RX copy.
- [ ] Decide initial address configuration (static versus DHCP), final service
  naming, TCP baseline scope, and timer wake mechanism before committing their
  public ABI.

## Graphics, VT, and User Space

Source: `architecture/19-virtual-terminal.md`,
`architecture/20-graphics-framebuffer-and-compositor.md`,
`architecture/23-cli-and-user-space.md`, and
`architecture/24-environment-scopes-and-inheritance.md`.

- [ ] Reclaim old libui font shared-memory objects when text buffers grow.
- [ ] Make `libui`, font-service, and compositor allocation/index arithmetic
  overflow-safe: validate dimensions before multiplication, compute buffer
  indexes in `usize`, and cap growth before capacity doubling.
- [ ] Reproduce and fix the deferred rapid-TTY-switch framebuffer prompt
  duplication/misalignment issue; keep the issue deferred until a stable repro
  exists.
- [ ] Expand VT behavior only from an explicit compatibility need: richer ANSI,
  UTF-8, scrollback, or input APIs should each include focused behavioral tests.
- [ ] Add script-engine diagnostics for unclosed `if` blocks and preserve the
  documented `script` versus `source` environment-scope semantics.

## Validation and Documentation

Source: `architecture/25-diagnostics-status.md`,
`architecture/26-repo-map-and-validation.md`, and
`architecture/27-python-test-framework.md`.

- [ ] Add behavioral regression coverage with every new subsystem contract;
  reject source-text assertions.
- [ ] Add focused stress/negative tests for TLB shootdown, service restart,
  futures cancellation, virtio-serial transport, networking, and new DMA paths.
- [ ] Add graphical input-injection tests only after virtio-serial transport is
  usable; use distinct sockets and never run QEMU sessions in parallel.
- [ ] Keep architecture documents authoritative for design, `STATUS.md` concise
  for current behavior, and this file limited to unfinished work.
