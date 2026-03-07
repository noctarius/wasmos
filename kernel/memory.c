#include "memory.h"
#include "physmem.h"
#include "serial.h"

#define PAGE_SIZE 0x1000ULL
static const boot_info_t *g_boot_info;
static mm_context_t g_contexts[MM_MAX_CONTEXTS];
static uint32_t g_context_count;
static mm_context_t g_root_ctx;

void mm_init(const boot_info_t *boot_info) {
    g_boot_info = boot_info;
    g_context_count = 0;
    serial_write("[mm] init\n");
    pfa_init(boot_info);

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

    mm_context_t *ctx1 = mm_context_create(1);
    if (ctx1) {
        serial_write("[mm] ctx1 regions=");
        value = ctx1->region_count;
        buf[0] = '0';
        buf[1] = 'x';
        for (int i = 0; i < 16; ++i) {
            buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
        }
        buf[18] = '\n';
        buf[19] = '\0';
        serial_write(buf);
    }
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
