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

