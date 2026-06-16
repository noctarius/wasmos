/* memory.h - Virtual memory management: per-process address spaces and shared regions.
 *
 * Each process owns an mm_context_t that groups a PML4 root table with a list of
 * typed virtual memory regions.  Physical frames are allocated from the physmem
 * bitmap allocator and mapped by paging.c.
 *
 * Shared regions allow two processes to see the same physical pages at different
 * virtual addresses — used for DMA buffers and the framebuffer. */
#ifndef WASMOS_MEMORY_H
#define WASMOS_MEMORY_H

#include <stdint.h>
#include "boot.h"
#include "list.h"

#define MM_MAX_CONTEXTS 128  /* hard cap on simultaneous process memory contexts */

/* Physical address boundary between the shmem zone and the WARP linear-memory zone.
 * Shmem pages are allocated below this limit (pfa_alloc_pages_below).
 * WARP linear memory pages are allocated at or above this limit (pfa_alloc_pages_above).
 * This prevents WARP's ensureLinearSize zero-fill from aliasing active shmem pages
 * via the kernel direct map (phys | KERNEL_HIGHER_HALF_BASE). */
#define WASMOS_SHMEM_PHYS_LIMIT (64ULL * 1024ULL * 1024ULL)  /* 64 MiB */

/* Semantic purpose of a mapped virtual region; controls page-table flags at fault time. */
typedef enum {
    MEM_REGION_WASM_LINEAR = 0, /* wasm3 linear memory heap */
    MEM_REGION_IPC,             /* IPC message buffers */
    MEM_REGION_DEVICE,          /* MMIO device memory (not cached) */
    MEM_REGION_STACK,           /* ring-3 or kernel thread stack */
    MEM_REGION_HEAP,            /* general kernel/user heap */
    MEM_REGION_CODE,            /* executable code segments */
    MEM_REGION_SHARED           /* shared region mapped from another context */
} mem_region_type_t;

#define MEM_REGION_FLAG_READ   (1u << 0)
#define MEM_REGION_FLAG_WRITE  (1u << 1)
#define MEM_REGION_FLAG_EXEC   (1u << 2)
/* Region is intended to be user-accessible once ring3 mappings are active. */
#define MEM_REGION_FLAG_USER   (1u << 3)

/* IPC message types for the kernel memory-fault service. */
typedef enum {
    IPC_MEM_FAULT = 0x1000,
    IPC_MEM_FAULT_REPLY = 0x1001
} memory_ipc_type_t;

/* A contiguous virtual address range with associated type and permissions.
 * phys_base is the backing physical frame (only valid for non-demand-paged regions). */
typedef struct {
    uint64_t base;
    uint64_t phys_base;
    uint64_t size;
    uint32_t flags;
    mem_region_type_t type;
    uint32_t shared_id;  /* valid only when type == MEM_REGION_SHARED */
} mem_region_t;

/* Per-process memory context.  root_table is the physical address of the PML4.
 * next_shared_base is the bump pointer for shared-region VA assignments. */
typedef struct {
    uint32_t id;
    uint64_t root_table;
    uint64_t next_shared_base;
    uint32_t region_count;
    list_t regions;
} mm_context_t;

/* Initialize the memory manager from UEFI memory map in boot_info. */
void mm_init(const boot_info_t *boot_info);

/* Initialize an already-allocated mm_context_t with a fresh PML4. */
int mm_context_init(mm_context_t *ctx, uint32_t id);

/* Record a fixed virtual region in ctx (does not touch page tables yet). */
int mm_context_add_region(mm_context_t *ctx, uint64_t base, uint64_t size, uint32_t flags, mem_region_type_t type);

/* Allocate pages physical frames and map them into ctx at the next available VA. */
int mm_context_alloc_region(mm_context_t *ctx, uint64_t pages, uint32_t flags, mem_region_type_t type);

/* Look up the first region of the given type in ctx. */
int mm_context_region_for_type(mm_context_t *ctx, mem_region_type_t type, mem_region_t *out_region);

/* Return the region at position index in ctx's region list. */
int mm_context_region_at(mm_context_t *ctx, uint32_t index, mem_region_t *out_region);

/* Handle a page fault for context_id at addr.  Maps the faulting page and
 * returns the region base in *out_mapped_base, or returns an error code. */
int mm_handle_page_fault(uint32_t context_id, uint64_t addr, uint64_t error_code, uint64_t *out_mapped_base);

/* Look up an existing context by id; returns NULL if not found. */
mm_context_t *mm_context_get(uint32_t id);

/* Allocate and initialize a new mm_context_t with the given id. */
mm_context_t *mm_context_create(uint32_t id);

int mm_context_destroy(uint32_t id);

/* Load context id's PML4 into CR3 (switches the active address space). */
int mm_context_activate(uint32_t id);

/* Return the physical address of context id's PML4. */
uint64_t mm_context_root_table(uint32_t id);

/* Create a shared anonymous region of pages frames; returns its id and base VA in the owner. */
int mm_shared_create(uint32_t owner_context_id, uint64_t pages, uint32_t flags,
                     uint32_t *out_id, uint64_t *out_base);

/* Map a previously granted shared region into ctx; returns the VA in *out_base. */
int mm_shared_map(mm_context_t *ctx, uint32_t id, uint32_t flags, uint64_t *out_base);
int mm_shared_unmap(mm_context_t *ctx, uint32_t id);

/* Allow/revoke target_context_id access to a shared region owned by owner_context_id. */
int mm_shared_grant(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id);
int mm_shared_revoke(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id);

/* Query the physical address and page count of a shared region. */
int mm_shared_get_phys(uint32_t owner_context_id, uint32_t id, uint64_t *out_base, uint64_t *out_pages);
int mm_shared_retain(uint32_t owner_context_id, uint32_t id);
int mm_shared_release(uint32_t owner_context_id, uint32_t id);

/* Map an arbitrary physical range into a context's virtual space (MMIO use). */
int mm_context_map_physical(uint32_t context_id, uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);

/* Safe user-memory copy helpers — validate the user VA range before touching it. */
int mm_copy_from_user(uint32_t context_id, void *dst, uint64_t user_src, uint64_t size);
int mm_copy_to_user(uint32_t context_id, uint64_t user_dst, const void *src, uint64_t size);

/* Return non-zero if [user_addr, user_addr+size) is mapped with at least needed_flags. */
int mm_user_range_permitted(uint32_t context_id, uint64_t user_addr, uint64_t size, uint32_t needed_flags);

#endif
