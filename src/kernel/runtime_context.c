#include "runtime_context.h"

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

int runtime_context_bind(mm_context_t *ctx, runtime_context_t *out_ctx) {
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
