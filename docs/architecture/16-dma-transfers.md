## DMA Transfer Support

### Goal
Add a minimal, policy-safe DMA transfer model that fits the current WASMOS
microkernel split:
- kernel owns DMA mechanism, mapping/pinning, and enforcement
- `device-manager` owns driver policy and capability assignment
- drivers use explicit DMA buffer APIs rather than ambient physical access

### Scope and Non-Goals
In scope:
- capability-gated DMA buffer allocation, mapping, synchronization, and release
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
- Driver capability profiles currently cover PIO range and IRQ masks; DMA
  windows are planned but not yet fully implemented.

### Design Principles
- Deny by default: no DMA operation succeeds without explicit capability grant.
- Least privilege: grants are limited by direction, size, and approved physical
  windows.
- Deterministic behavior: each DMA mapping has explicit owner, state, and
  teardown path.
- Minimal kernel ABI: small set of primitives that can back both WASM and
  native drivers.

### Technical Model

#### Capability Contract
Each DMA-capable driver receives a capability descriptor:
- `cap.dma.windows[]`: allowed physical ranges (`base`, `length`)
- `cap.dma.max_bytes`: maximum bytes per transfer/map
- `cap.dma.flags`: direction policy (`to_device`, `from_device`, `bidirectional`)

Kernel checks this contract on every DMA API entry.

#### Kernel DMA Objects
Introduce a kernel-owned `dma_region` object:
- `region_id` (opaque handle)
- `owner_pid` / `owner_context`
- `length`
- `iova_or_phys` (phase-dependent; initially physical/bounce-backed)
- `direction`
- `state` (`allocated`, `mapped`, `active`, `released`)
- `pin_count`

The object lives in a per-process registry and is removed on process teardown.

#### DMA API Surface (Kernel Internal + Hostcall/Syscall Binding)
- `dma_alloc(length, align, direction) -> region_id`
- `dma_map(region_id, device_id, offset, length) -> device_addr`
- `dma_sync(region_id, offset, length, op)` where `op` in:
  - `DMA_SYNC_TO_DEVICE`
  - `DMA_SYNC_FROM_DEVICE`
  - `DMA_SYNC_BIDIR`
- `dma_unmap(region_id, device_id)`
- `dma_free(region_id)`

Constraints:
- `dma_map` requires owner check and capability window check.
- `dma_sync` is mandatory before and after hardware ownership transitions.
- `dma_free` on mapped/active region fails unless explicitly force-cleaned
  during process exit.

#### Phase-1 Mapping Strategy (No IOMMU)
- Use bounce-backed physically contiguous kernel buffers for deterministic
  behavior.
- Enforce approved physical windows at allocation/map time.
- Track bus-master exposure strictly by region ownership and capability checks.

TODO: Replace/augment bounce-backed mapping with IOMMU-backed IOVA domains once
IOMMU support is introduced.

#### Interrupt and Completion Interactions
- DMA completion remains device/driver specific and delivered by existing IRQ
  ownership routing.
- Kernel DMA layer is transport-agnostic; it only validates region state
  transitions and synchronization calls.

#### Failure and Recovery Semantics
- On driver crash/kill:
  - auto-unmap all mapped regions
  - revoke active mappings before process object free
  - free remaining allocated regions
- On invalid operation:
  - return deterministic deny/error code
  - emit structured kernel marker with `pid`, `region_id`, and reason

### Security and Isolation
- DMA APIs require `dma.buffer` capability and a matching `cap.dma` descriptor.
- Cross-process DMA handle use is denied.
- Oversized, out-of-window, or direction-mismatched requests are denied.
- Driver restart does not inherit old DMA regions; a new process gets new
  handles only.

### Rollout Plan

Phase 0: Contract and ABI definition
- Tasks:
  - define `cap.dma` descriptor schema for spawn-time capability profiles
  - define DMA IPC/hostcall IDs and status/error codes
  - document ownership/state machine in kernel and user-space headers
- Done gate:
  - schema + ABI headers merged and referenced by `device-manager` + kernel

Phase 1: Kernel region manager
- Tasks:
  - add `dma_region` tables and lifecycle helpers in kernel memory subsystem
  - enforce owner/context checks and `cap.dma` window checks
  - implement process-exit cleanup path for DMA region registry
- Done gate:
  - kernel can allocate/map/sync/unmap/free in a self-contained DMA test path

Phase 2: Driver capability plumbing
- Tasks:
  - extend PM spawn profile payload to carry `cap.dma` windows/limits/flags
  - wire `device-manager` policy to attach DMA grants per matched driver
  - fail closed when DMA request is present but profile is missing/invalid
- Done gate:
  - spawned DMA driver receives only declared windows and denied-default holds

Phase 3: Storage-path integration
- Tasks:
  - integrate one driver path to use DMA APIs (first target: storage stack)
  - add transition from CPU copy path to DMA path with deterministic fallback
  - add capability-aware startup checks and explicit logs
- Done gate:
  - selected storage path performs DMA-backed transfer in QEMU smoke

Phase 4: Validation and hardening
- Tasks:
  - add negative tests: missing capability, bad owner, out-of-window, oversize
  - add lifecycle tests: crash/restart cleanup and stale handle deny
  - add stress tests: repeated alloc/map/free and IRQ-completion churn
- Done gate:
  - `run-qemu-test` remains green and DMA-specific deny/cleanup markers pass

Phase 5: IOMMU readiness boundary
- Tasks:
  - introduce abstraction split between physical/bounce backend and IOVA backend
  - define per-device DMA domain model without enabling full VT-d yet
  - document migration hooks and compatibility constraints
- Done gate:
  - code and docs support backend swap without changing driver-facing DMA ABI

### Task Checklist (Execution Order)
1. Add kernel DMA region structs + state transitions.
2. Add capability parser support for `cap.dma` profile fields.
3. Add hostcall/syscall glue and libc/native headers.
4. Integrate one driver path and keep PIO/copy fallback behind explicit branch.
5. Add validation matrix markers and deny-path coverage.
6. Update architecture/status docs and boot-time diagnostics markers.

### Validation Matrix
- Baseline regression:
  - `cmake --build build --target run-qemu-test`
- DMA capability deny path:
  - driver without `dma.buffer` fails `dma_alloc`
- DMA window policy:
  - out-of-window mapping denied
- Lifecycle correctness:
  - crash/restart does not retain old `region_id` validity
- Stress:
  - repeated map/unmap under IRQ activity shows no leaked regions

### Observability Markers (Planned)
- `[dma] alloc ok|deny`
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
