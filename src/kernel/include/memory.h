/* memory.h - Virtual memory management: per-process address spaces and shared regions.
 *
 * Each process owns an mm_context_t that groups a PML4 root table with a list of
 * typed virtual memory regions.  Physical frames come from the physmem free-list
 * allocator (physmem.h) and are mapped by paging.c.
 *
 * Shared regions allow two processes to see the same physical pages at different
 * virtual addresses — used for DMA buffers and the framebuffer. */
#ifndef WASMOS_MEMORY_H
#define WASMOS_MEMORY_H

#include <stdint.h>
#include "boot.h"
#include "list.h"

/* TODO: legacy guard constant, enforced nowhere -- the context list grows on
 * demand and has no cap (docs/architecture/06-memory-management.md). Either
 * enforce it in mm_context_create or delete it. */
#define MM_MAX_CONTEXTS 128

/* Physical address boundary between the shmem zone and the WARP linear-memory zone.
 * Shmem pages are allocated below this limit (pfa_alloc_pages_below).
 * WARP linear memory pages are allocated at or above this limit (pfa_alloc_pages_above).
 * This prevents WARP's ensureLinearSize zero-fill from aliasing active shmem pages
 * via the kernel direct map (phys | KERNEL_HIGHER_HALF_BASE). */
#define WASMOS_SHMEM_PHYS_LIMIT (64ULL * 1024ULL * 1024ULL) /* 64 MiB */

/* Semantic purpose of a mapped virtual region; controls page-table flags at fault time. */
typedef enum {
    MEM_REGION_WASM_LINEAR = 0, /* WASM linear memory (both runtime backends) */
    MEM_REGION_IPC,             /* IPC message buffers */
    MEM_REGION_DEVICE,          /* MMIO device memory (not cached) */
    MEM_REGION_STACK,           /* ring-3 or kernel thread stack */
    MEM_REGION_HEAP,            /* general kernel/user heap */
    MEM_REGION_CODE,            /* executable code segments */
    MEM_REGION_SHARED           /* shared region mapped from another context */
} mem_region_type_t;

/* Region permission bits.  The same mask is what paging_map_4k_in_root consumes as its
 * `flags` argument, so these — not raw x86 PTE bits — are the currency of every mapping
 * call in this tree.  WRITE sets the PTE writable bit, EXEC clears NX, and USER sets the
 * user bit; the combination USER|WRITE|EXEC is refused (W^X) by both
 * mm_context_add_region and paging_map_4k_in_root. */
#define MEM_REGION_FLAG_READ (1u << 0)
#define MEM_REGION_FLAG_WRITE (1u << 1)
#define MEM_REGION_FLAG_EXEC (1u << 2)
/* Region is intended to be user-accessible once ring3 mappings are active. */
#define MEM_REGION_FLAG_USER (1u << 3)
/* Physical backing is owned by another subsystem and must not be freed by
 * mm_context_destroy(). Set by mm_context_rebind_wasm_linear once the
 * placeholder region has been rebound to a runtime-owned allocation. */
#define MEM_REGION_FLAG_PHYS_EXTERNAL (1u << 31)

/* IPC message types for the kernel memory-fault service.  A request packs the faulting
 * address into arg0 (low 32 bits) and arg1 (high 32 bits), the x86 page-fault error code
 * into arg2 and the faulting context id into arg3; the reply carries the handler status
 * in arg0 and the mapped page base split across arg1/arg2 the same way. */
typedef enum { IPC_MEM_FAULT = 0x1000, IPC_MEM_FAULT_REPLY = 0x1001 } memory_ipc_type_t;

/* A contiguous virtual address range with associated type and permissions.
 * phys_base is the backing physical frame (only valid for non-demand-paged regions). */
typedef struct {
    uint64_t base;      /* first virtual address of the region, in the owning context */
    uint64_t phys_base; /* physical base of the contiguous backing; 0 = no backing recorded */
    uint64_t size;      /* virtual extent in bytes; grows for WASM_LINEAR as the guest grows */
    /* Owned physical backing size (pages) freed at teardown.  Usually size/PAGE,
     * but decoupled for WASM_LINEAR under the slot model: the region's VA `size`
     * grows with the guest's linear memory, while the freeable backing stays the
     * original placeholder (the real backing is the linmem slot, freed by its
     * own owner).  0 = nothing to free here. */
    uint64_t backing_pages;
    uint32_t flags; /* MEM_REGION_FLAG_* mask */
    mem_region_type_t type;
    uint32_t shared_id; /* valid only when type == MEM_REGION_SHARED */
} mem_region_t;

/* Per-process memory context.  root_table is the physical address of the PML4.
 * next_shared_base is the bump pointer for shared-region VA assignments. */
typedef struct {
    uint32_t id;               /* context id; 0 is the kernel/root context */
    uint64_t root_table;       /* physical address of this context's PML4; 0 = not built yet */
    uint64_t next_shared_base; /* next user VA handed out to a MEM_REGION_SHARED mapping */
    uint32_t region_count;     /* number of entries in `regions` */
    list_t regions;            /* mem_region_t list, owned by the context */
} mm_context_t;

/* Initialize the memory manager from UEFI memory map in boot_info.  Brings up the PFA
 * and paging, then builds context 0 (the kernel/root context) with a default set of
 * supervisor regions.  Retains `boot_info` by pointer for the lifetime of the kernel,
 * so the caller must keep that structure alive.  Failures are logged, not reported. */
void mm_init(const boot_info_t* boot_info);

/* Initialize an already-allocated mm_context_t with a fresh PML4.  Zeroes *ctx, sets the
 * id, and creates an empty region list; root_table is left 0, so the caller must supply
 * one (mm_context_create does this via paging_create_address_space).  Returns 0 on
 * success, -1 on a NULL ctx or a failed region-list allocation. */
int mm_context_init(mm_context_t* ctx, uint32_t id);

/* Record a fixed virtual region [base, base+size) in ctx without touching page tables:
 * the pages are wired lazily by mm_handle_page_fault or eagerly by an explicit map call.
 * phys_base is left 0, so a region added this way has no backing until a caller fills it
 * in.  `size` is in bytes and is stored verbatim (no page rounding).  Returns 0 on
 * success, -1 on a NULL ctx, a W^X-violating flag set, or a full region list. */
int mm_context_add_region(mm_context_t* ctx, uint64_t base, uint64_t size, uint32_t flags,
                          mem_region_type_t type);

/* Allocate `pages` contiguous 4 KiB physical frames and record them as a region of the
 * given type at that type's fixed user VA (MEM_REGION_SHARED bumps next_shared_base
 * instead).  The region owns the frames: backing_pages is set to `pages` and
 * mm_context_destroy frees exactly that many.  MEM_REGION_CODE has no VA assignment and
 * is always rejected.  Returns 0 on success, -1 on a NULL ctx, pages == 0, exhausted
 * physical memory, an unassignable type, or a full region list; the frames are released
 * again on the later failures.  Adding a second region of the same non-shared type gets
 * the same VA as the first, and address lookups keep resolving to the earlier one. */
int mm_context_alloc_region(mm_context_t* ctx, uint64_t pages, uint32_t flags,
                            mem_region_type_t type);

/* Copy the first region of the given type in ctx into *out_region.  Returns 0 when one
 * was found, -1 on a NULL argument or no match.  The copy is a snapshot: later growth or
 * rebinding of the region is not reflected in it. */
int mm_context_region_for_type(mm_context_t* ctx, mem_region_type_t type, mem_region_t* out_region);

/* Copy the region at position `index` (insertion order, 0-based) of ctx's region list
 * into *out_region.  Returns 0 on success, -1 on a NULL argument or an index past the
 * end — which is how callers enumerate a context's regions. */
int mm_context_region_at(mm_context_t* ctx, uint32_t index, mem_region_t* out_region);

/* Service a #PF for context_id at user VA `addr` with the raw x86 page-fault error code.
 * Wires the containing 4 KiB page to phys_base + (page VA - region base) in that
 * context's root, and reports the page-aligned VA in *out_mapped_base (optional).
 * Returns 0 when the page was mapped and -1 otherwise: unknown context, a
 * protection-violation fault (present bit set — this handler only fills in absent
 * pages), an address in no region, a user fault on a supervisor region, or an access the
 * region's flags do not permit.  A -1 means the fault is fatal to the faulting thread. */
int mm_handle_page_fault(uint32_t context_id, uint64_t addr, uint64_t error_code,
                         uint64_t* out_mapped_base);

/* Look up an existing context by id; returns NULL if not found.  Id 0 resolves to the
 * kernel root context.  The pointer is borrowed: it belongs to the manager's context
 * list and is invalidated by mm_context_destroy for that id.  Takes the context lock
 * internally but returns the pointer unlocked, so concurrent mutation of the returned
 * context is the caller's problem. */
mm_context_t* mm_context_get(uint32_t id);

/* Allocate a context with the given id, give it a private address space, and populate
 * the default user regions: 16 frames of WASM linear memory (64 KiB, matching a wasm3
 * module's initial memory), 2 frames of stack and 4 frames of heap, all
 * READ|WRITE|USER.  Verifies the resulting root before publishing it.  Returns the
 * borrowed context pointer, or NULL when the id is already in use, when any allocation
 * fails, or when root verification fails — all partial state is unwound in those cases.
 * Passing the root context's id (0) returns the existing root context unchanged. */
mm_context_t* mm_context_create(uint32_t id);

/* Tear down context `id`: free the physical backing each region owns (skipping
 * PHYS_EXTERNAL regions and dropping the shared-region references), free the page-table
 * structures, and remove the context from the manager.  Returns 0 on success, -1 for id
 * 0 / the root context, which cannot be destroyed, or for an unknown id.  Every pointer
 * previously returned by mm_context_get for this id is dangling afterwards. */
int mm_context_destroy(uint32_t id);

/* Load context id's PML4 into CR3 (switches the active address space).  Returns 0 on
 * success, -1 when the context is unknown or has no root table.  Takes effect on the
 * calling CPU only, and only until the scheduler next switches this CPU. */
int mm_context_activate(uint32_t id);

/* Return the physical address of context id's PML4, or 0 when the context is unknown or
 * has not been given an address space yet. */
uint64_t mm_context_root_table(uint32_t id);

/* Create a shared anonymous region of `pages` 4 KiB frames owned by owner_context_id.
 * Writes the new region id to *out_id and the region's PHYSICAL base address to
 * *out_base — not a virtual address; mapping it into a context is mm_shared_map's job.
 * Under the WARP backend the frames are taken from below WASMOS_SHMEM_PHYS_LIMIT so
 * WARP's own allocations cannot alias them through the kernel direct map.  The region
 * starts at refcount 0 and is destroyed by the first mm_shared_release/unmap that drives
 * the count back to 0, so a creator that intends to hold it must mm_shared_retain.
 * Returns 0 on success, -1 on a NULL out pointer, pages == 0, exhausted physical memory,
 * an exhausted id space, or a full region table. */
int mm_shared_create(uint32_t owner_context_id, uint64_t pages, uint32_t flags, uint32_t* out_id,
                     uint64_t* out_base);

/* Map shared region `id` into ctx at the next shared VA and report that VA in *out_base
 * (optional).  Requires ctx to be the owner, a grantee, or context 0.  `flags` is
 * intersected with the region's creation flags when non-zero; 0 means "the region's own
 * flags".  Takes a reference on the region and pins the frames, so the mapping survives
 * the owner releasing its own reference.  Returns 0 on success, -1 when the region does
 * not exist, access is not permitted, the refcount would overflow, or the region slot
 * could not be added. */
int mm_shared_map(mm_context_t* ctx, uint32_t id, uint32_t flags, uint64_t* out_base);

/* Remove ctx's mapping of shared region `id` and drop the reference mm_shared_map took;
 * the frames are returned to the PFA once the last reference goes.  Returns 0 on
 * success, -1 when the region does not exist, access is not permitted, ctx has no
 * matching region entry, or the refcount is already 0.  Does not tear down the page-table
 * entries — the region record is dropped, and the pin pfa_pin_pages took at map time is
 * released by mm_context_release_regions at teardown, not here. */
int mm_shared_unmap(mm_context_t* ctx, uint32_t id);

/* Allow/revoke target_context_id access to a shared region owned by owner_context_id.
 * owner_context_id 0 acts as the supervisor and bypasses the ownership check.  Both
 * return 0 on success — including the no-op cases of granting the owner itself, a
 * duplicate grant, and revoking a context that holds no grant — and -1 when the region
 * does not exist, the target id is 0, the caller does not own the region, or (grant
 * only) the fixed grant table is full.  Revoking does not unmap an existing mapping. */
int mm_shared_grant(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id);
int mm_shared_revoke(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id);

/* Report the physical base address and 4 KiB page count of shared region `id` in
 * *out_base / *out_pages.  Returns 0 on success, -1 on a NULL out pointer, an unknown
 * region, or a caller that is neither owner, grantee, nor context 0. */
int mm_shared_get_phys(uint32_t owner_context_id, uint32_t id, uint64_t* out_base,
                       uint64_t* out_pages);

/* Take one reference on shared region `id`, keeping its frames alive across other
 * holders' releases.  Returns 0 on success, -1 on an unknown region, a caller without
 * access, or a saturated refcount. */
int mm_shared_retain(uint32_t owner_context_id, uint32_t id);

/* Drop one reference on shared region `id`; at zero the frames are freed and the region
 * record is removed, invalidating the id.  Returns 0 on success, -1 on an unknown
 * region, a caller without access, or a refcount that is already 0. */
int mm_shared_release(uint32_t owner_context_id, uint32_t id);

/* Map an arbitrary physical range into a context's virtual space (MMIO use).  `virt`,
 * `phys` and `size` must all be 4 KiB-aligned and non-zero, and [virt, virt+size) must
 * lie inside the context's WASM_LINEAR region — this call overlays device pages onto the
 * guest's linear memory rather than creating a new region, so no region record and no
 * ownership of the frames is created and mm_context_destroy will not unmap them.  Any
 * existing mapping of those pages is replaced.  Returns 0 on success, -1 on a
 * misaligned/zero argument, a W^X-violating flag set, an unknown context, a missing or
 * too-small WASM_LINEAR region, or a failed mapping — in which case the pages mapped
 * before the failure stay mapped. */
int mm_context_map_physical(uint32_t context_id, uint64_t virt, uint64_t phys, uint64_t size,
                            uint32_t flags);
/* Point the context's WASM_LINEAR region at a contiguous runtime-owned backing.
 * Unmaps the old range, frees the previous backing unless it was already
 * external, and marks the region MEM_REGION_FLAG_PHYS_EXTERNAL so teardown
 * leaves the new backing to its owner. Returns 0, or -1 if the context or the
 * region does not exist. */
int mm_context_rebind_wasm_linear(uint32_t context_id, uint64_t phys_base, uint64_t size);
/* Bind the WASM_LINEAR user-region page range [from_page, to_page) to the
 * physical frames backing the linmem slot at slot_va_base (region page P maps
 * the frame under slot_va_base + P*PAGE, header page 0 included).  from_page/
 * to_page let a grow bind only the freshly committed tail so pre-existing
 * overlays (shmem / framebuffer / DMA / net ring) mapped into lower pages are
 * never clobbered.  Grows region->size to cover to_page. */
int mm_context_bind_wasm_linear_scattered(uint32_t context_id, uint64_t slot_va_base,
                                          uint64_t from_page, uint64_t to_page);

/* VA base of the per-process WASM linear-memory execution window (PML4[1]).
 * This is the user-VA view the interpreter reads under the unified linmem
 * model; it is dual-mapped with the linmem slot's kernel alias. */
uint64_t mm_user_wasm_linear_base(void);

/* Safe user-memory copy helpers — validate the user VA range before touching it.
 * `user_src`/`user_dst` are virtual addresses in context_id's address space; `dst`/`src`
 * are kernel pointers.  Each verifies the range against the context's regions and wires
 * any not-yet-mapped page, then copies in 256-byte chunks through a stack bounce buffer,
 * flipping CR3 to the user root and back for each chunk while running on a per-CPU
 * copy stack (kernel stacks are not mapped under a user root).  Return 0 on success and
 * -1 on a zero/NULL argument, an unknown context, or a range the regions do not permit;
 * nothing is copied in those cases, but a failure partway through a multi-chunk copy
 * leaves the earlier chunks written.  Both kpanic if the kernel root cannot be restored
 * after a chunk, since returning would fault on every kernel address.  Not safe from an
 * interrupt handler: the CR3 flip is not re-entrant. */
int mm_copy_from_user(uint32_t context_id, void* dst, uint64_t user_src, uint64_t size);
int mm_copy_to_user(uint32_t context_id, uint64_t user_dst, const void* src, uint64_t size);

/* Returns 0 when [user_addr, user_addr+size) is covered by regions of context_id that
 * carry MEM_REGION_FLAG_USER and at least needed_flags, and -1 otherwise — including for
 * a zero context_id, a zero user_addr or size, an unknown context, and a range that
 * wraps.  This is a region-permission check, not a page-table check: a permitted range
 * may still be unmapped and fault on first touch.  Note the polarity: 0 means PERMITTED, so the
 * guard is `if (mm_user_range_permitted(...) != 0) { reject; }`.  Writing the
 * natural-reading `if (mm_user_range_permitted(...))` inverts the check and
 * admits exactly the accesses it was meant to refuse. */
int mm_user_range_permitted(uint32_t context_id, uint64_t user_addr, uint64_t size,
                            uint32_t needed_flags);

#endif
