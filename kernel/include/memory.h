#ifndef WASMOS_MEMORY_H
#define WASMOS_MEMORY_H

#include <stdint.h>
#include "boot.h"

#define MM_MAX_CONTEXTS 16
#define MM_MAX_REGIONS 8

typedef enum {
    MEM_REGION_WASM_LINEAR = 0,
    MEM_REGION_IPC,
    MEM_REGION_DEVICE,
    MEM_REGION_STACK,
    MEM_REGION_HEAP,
    MEM_REGION_CODE
} mem_region_type_t;

#define MEM_REGION_FLAG_READ   (1u << 0)
#define MEM_REGION_FLAG_WRITE  (1u << 1)
#define MEM_REGION_FLAG_EXEC   (1u << 2)

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    mem_region_type_t type;
} mem_region_t;

typedef struct {
    uint32_t id;
    uint32_t region_count;
    mem_region_t regions[MM_MAX_REGIONS];
} mm_context_t;

void mm_init(const boot_info_t *boot_info);
int mm_context_init(mm_context_t *ctx, uint32_t id);
int mm_context_add_region(mm_context_t *ctx, uint64_t base, uint64_t size, uint32_t flags, mem_region_type_t type);
int mm_context_alloc_region(mm_context_t *ctx, uint64_t pages, uint32_t flags, mem_region_type_t type);

#endif
