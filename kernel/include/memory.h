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

#endif
