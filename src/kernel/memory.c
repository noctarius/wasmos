#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "serial.h"

#define PAGE_SIZE 0x1000ULL
static const uint64_t pf_err_present = 1ULL << 0;
static const uint64_t pf_err_write = 1ULL << 1;
static const uint64_t pf_err_instr = 1ULL << 4;
static const boot_info_t *g_boot_info;
static mm_context_t g_contexts[MM_MAX_CONTEXTS];
static uint32_t g_context_count;
static mm_context_t g_root_ctx;

#define MM_MAX_SHARED 16
typedef struct {
    uint32_t id;
    uint8_t in_use;
    uint32_t refcount;
    uint64_t base;
    uint64_t pages;
    uint32_t flags;
} mm_shared_region_t;

static mm_shared_region_t g_shared[MM_MAX_SHARED];
static uint32_t g_shared_next_id = 1;

void mm_init(const boot_info_t *boot_info) {
    g_boot_info = boot_info;
    g_context_count = 0;
    serial_write("[mm] init\n");
    pfa_init(boot_info);
    if (paging_init() != 0) {
        serial_write("[mm] paging init failed\n");
    } else {
        serial_write("[mm] paging init\n");
    }

    if (mm_context_init(&g_root_ctx, 0) == 0) {
        mm_context_alloc_region(&g_root_ctx, 16, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_WASM_LINEAR);
        mm_context_alloc_region(&g_root_ctx, 4, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_STACK);
        mm_context_alloc_region(&g_root_ctx, 8, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_HEAP);
        mm_context_alloc_region(&g_root_ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_IPC);
        mm_context_alloc_region(&g_root_ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_DEVICE);
    }

    serial_write("[mm] ctx0 regions=");
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    uint64_t value = g_root_ctx.region_count;
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);

}

int mm_context_init(mm_context_t *ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    ctx->id = id;
    ctx->region_count = 0;
    for (uint32_t i = 0; i < MM_MAX_REGIONS; ++i) {
        ctx->regions[i].base = 0;
        ctx->regions[i].size = 0;
        ctx->regions[i].flags = 0;
        ctx->regions[i].type = MEM_REGION_WASM_LINEAR;
    }
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

int mm_context_region_for_type(mm_context_t *ctx, mem_region_type_t type, mem_region_t *out_region) {
    if (!ctx || !out_region) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        if (ctx->regions[i].type == type) {
            *out_region = ctx->regions[i];
            return 0;
        }
    }
    return -1;
}

static mm_shared_region_t *mm_shared_find(uint32_t id) {
    if (id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < MM_MAX_SHARED; ++i) {
        if (g_shared[i].in_use && g_shared[i].id == id) {
            return &g_shared[i];
        }
    }
    return 0;
}

static mem_region_t *mm_find_region_for_addr(mm_context_t *ctx, uint64_t addr) {
    if (!ctx) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        uint64_t end = region->base + region->size;
        if (addr >= region->base && addr < end) {
            return region;
        }
    }
    return 0;
}

int mm_handle_page_fault(uint32_t context_id, uint64_t addr, uint64_t error_code, uint64_t *out_mapped_base) {
    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }

    if (error_code & pf_err_present) {
        return -1;
    }

    mem_region_t *region = mm_find_region_for_addr(ctx, addr);
    if (!region) {
        return -1;
    }

    if ((error_code & pf_err_write) && !(region->flags & MEM_REGION_FLAG_WRITE)) {
        return -1;
    }
    if ((error_code & pf_err_instr) && !(region->flags & MEM_REGION_FLAG_EXEC)) {
        return -1;
    }

    uint64_t page_base = addr & ~(PAGE_SIZE - 1ULL);
    int rc = paging_map_4k(page_base, page_base, region->flags);
    if (rc < 0) {
        return -1;
    }

    if (out_mapped_base) {
        *out_mapped_base = page_base;
    }
    return 0;
}

int mm_shared_create(uint64_t pages, uint32_t flags, uint32_t *out_id, uint64_t *out_base) {
    if (!out_id || !out_base || pages == 0) {
        return -1;
    }
    uint32_t slot = MM_MAX_SHARED;
    for (uint32_t i = 0; i < MM_MAX_SHARED; ++i) {
        if (!g_shared[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == MM_MAX_SHARED) {
        return -1;
    }
    uint64_t base = pfa_alloc_pages(pages);
    if (!base) {
        return -1;
    }
    uint32_t id = g_shared_next_id++;
    if (id == 0) {
        id = g_shared_next_id++;
    }
    g_shared[slot].id = id;
    g_shared[slot].in_use = 1;
    g_shared[slot].refcount = 0;
    g_shared[slot].base = base;
    g_shared[slot].pages = pages;
    g_shared[slot].flags = flags;
    *out_id = id;
    *out_base = base;
    return 0;
}

int mm_shared_map(mm_context_t *ctx, uint32_t id, uint32_t flags, uint64_t *out_base) {
    if (!ctx) {
        return -1;
    }
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region) {
        return -1;
    }
    uint32_t effective_flags = region->flags;
    if (flags) {
        effective_flags &= flags;
    }
    if (mm_context_add_region(ctx, region->base, region->pages * PAGE_SIZE,
                              effective_flags, MEM_REGION_SHARED) != 0) {
        return -1;
    }
    region->refcount++;
    if (out_base) {
        *out_base = region->base;
    }
    return 0;
}

int mm_shared_unmap(mm_context_t *ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region) {
        return -1;
    }

    uint32_t found = 0;
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *r = &ctx->regions[i];
        if (r->type == MEM_REGION_SHARED && r->base == region->base &&
            r->size == region->pages * PAGE_SIZE) {
            for (uint32_t j = i; j + 1 < ctx->region_count; ++j) {
                ctx->regions[j] = ctx->regions[j + 1];
            }
            ctx->region_count--;
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    if (region->refcount > 0) {
        region->refcount--;
    }
    if (region->refcount == 0) {
        pfa_free_pages(region->base, region->pages);
        region->in_use = 0;
        region->id = 0;
        region->base = 0;
        region->pages = 0;
        region->flags = 0;
    }
    return 0;
}

int mm_context_alloc_region(mm_context_t *ctx, uint64_t pages, uint32_t flags, mem_region_type_t type) {
    if (!ctx || pages == 0) {
        return -1;
    }
    uint64_t base = pfa_alloc_pages(pages);
    if (!base) {
        return -1;
    }
    return mm_context_add_region(ctx, base, pages * PAGE_SIZE, flags, type);
}

mm_context_t *mm_context_get(uint32_t id) {
    if (id == g_root_ctx.id) {
        return &g_root_ctx;
    }
    for (uint32_t i = 0; i < g_context_count; ++i) {
        if (g_contexts[i].id == id) {
            return &g_contexts[i];
        }
    }
    return 0;
}

mm_context_t *mm_context_create(uint32_t id) {
    if (id == g_root_ctx.id) {
        return &g_root_ctx;
    }
    if (g_context_count >= MM_MAX_CONTEXTS) {
        return 0;
    }
    mm_context_t *ctx = &g_contexts[g_context_count];
    if (mm_context_init(ctx, id) != 0) {
        return 0;
    }
    if (mm_context_alloc_region(ctx, 8, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_WASM_LINEAR) != 0) {
        return 0;
    }
    if (mm_context_alloc_region(ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_STACK) != 0) {
        return 0;
    }
    if (mm_context_alloc_region(ctx, 4, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_HEAP) != 0) {
        return 0;
    }
    g_context_count++;
    return ctx;
}
