## Memory Management

### Current State
Implemented:
- physical frame allocator from the UEFI memory map
- freeing of physical pages
- kernel-owned x86_64 page tables
- higher-half kernel alias mapping at `0xFFFFFFFF80000000`
- root context creation
- per-process context creation
- per-process root page tables cloned from the kernel mappings
- CR3 switching on scheduler dispatch/return
- fault-driven mapping of process-owned virtual regions into a private user slot
- guard pages around process stacks
- stack canaries for overflow diagnostics

### Current Constraints
- Shared-memory primitives are still mostly architectural intent.
- Page faults are handled through a kernel-hosted memory service scaffold rather
  than a real user-space pager.
- All tasks still execute in ring 0, so address-space separation is not yet a
  security boundary.
- Process runtime stacks still rely on shared low kernel mappings rather than a
  dedicated kernel-stack virtual range per process.
- Several allocation paths still depend on hardcoded physical-address ceilings
  (for example `pfa_alloc_pages_below(...)` with a fixed higher-half window),
  which can cause artificial exhaustion even when total free RAM is available.

The native-driver loader maps requested physical device memory (for example, the
GOP framebuffer) into the driver process context at a fixed device virtual base
for direct native access after validation.

### Direction
The desired endpoint is:
- shared kernel higher-half mappings
- per-process user mappings with ring 3 execution
- explicit shared regions for bulk IPC
- user-space memory policy
- user-mode page-fault handling

### Technical Problem Statement (Current)
Recent gfx/compositor bring-up exposed a systemic MM issue:
- multiple subsystems (stacks, paging tables, shm remap support) consume pages
  from the same constrained low physical window
- those constraints are duplicated across files as literals, and can drift
- failures appear as local errors (for example stack allocation failure on
  process spawn) even though global memory is not exhausted

This is a policy/architecture mismatch, not only a capacity problem.

### Target Memory Model
Near-term target for this codebase:
- one authoritative kernel mapping-window definition (no duplicate literals)
- allocation by intent/class instead of ad-hoc "below X" calls
- only DMA-constrained paths should require low physical zones
- stack/page-table/shared-memory metadata allocations should use a general
  kernel allocation path with deterministic kernel virtual reachability

Long-term target:
- either a full direct-map (physmap) of RAM into kernel virtual space, or a
  robust map-on-demand kmap layer for arbitrary physical pages
- zone-aware physical allocator (`DMA32`, `NORMAL`, future `HIGHMEM`)
- ring-3-safe kernel stack model independent of low physical address ceilings

### Migration Plan (Phases, Tasks)
#### Phase 0: Unify Constants and Add Diagnostics
Tasks:
- define one shared source of truth for kernel-shared mapping window size and
  remove duplicated hardcoded values in `paging.c`, `process.c`, and related MM
  callers
- add explicit reason logging for allocation failures (`zone`, `requested
  pages`, `limit`, caller tag)
- add compile-time and boot-time assertions that window assumptions stay
  consistent across paging + scheduler paths

Deliverables:
- no mismatched window constants in MM/scheduler code
- deterministic diagnostics for all `pfa_alloc_pages_below` failures

#### Phase 1: Allocation-Class API
Tasks:
- introduce allocator-intent API (for example `MM_ALLOC_STACK`,
  `MM_ALLOC_PGTABLE`, `MM_ALLOC_DMA32`, `MM_ALLOC_GENERIC`)
- route existing callers through intent APIs instead of direct hardcoded
  address limits
- keep behavior parity initially by mapping each intent to current policy, then
  adjust policies centrally

Deliverables:
- call sites no longer embed policy literals
- MM policy changes happen in one place

#### Phase 2: Decouple Kernel-Internal Allocations from DMA32 Policy
Tasks:
- migrate stacks and page-table allocations off `below-window` constraints
  where not architecturally required
- keep strict low-address constraints only for true device/DMA needs
- add targeted stress tests spawning many short-lived processes/threads and
  repeated shm/gfx workloads

Deliverables:
- repeated spawn/exit workloads do not fail due to low-window depletion
- clearer separation between kernel-internal and device-constrained allocation

#### Phase 3: Kernel Virtual Reachability Upgrade
Tasks (choose one path, keep both documented until decision):
- Path A: expand to direct-map style kernel VA coverage of physical memory
- Path B: implement map-on-demand kernel kmap API with bounded mapping cache
- refactor MM helpers (`mm_context_map_physical`, shmem map paths, page-table
  helpers) to use chosen reachability abstraction

Deliverables:
- kernel can operate on arbitrary physical pages without fragile "below X"
  assumptions

#### Phase 4: Ring-3 and Pager Alignment
Tasks:
- align kernel stack and user fault handling architecture with strict ring-3
- move pager policy boundaries toward user-space service ownership
- keep shared-memory and compositor paths compliant with final capability model

Deliverables:
- memory policy boundaries are explicit and compatible with ring-3 isolation

### Validation and Exit Criteria
- repeated `exec gfx_smoke` runs succeed without stack allocation failures
- process/thread churn tests (high spawn/exit counts) pass without low-zone
  artificial exhaustion
- page-table allocations remain stable under mixed workloads
- DMA-constrained drivers still receive appropriately constrained physical
  allocations

### Open Design Decisions
- direct-map versus kmap-on-demand as the primary long-term kernel reachability
  strategy
- final allocator-zone model for large-RAM systems (for example 128 GiB+)
- exact API surface for intent-based allocation in C/ASM boundary code
