## DMA Transfer Support

### Goal
Add a minimal, policy-safe DMA transfer model that fits the current WASMOS
microkernel split:
- kernel owns DMA mechanism, mapping/pinning, and enforcement
- `device-manager` owns driver policy and capability assignment
- drivers use explicit DMA operations on top of existing buffer-borrow handles
  rather than ambient physical access

### Scope and Non-Goals
In scope:
- capability-gated DMA mapping, synchronization, and release for borrowed
  buffers
- per-driver DMA window enforcement at kernel boundary
- deterministic cleanup on process exit/restart
- one production path integration (storage-first baseline)

Out of scope for initial rollout:
- full IOMMU enablement (VT-d/AMD-Vi)
- scatter-gather list acceleration beyond a minimal descriptor vector
- zero-copy userland DMA for generic apps

### Current Baseline
- Capability name `dma.buffer` already exists and is enforced on relevant
  hostcalls.
- Shared-memory and copy paths exist but are not a complete DMA lifecycle
  contract.
- Kernel already exposes generic cross-context buffer borrowing
  (`buffer_borrow`/`buffer_release`) with ownership and grant semantics.
- Driver capability profiles currently cover PIO range and IRQ masks; DMA
  windows are planned but not yet fully implemented.

### Design Principles
- Deny by default: no DMA operation succeeds without explicit capability grant.
- Least privilege: grants are limited by direction, size, and approved physical
  windows.
- Deterministic behavior: each DMA mapping has explicit owner, state, and
  teardown path.
- Reuse existing kernel object lifecycles: DMA must build on borrow-buffer
  objects instead of introducing parallel region allocators.
- New API rule: any new kernel API must include explicit insufficiency rationale
  for existing APIs, ownership invariants, failure semantics, and migration
  plan.

### Technical Model

#### Capability Contract
Each DMA-capable driver receives a capability descriptor:
- `cap.dma.windows[]`: allowed physical ranges (`base`, `length`)
- `cap.dma.max_bytes`: maximum bytes per mapped transfer
- `cap.dma.flags`: direction policy (`to_device`, `from_device`, `bidirectional`)

Kernel checks this contract on every DMA API entry.

#### Kernel DMA Object Model (Borrow-Buffer Based)
DMA state is attached to an existing borrowed buffer object; no separate
`dma_region` allocator is introduced.
- primary key: `borrow_id` (existing opaque handle)
- owner: `owner_pid` / `owner_context` (existing borrow ownership)
- dma metadata:
  - `dma_mapped` flag
  - `dma_direction`
  - `dma_device_binding` (device/instance id)
  - `dma_window` (resolved allowed window)
  - `dma_backend_addr` (phase-dependent bus address)

DMA metadata lifecycle is tied to borrow lifecycle and removed when the borrow
is released or owner exits.

#### DMA API Surface (Kernel Internal + Hostcall/Syscall Binding)
- `dma_map_borrow(borrow_id, device_id, offset, length, direction) -> device_addr`
- `dma_sync_borrow(borrow_id, offset, length, op)` where `op` in:
  - `DMA_SYNC_TO_DEVICE`
  - `DMA_SYNC_FROM_DEVICE`
  - `DMA_SYNC_BIDIR`
- `dma_unmap_borrow(borrow_id, device_id)`

Constraints:
- `dma_map_borrow` requires owner check, borrow grant check, and capability
  window check.
- `dma_sync_borrow` is mandatory before and after hardware ownership
  transitions.
- `buffer_release` on mapped/active DMA borrow fails in-process; process-exit
  cleanup force-unmaps before release.

#### Phase-1 Mapping Strategy (No IOMMU)
- Use the existing borrowed-buffer backing and expose DMA only for buffer kinds
  explicitly approved for DMA-capable drivers.
- Enforce approved physical windows at map time.
- Track bus-master exposure strictly by borrow ownership and capability checks.

TODO: Replace/augment bounce-backed mapping with IOMMU-backed IOVA domains once
IOMMU support is introduced.

#### Interrupt and Completion Interactions
- DMA completion remains device/driver specific and delivered by existing IRQ
  ownership routing.
- Kernel DMA layer is transport-agnostic; it only validates borrow-attached DMA
  state transitions and synchronization calls.

#### Failure and Recovery Semantics
- On driver crash/kill:
  - auto-unmap all mapped borrows
  - revoke active mappings before process object free
  - release remaining borrowed buffers per existing cleanup path
- On invalid operation:
  - return deterministic deny/error code
  - emit structured kernel marker with `pid`, `borrow_id`, and reason

### Security and Isolation
- DMA APIs require `dma.buffer` capability and a matching `cap.dma` descriptor.
- Cross-process borrow-handle use is denied.
- Oversized, out-of-window, or direction-mismatched requests are denied.
- Driver restart does not inherit old borrows; a new process gets new handles
  only.

### Rollout Plan

Phase 0: Contract and ABI definition
- Tasks:
  - define `cap.dma` descriptor schema for spawn-time capability profiles
  - define DMA IPC/hostcall IDs and status/error codes
  - document borrow-attached DMA ownership/state machine in kernel and
    user-space headers
  - document explicit rationale: why DMA reuses borrow API and why no parallel
    allocator API is introduced
- Done gate:
  - schema + ABI headers merged and referenced by `device-manager` + kernel

Phase 1: Kernel borrow-DMA manager
- Tasks:
  - add DMA metadata/state transitions to existing borrow records
  - enforce owner/context checks and `cap.dma` window checks
  - implement process-exit cleanup path for mapped DMA borrows
- Done gate:
  - kernel can map/sync/unmap borrowed buffers in a self-contained DMA test path

Phase 2: Driver capability plumbing
- Tasks:
  - extend PM spawn profile payload to carry `cap.dma` windows/limits/flags
  - wire `device-manager` policy to attach DMA grants per matched driver
  - fail closed when DMA request is present but profile is missing/invalid
- Done gate:
  - spawned DMA driver receives only declared windows and denied-default holds

Phase 3: Storage-path integration (implemented)
- Tasks:
  - integrate one driver path to use borrow-based DMA APIs (first target:
    storage stack)
  - add transition from CPU copy path to DMA path with deterministic fallback
  - add capability-aware startup checks and explicit logs
- Done gate:
  - selected storage path performs DMA-backed transfer from borrowed buffers in
    QEMU smoke
Implementation note:
- ATA block read/write now attempts a borrow-based DMA lifecycle
  (`buffer_borrow` + `dma_map_borrow` + `dma_sync_borrow` +
  `dma_unmap_borrow`) before the transfer path and emits one-shot
  `[ata] dma ... active|fallback` markers for observability.
- If DMA policy denies/unavailable/range checks fail, ATA falls back
  deterministically to the existing PIO/copy data path so bootstrap remains
  stable.
- Native framebuffer borrow path now also validates borrow-attached DMA wiring
  through kernel native-driver plumbing: framebuffer buffer borrows attempt
  DMA map/sync/unmap and emit one-shot `[framebuffer] dma path active` or
  `[framebuffer] dma fallback active` markers.

Phase 4: Validation and hardening
- Tasks:
  - add negative tests: missing capability, bad owner, out-of-window, oversize
  - add lifecycle tests: crash/restart cleanup and stale borrow-handle deny
  - add stress tests: repeated map/unmap/release and IRQ-completion churn
- Done gate:
  - `run-qemu-test` remains green and DMA-specific deny/cleanup markers pass

Phase 5: IOMMU readiness boundary
- Tasks:
  - introduce abstraction split between current borrow backing and IOVA backend
  - define per-device DMA domain model without enabling full VT-d yet
  - document migration hooks and compatibility constraints
- Done gate:
  - code and docs support backend swap without changing driver-facing
    borrow-based DMA ABI

### Task Checklist (Execution Order)
1. Add DMA metadata/state transitions to borrow records.
2. Add capability parser support for `cap.dma` profile fields.
3. Add hostcall/syscall glue and libc/native headers.
4. Integrate one driver path and keep PIO/copy fallback behind explicit branch.
5. Add validation matrix markers and deny-path coverage.
6. Update architecture/status docs and boot-time diagnostics markers.

### Validation Matrix
- Baseline regression:
  - `cmake --build build --target run-qemu-test`
- DMA capability deny path:
  - driver without `dma.buffer` fails `dma_map_borrow`
- DMA window policy:
  - out-of-window mapping denied
- Lifecycle correctness:
  - crash/restart does not retain old `borrow_id` DMA validity
- Stress:
  - repeated map/unmap/release under IRQ activity shows no leaked DMA mappings

### Observability Markers (Planned)
- `[dma] map_borrow ok|deny`
- `[dma] map ok|deny`
- `[dma] sync ok|deny`
- `[dma] cleanup exit ok`

### Risks and Mitigations
- Risk: weak isolation without IOMMU.
  - Mitigation: strict capability windows + bounce-backed controlled buffers.
- Risk: driver complexity increase.
  - Mitigation: small fixed ABI + reference integration in one driver first.
- Risk: stale mapping leaks during failures.
  - Mitigation: mandatory teardown in process exit path + lifecycle tests.
