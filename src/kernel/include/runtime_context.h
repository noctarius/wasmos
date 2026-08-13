/* runtime_context.h - WASM runtime memory context bound to a process.
 * Flattens three regions of the process mm_context_t -- WASM linear memory,
 * stack, and heap -- into one struct. One runtime_context_t per WASM process.
 *
 * TODO: nothing in the tree calls runtime_context_bind(); either wire the
 * host-call pointer-validation paths through it or drop the module. */
#ifndef WASMOS_RUNTIME_CONTEXT_H
#define WASMOS_RUNTIME_CONTEXT_H

#include <stdint.h>
#include "memory.h"

/* Snapshot of the WASM linear memory layout within a process address space.
 * Sizes are in bytes. The three regions are distinct mappings, not slices of
 * one another: stack_size and heap_size are the sizes of MEM_REGION_STACK and
 * MEM_REGION_HEAP, and neither is carved out of the linear region. */
typedef struct {
    mm_context_t* mm;     /* borrowed, not owned; the caller keeps it alive */
    uint64_t linear_base; /* virtual base of MEM_REGION_WASM_LINEAR */
    uint64_t linear_size;
    uint32_t stack_size; /* MEM_REGION_STACK, truncated to 32 bits */
    uint32_t heap_size;  /* MEM_REGION_HEAP, truncated to 32 bits */
} runtime_context_t;

/* Populate out_ctx from ctx. Returns 0 on success, or -1 if either pointer is
 * NULL or ctx is missing any of MEM_REGION_WASM_LINEAR, MEM_REGION_STACK, or
 * MEM_REGION_HEAP -- all three are required. */
int runtime_context_bind(mm_context_t* ctx, runtime_context_t* out_ctx);

#endif
