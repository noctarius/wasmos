#include "memory.h"
#include "serial.h"

static const boot_info_t *g_boot_info;
static mm_context_t g_contexts[MM_MAX_CONTEXTS];
static uint32_t g_context_count;

void mm_init(const boot_info_t *boot_info) {
    g_boot_info = boot_info;
    g_context_count = 0;
    serial_write("[mm] init\n");
    if (g_boot_info) {
        serial_write("[mm] mmap size=");
        char buf[21];
        // Print size as hex without pulling in printf.
        uint64_t value = g_boot_info->memory_map_size;
        static const char hex[] = "0123456789ABCDEF";
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
    if (!ctx || g_context_count >= MM_MAX_CONTEXTS) {
        return -1;
    }
    ctx->id = id;
    ctx->region_count = 0;
    // Avoid libc memcpy in freestanding build.
    g_contexts[g_context_count] = *ctx;
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
