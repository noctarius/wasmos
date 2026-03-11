#ifndef WASMOS_WAMR_CONTEXT_H
#define WASMOS_WAMR_CONTEXT_H

#include <stdint.h>
#include "memory.h"

typedef struct {
    mm_context_t *mm;
    uint64_t linear_base;
    uint64_t linear_size;
    uint32_t stack_size;
    uint32_t heap_size;
} wamr_context_t;

int wamr_context_init(void);
int wamr_context_bind(mm_context_t *ctx, wamr_context_t *out_ctx);
void wamr_runtime_heap_range(uintptr_t *out_base, uint32_t *out_size);

#endif
