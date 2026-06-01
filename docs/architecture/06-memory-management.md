## Memory Management

This document covers the WASMOS kernel memory subsystem in full implementation
detail: the physical frame allocator, the paging layer, memory contexts,
the user virtual address space layout, shared memory, user-pointer copy,
and the slab allocator. The authoritative sources are `src/kernel/physmem.c`,
`src/kernel/paging.c`, `src/kernel/memory.c`, `src/kernel/slab.c`, and
their headers.

---

### Physical Frame Allocator

The PFA (`src/kernel/physmem.c`) manages physical pages. It is initialized
from the UEFI memory map immediately after the kernel takes over from the
bootloader.

#### Free-Range List

Available physical memory is tracked as a sorted array of up to 128
`pfa_range_t { base, pages }` entries. Adjacent ranges are coalesced on
insert and free. Physical address 0 is excluded (`add_range` skips page 0
if `base == 0`).

Usable EFI memory types accepted:
- `EFI_MEMORY_TYPE_CONVENTIONAL (7)`
- `EFI_MEMORY_TYPE_BOOT_SERVICES_CODE (3)`
- `EFI_MEMORY_TYPE_BOOT_SERVICES_DATA (4)`

The kernel image is carved out of the free list via `reserve_range(__kernel_start,
__kernel_end - __kernel_start)` after the EFI map is scanned.

#### Reference Count Array

A byte-per-page refcount array tracks allocation state:

- **Bootstrap:** `g_refcount_static[16384]` in BSS covers the first 64 MB
  (16384 × 4KB = 64 MB, consuming 16 KB of BSS).
- **Upgrade:** `pfa_upgrade_refcount()` allocates a dynamic array sized to
  `max_physical_address / 4096` pages from the just-initialized free list,
  copies the static data, and switches `g_refcount` to the dynamic array.
  Halts on upgrade failure.

Maximum refcount per page: 255. `pfa_pin_pages` halts on overflow.

#### Allocation and Free

| Function                             | Behavior                                                                   |
|--------------------------------------|----------------------------------------------------------------------------|
| `pfa_alloc_pages(n)`                 | First-fit from range list; advances `range->base`                          |
| `pfa_alloc_pages_below(n, max_addr)` | First-fit below `max_addr` ceiling                                         |
| `pfa_free_pages(base, n)`            | Decrements refcount; pages reaching 0 are re-inserted sorted and coalesced |
| `pfa_pin_pages(base, n)`             | Increments refcount (used for shared-memory pinning)                       |

**Double-free detection:** `pfa_free_pages` halts via `PFA_BUG` if any page
has refcount 0 at free time. `pfa_pin_pages` halts on pin of a free page.

Both `pfa_alloc_pages` and `pfa_alloc_pages_below` return 0 on failure.
The caller must check before use; no fallback or retry is performed.

---

### Paging Layer

`src/kernel/paging.c` manages x86_64 4-level page tables and CR3 switching.

#### Address Space Layout

```
PML4 index  Virtual range                            Use
──────────  ────────────────────────────────--──     ───────────────────────
1           0x0000008000000000 – 0x0000BFFFFF        User process regions
511         0xFFFFFFFF80000000 – 0xFFFFFFFF9FFFFFFF  Kernel higher-half (512 MB)
```

`KERNEL_HIGHER_HALF_BASE = 0xFFFFFFFF80000000` is the kernel's self-reference
base. All kernel virtual addresses are in this range; the first 512 MB of
physical memory is mapped here.

Higher-half mapping uses 2 MB large pages (`PT_FLAG_LARGE_PAGE`):
- `HIGHER_HALF_PD_COUNT = 1` page directory
- `HIGHER_HALF_PDE_COUNT = 256` entries → 512 MB window
- `HIGHER_HALF_PDPT_INDEX = 510` within PML4[511]

`USER_PML4_INDEX = 1` is the single user-space PML4 slot. All user virtual
regions live under this entry.

**`KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES = 512 MB`** is the single
authoritative constant governing which physical addresses are reachable in
the kernel's shared mapping. Stack and page-table allocations must use
`pfa_alloc_pages_below(n, 512MB)` to remain within this window.

#### Page Table Flags

| Flag                 | Bit | Purpose                     |
|----------------------|-----|-----------------------------|
| `PT_FLAG_PRESENT`    | 0   | Page is mapped              |
| `PT_FLAG_WRITE`      | 1   | Writable                    |
| `PT_FLAG_USER`       | 2   | CPL3-accessible             |
| `PT_FLAG_LARGE_PAGE` | 7   | 2 MB large page (PDE level) |
| `PT_FLAG_NX`         | 63  | No-execute                  |

#### Per-Process CR3

`paging_create_address_space(out_root)` allocates a new PML4 page
(`pfa_alloc_pages_below(1, 512MB)` — must be within the shared window).

`paging_clone_low_slot_in_root(root)` copies PML4[511] (the kernel
higher-half entry) from the current root into the new root, giving every
process the same kernel mappings. PML4[0] (bootstrap identity map) is also
cloned initially for early kernel-mode processes.

`paging_strip_low_slot_in_root(root)` removes PML4[0] before a process
enters CPL3. This is part of the ring3 hardening path.

`paging_verify_user_root(root, log)` / `paging_verify_user_root_no_low_slot`
enforce allowed PML4 slot constraints: only slots 0 (kernel bootstrap,
pre-strip), 1 (user), and 511 (kernel higher-half) are permitted. Any other
populated slot is a verification failure.

#### W^X Enforcement

`mm_region_flags_valid` rejects any region with `USER | WRITE | EXEC`
simultaneously. This is checked at `mm_context_add_region_slot` time, so
no writable-executable user mapping can be registered.

---

### Memory Contexts

`src/kernel/memory.c` wraps the PFA and paging layers into per-process
memory contexts.

#### `mm_context_t`

```c
typedef struct {
    uint32_t id;              // context identity (== pid at creation)
    uint64_t root_table;      // physical address of PML4
    uint64_t next_shared_base; // advance pointer for MEM_REGION_SHARED mappings
    uint32_t region_count;
    list_t   regions;         // list of mem_region_t
} mm_context_t;
```

The global context list (`g_contexts`) grows in chunks of 16. The root
context (`g_root_ctx`, id=0) is static and not in the dynamic list.
`MM_MAX_CONTEXTS = 128` is a legacy guard constant; the list itself has no
hard cap.

#### Context Lifecycle

`mm_context_create(id)` allocates a fresh address space and seeds it with
three regions:

| Region type              | Pages | Flags    |
|--------------------------|-------|----------|
| `MEM_REGION_WASM_LINEAR` | 8     | R+W+User |
| `MEM_REGION_STACK`       | 2     | R+W+User |
| `MEM_REGION_HEAP`        | 4     | R+W+User |

After region creation, `paging_verify_user_root` is called. Creation fails
and all resources are released if verification fails.

`mm_context_destroy(id)` releases all region physical pages (unpin shared
regions; free owned regions), destroys the address space, and removes the
context from the list.

---

### User Virtual Address Space Layout

The following fixed bases are defined in `memory.c`:

| Symbol                | Address              | Region type                           |
|-----------------------|----------------------|---------------------------------------|
| `MM_USER_LINEAR_BASE` | `0x0000008000000000` | `MEM_REGION_WASM_LINEAR`              |
| `MM_USER_STACK_BASE`  | `0x0000008100000000` | `MEM_REGION_STACK`                    |
| `MM_USER_HEAP_BASE`   | `0x0000008200000000` | `MEM_REGION_HEAP`                     |
| `MM_USER_IPC_BASE`    | `0x0000008300000000` | `MEM_REGION_IPC`                      |
| `MM_USER_DEVICE_BASE` | `0x0000008400000000` | `MEM_REGION_DEVICE`                   |
| `MM_USER_SHARED_BASE` | `0x0000008500000000` | `MEM_REGION_SHARED` (advance pointer) |

Each type gets exactly one base address within the canonical user range under
PML4[1]. `MEM_REGION_SHARED` uses `ctx->next_shared_base` as an advancing
pointer, allocating each new shared mapping at the next available address
without reuse.

#### `mem_region_t`

```c
typedef struct {
    uint64_t base;       // virtual start address
    uint64_t phys_base;  // physical backing start (0 if not yet faulted in)
    uint64_t size;       // bytes
    uint32_t flags;      // MEM_REGION_FLAG_{READ,WRITE,EXEC,USER}
    mem_region_type_t type;
    uint32_t shared_id;  // valid only for MEM_REGION_SHARED
} mem_region_t;
```

#### Demand Mapping and Page Fault Handling

`mm_handle_page_fault(context_id, addr, error_code, out_mapped_base)`:

1. Rejects `pf_err_present` faults (protection violation, not absence).
2. Finds the containing `mem_region_t` for `addr`.
3. Checks user/write/instr bits against region flags.
4. Computes `phys_page = region->phys_base + (page_base − region->base)`.
5. Calls `paging_map_4k_in_root(ctx->root_table, page_base, phys_page, flags)`.

This is a demand-mapping model: physical pages are allocated at region
creation (`mm_context_alloc_region`) but not mapped into the page table
until a fault fires. The fault handler wires the specific page on demand.

---

### Shared Memory

`mm_shared_region_t` in `memory.c` is the kernel-managed shared memory object:

```c
typedef struct {
    uint32_t id;
    uint32_t owner_context_id;
    uint32_t refcount;
    uint64_t base;                      // physical base
    uint64_t pages;
    uint32_t flags;
    uint32_t grant_contexts[MM_MAX_SHARED_GRANTS]; // 8 grantees
    uint8_t  grant_count;
} mm_shared_region_t;
```

Global limits: `MM_MAX_SHARED = 16` regions; `MM_MAX_SHARED_GRANTS = 8`
grantees per region. The shared list is a `list_t` initialized to a starting
chunk of 16.

#### Lifecycle

| Function                                                  | Effect                                                                              |
|-----------------------------------------------------------|-------------------------------------------------------------------------------------|
| `mm_shared_create(owner, pages, flags, out_id, out_base)` | Allocates physical pages with `pfa_alloc_pages`; initial refcount=0                 |
| `mm_shared_grant(owner, id, target)`                      | Adds `target` to grantee list; owner-restricted                                     |
| `mm_shared_revoke(owner, id, target)`                     | Removes `target`; owner-restricted                                                  |
| `mm_shared_map(ctx, id, flags, out_base)`                 | Increments refcount; pins pages; advances `ctx->next_shared_base`; registers region |
| `mm_shared_unmap(ctx, id)`                                | Removes region from context; releases pin; decrements refcount                      |
| `mm_shared_retain(owner, id)`                             | Increments refcount (caller must have access)                                       |
| `mm_shared_release(owner, id)`                            | Decrements refcount; frees physical pages if refcount reaches 0                     |
| `mm_shared_get_phys(owner, id, out_base, out_pages)`      | Returns physical address of a shared region                                         |

Context 0 (kernel) bypasses the owner/grantee access check and can operate on
any region. All other callers must be the owner or an explicit grantee.

`mm_shared_map` calls `pfa_pin_pages` to increment the refcount on the
physical pages, preventing `pfa_free_pages` from returning them to the free
pool while they remain mapped. `mm_shared_unmap` calls `pfa_free_pages` to
release the pin before decrementing the shared region's logical refcount.

---

### User-Pointer Copy

`mm_copy_from_user` and `mm_copy_to_user` are the only safe paths for reading
or writing user-space memory from kernel code. They perform a CR3-switch-
around-copy using a 256-byte bounce buffer to keep kernel accesses under
kernel CR3 at all times.

#### Copy Protocol

1. Validate arguments (non-null, non-zero context, non-zero addresses).
2. `mm_ensure_user_range_mapped`: walks the range page by page, finding each
   region and calling `paging_map_4k_in_root` on any unmapped pages. This
   demand-maps the full range before the CR3 switch so the copy never takes
   a page fault mid-transfer.
3. If current RSP is below `KERNEL_HIGHER_HALF_BASE` (bootstrap path), execute
   on the dedicated 8 KB `g_mm_copy_stack` to avoid aliasing issues.
4. Per-chunk loop (256 bytes per iteration):
   - `mm_copy_from_user`: `switch_root(user_root)` → `memcpy(bounce, user_src, n)` → `switch_root(prev_root)` → `memcpy(dst, bounce, n)`
   - `mm_copy_to_user`: `memcpy(bounce, src, n)` → `switch_root(user_root)` → `memcpy(user_dst, bounce, n)` → `switch_root(prev_root)`
5. If `switch_root(prev_root)` fails after a user CR3 was loaded, the copy
   halts (`for(;;){}`) because it cannot safely return with the CPU still
   under the user page table.

`mm_user_range_permitted(context_id, addr, size, needed_flags)` is a pure
permission check (no mapping) used by syscall entry paths that need to
validate a user pointer before acting on it.

`mm_context_map_physical(context_id, virt, phys, size, flags)` maps a
physical device memory range (e.g., GOP framebuffer) into a context's
`MEM_REGION_WASM_LINEAR` window. Validates alignment (4KB), size, and that
the virtual range fits within the existing linear region.

---

### Slab Allocator

`src/kernel/slab.c` provides a minimal fixed-size slab for small kernel
objects. It is optional infrastructure alongside the main static-table
allocation patterns.

**Three size classes:**

| Class | Object size | Slots | BSS usage |
|-------|-------------|-------|-----------|
| 0     | 32 B        | 128   | 4 KB      |
| 1     | 64 B        | 128   | 8 KB      |
| 2     | 128 B       | 96    | 12 KB     |

Total BSS: ~24 KB.

Each allocation prepends a 4-byte `slab_header_t` (magic `0x51AB` + class
index). `kalloc_small(size)` selects the smallest class where
`size + sizeof(slab_header_t) ≤ chunk_size`. Returns null if the class free
list is empty or if no class fits.

`kfree_small(ptr)` validates the magic and class index before pushing the
chunk back to its free list. Invalid magic or out-of-range class index is
silently dropped.

---

### Known Constraints

**Stack and page-table allocations compete with device DMA for low physical
memory.** Both use `pfa_alloc_pages_below(n, 512MB)` because the kernel's
shared higher-half window only covers the first 512 MB. If a machine has
256 MB of RAM, high stack-spawn rates can exhaust low physical pages even
when global memory is not full. The failure appears as a stack allocation
error, not an out-of-memory error.

**No intent-based allocation API exists yet.** All callers embed the
physical-address ceiling directly. Adding or changing the constraint requires
finding all call sites.

**`mm_context_create` always allocates three fixed regions.** A WASM process
that never uses its stack or heap base still consumes physical pages for them
at context creation time. Region size is not currently configurable from
WASMOS-APP metadata.

**`MM_MAX_SHARED = 16` limits shared regions globally.** The compositor and
graphics stack allocate one shared buffer per window. Under high window
counts this cap may be reached. The list implementation can grow beyond 16
but the `mm_shared_create` search loop iterates at most `MM_MAX_SHARED`
times before giving up.

---

### Migration Plan

The migration toward a fully intent-based, DMA-decoupled allocator is
tracked in three phases:

#### Phase 1 — Allocation-Class API

Introduce typed intent: `MM_ALLOC_STACK`, `MM_ALLOC_PGTABLE`,
`MM_ALLOC_DMA32`, `MM_ALLOC_GENERIC`. Route existing
`pfa_alloc_pages_below` callers through intent APIs. Initial behavior is
identical; the benefit is a single location where allocation policy is
changed rather than scattered literals.

Deliverable: no `pfa_alloc_pages_below` call sites with embedded policy
literals; all policy lives in the intent dispatch table.

#### Phase 2 — Decouple Kernel-Internal Allocations from DMA32

Migrate kernel stacks and page-table page allocations off the
`below-512MB` constraint. Only paths with a genuine hardware DMA
requirement (virtio queues, DMA descriptors) remain constrained to low
addresses. Everything else uses `pfa_alloc_pages` (unconstrained).

Deliverable: high-spawn workloads do not fail due to low-zone exhaustion.

#### Phase 3 — Kernel Virtual Reachability Upgrade

Either:
- **Path A (physmap)**: extend the kernel higher-half to a direct map of
  all RAM. Every physical address has a stable kernel virtual alias.
  Eliminates the 512 MB window ceiling entirely.
- **Path B (kmap-on-demand)**: implement a bounded `kmap` cache that maps
  arbitrary physical pages temporarily into a kernel window, unmapping
  them after use. Lower memory footprint than physmap but requires callers
  to hold kmap windows across operations.

Decision deferred until Phase 2 is complete and actual allocation pressure
beyond 512 MB is observed.

Deliverable: kernel can operate on arbitrary physical pages without any
`below-X` ceiling.

---

### Open Design Decisions

- **physmap vs. kmap-on-demand** as the kernel virtual reachability
  strategy for Phase 3.
- **Shared region limit** (`MM_MAX_SHARED`): fixed cap vs. dynamic list with
  no global cap.
- **Region sizing at context create**: whether WASMOS-APP metadata should
  control initial region pages rather than the fixed defaults (8/2/4).
- **RSS tracking**: `process_stats_t` currently returns `vm_total_bytes` as
  the RSS estimate. Real per-page presence tracking requires per-context page
  walk or dirty-bit accounting, deferred until the paging model stabilizes.
