#include "memory.h"
#include "physmem.h"
#include "serial.h"

static const boot_info_t *g_boot_info;
static mm_context_t g_contexts[MM_MAX_CONTEXTS];
static uint32_t g_context_count;

void mm_init(const boot_info_t *boot_info) {
    g_boot_info = boot_info;
    g_context_count = 0;
    serial_write("[mm] init\n");
    pfa_init(boot_info);
    (void)g_boot_info;
}

int mm_context_init(mm_context_t *ctx, uint32_t id) {
    if (!ctx || g_context_count >= MM_MAX_CONTEXTS) {
        return -1;
    }
    mm_context_t *slot = &g_contexts[g_context_count];
    slot->id = id;
    slot->region_count = 0;
    for (uint32_t i = 0; i < MM_MAX_REGIONS; ++i) {
        slot->regions[i].base = 0;
        slot->regions[i].size = 0;
        slot->regions[i].flags = 0;
        slot->regions[i].type = MEM_REGION_WASM_LINEAR;
    }
    g_context_count++;
    return 0;
}

int mm_context_add_region(mm_context_t *ctx, uint64_t base, uint64_t size, uint32_t flags, mem_region_type_t type) {
    if (!ctx || ctx->region_count >= MM_MAX_REGIONS) {
        return -1;
    }
    mem_region_t *region = &ctx->regions[ctx->region_count++];
    region->base = base;
    region->size = size;
    region->flags = flags;
    region->type = type;
    return 0;
}
