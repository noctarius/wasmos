#ifndef WASMOS_MEMORY_H
#define WASMOS_MEMORY_H

#include <stdint.h>
#include "boot.h"

#define MM_MAX_CONTEXTS 32
#define MM_MAX_REGIONS 8

typedef enum {
    MEM_REGION_WASM_LINEAR = 0,
    MEM_REGION_IPC,
    MEM_REGION_DEVICE,
    MEM_REGION_STACK,
    MEM_REGION_HEAP,
    MEM_REGION_CODE,
    MEM_REGION_SHARED
} mem_region_type_t;

#define MEM_REGION_FLAG_READ   (1u << 0)
#define MEM_REGION_FLAG_WRITE  (1u << 1)
#define MEM_REGION_FLAG_EXEC   (1u << 2)
/* Region is intended to be user-accessible once ring3 mappings are active. */
#define MEM_REGION_FLAG_USER   (1u << 3)

typedef enum {
    IPC_MEM_FAULT = 0x1000,
    IPC_MEM_FAULT_REPLY = 0x1001
} memory_ipc_type_t;

typedef struct {
    uint64_t base;
    uint64_t phys_base;
    uint64_t size;
    uint32_t flags;
    mem_region_type_t type;
} mem_region_t;

typedef struct {
    uint32_t id;
    uint64_t root_table;
    uint64_t next_shared_base;
    uint32_t region_count;
    mem_region_t regions[MM_MAX_REGIONS];
} mm_context_t;

void mm_init(const boot_info_t *boot_info);
int mm_context_init(mm_context_t *ctx, uint32_t id);
int mm_context_add_region(mm_context_t *ctx, uint64_t base, uint64_t size, uint32_t flags, mem_region_type_t type);
int mm_context_alloc_region(mm_context_t *ctx, uint64_t pages, uint32_t flags, mem_region_type_t type);
int mm_context_region_for_type(mm_context_t *ctx, mem_region_type_t type, mem_region_t *out_region);
int mm_handle_page_fault(uint32_t context_id, uint64_t addr, uint64_t error_code, uint64_t *out_mapped_base);
mm_context_t *mm_context_get(uint32_t id);
mm_context_t *mm_context_create(uint32_t id);
int mm_context_destroy(uint32_t id);
int mm_context_activate(uint32_t id);
uint64_t mm_context_root_table(uint32_t id);
int mm_shared_create(uint32_t owner_context_id, uint64_t pages, uint32_t flags,
                     uint32_t *out_id, uint64_t *out_base);
int mm_shared_map(mm_context_t *ctx, uint32_t id, uint32_t flags, uint64_t *out_base);
int mm_shared_unmap(mm_context_t *ctx, uint32_t id);
int mm_shared_get_phys(uint32_t owner_context_id, uint32_t id, uint64_t *out_base, uint64_t *out_pages);
int mm_shared_retain(uint32_t owner_context_id, uint32_t id);
int mm_shared_release(uint32_t owner_context_id, uint32_t id);
int mm_context_map_physical(uint32_t context_id, uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);
int mm_copy_from_user(uint32_t context_id, void *dst, uint64_t user_src, uint64_t size);
int mm_copy_to_user(uint32_t context_id, uint64_t user_dst, const void *src, uint64_t size);
int mm_user_range_permitted(uint32_t context_id, uint64_t user_addr, uint64_t size, uint32_t needed_flags);

#endif
