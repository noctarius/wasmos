/* runtime_context.h - WASM runtime memory context bound to a process.
 * Captures the wasm3 linear memory region (base + size) and stack/heap limits
 * derived from the process mm_context_t.  One runtime_context_t per WASM process. */
#ifndef WASMOS_RUNTIME_CONTEXT_H
#define WASMOS_RUNTIME_CONTEXT_H

#include <stdint.h>
#include "memory.h"

/* Snapshot of the WASM linear memory layout within a process address space. */
typedef struct {
    mm_context_t *mm;
    uint64_t linear_base;   /* virtual base of the wasm3 linear memory region */
    uint64_t linear_size;   /* total size in bytes */
    uint32_t stack_size;    /* wasm stack portion at the top of linear memory */
    uint32_t heap_size;     /* remaining heap portion */
} runtime_context_t;

/* Populate out_ctx from the MEM_REGION_WASM_LINEAR region in ctx.
 * Returns 0 on success or -1 if no wasm linear region is mapped. */
int runtime_context_bind(mm_context_t *ctx, runtime_context_t *out_ctx);

#endif
