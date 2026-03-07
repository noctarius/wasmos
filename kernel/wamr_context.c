#include "wamr_context.h"
#include "physmem.h"
#include "serial.h"
#include "wamr_runtime.h"

#define WAMR_RUNTIME_HEAP_PAGES 128

static int find_region(const mm_context_t *ctx, mem_region_type_t type, mem_region_t *out) {
    if (!ctx || !out) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        if (ctx->regions[i].type == type) {
            *out = ctx->regions[i];
            return 1;
        }
    }
    return 0;
}

int wamr_context_init(void) {
    uint64_t heap_base = pfa_alloc_pages(WAMR_RUNTIME_HEAP_PAGES);
    if (!heap_base) {
        serial_write("[wamr] runtime heap alloc failed\n");
        return -1;
    }
    uint32_t heap_size = (uint32_t)(WAMR_RUNTIME_HEAP_PAGES * 0x1000ULL);
    if (!wamr_runtime_init_with_pool((void *)(uintptr_t)heap_base, heap_size)) {
        serial_write("[wamr] runtime init failed\n");
        return -1;
    }
    serial_write("[wamr] runtime init\n");
    return 0;
}

int wamr_context_bind(mm_context_t *ctx, wamr_context_t *out_ctx) {
    if (!ctx || !out_ctx) {
        return -1;
    }
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    mem_region_t heap = {0};
    if (!find_region(ctx, MEM_REGION_WASM_LINEAR, &linear)) {
        return -1;
    }
    if (!find_region(ctx, MEM_REGION_STACK, &stack)) {
        return -1;
    }
    if (!find_region(ctx, MEM_REGION_HEAP, &heap)) {
        return -1;
    }
    out_ctx->mm = ctx;
    out_ctx->linear_base = linear.base;
    out_ctx->linear_size = linear.size;
    out_ctx->stack_size = (uint32_t)stack.size;
    out_ctx->heap_size = (uint32_t)heap.size;
    return 0;
}
