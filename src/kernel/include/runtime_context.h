#ifndef WASMOS_RUNTIME_CONTEXT_H
#define WASMOS_RUNTIME_CONTEXT_H

#include <stdint.h>
#include "memory.h"

typedef struct {
    mm_context_t *mm;
    uint64_t linear_base;
    uint64_t linear_size;
    uint32_t stack_size;
    uint32_t heap_size;
} runtime_context_t;

int runtime_context_bind(mm_context_t *ctx, runtime_context_t *out_ctx);

#endif
