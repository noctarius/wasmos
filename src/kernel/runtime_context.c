/* runtime_context.c - WASM runtime memory-region helpers.
 * Resolves the LINEAR_MEMORY, STACK and HEAP regions of an mm_context_t into a
 * single runtime_context_t, the shape a host-call handler would need to bounds-
 * check a guest linear-memory pointer.  Nothing calls it yet; see the TODO in
 * runtime_context.h. */
#include "runtime_context.h"

static int find_region(const mm_context_t* ctx, mem_region_type_t type, mem_region_t* out) {
    if (!ctx || !out) {
        return 0;
    }
    return mm_context_region_for_type((mm_context_t*)ctx, type, out) == 0 ? 1 : 0;
}

/* Fills *out_ctx from ctx's region table.  All three of WASM_LINEAR, STACK and
 * HEAP must be present, so it fails on a context that has not finished its
 * standard region set-up.  Sizes are snapshots taken at bind time and do not
 * track a later region grow; stack_size and heap_size are narrowed to 32 bits.
 *
 * out_ctx->mm keeps ctx as a BORROWED pointer, so the bound structure is only
 * valid while the context lives.  Returns 0 on success, -1 for a NULL argument
 * or a missing region. */
int runtime_context_bind(mm_context_t* ctx, runtime_context_t* out_ctx) {
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
