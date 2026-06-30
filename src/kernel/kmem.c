/* kmem.c - Small shared allocator for kernel container metadata.
 * malloc-backed, with a fixed early-boot arena fallback for allocations that
 * happen before the heap allocator is fully available.  Shared by the generic
 * containers (list.c, hashmap.c). */
#include "kmem.h"
#include "stdlib.h"

/* Early-boot fallback arena, used when malloc is not yet available. */
#define KMEM_EARLY_ARENA_BYTES (256u * 1024u)
static uint8_t g_kmem_early_arena[KMEM_EARLY_ARENA_BYTES];
static uint32_t g_kmem_early_arena_off;

static void *
kmem_early_alloc(size_t size)
{
    uintptr_t base = (uintptr_t)&g_kmem_early_arena[0];
    if (size == 0) {
        return 0;
    }
    for (;;) {
        uint32_t observed = g_kmem_early_arena_off;
        uintptr_t cursor = base + (uintptr_t)observed;
        uintptr_t aligned = (cursor + 7u) & ~(uintptr_t)0x7u;
        uintptr_t end = aligned + (uintptr_t)size;
        uint32_t next = 0;
        if (end < aligned || end > (base + (uintptr_t)KMEM_EARLY_ARENA_BYTES)) {
            return 0;
        }
        next = (uint32_t)(end - base);
        if (__sync_bool_compare_and_swap(&g_kmem_early_arena_off, observed, next)) {
            return (void *)aligned;
        }
    }
}

void *
kmem_alloc(size_t size)
{
    void *ptr = 0;
    if (size == 0) {
        return 0;
    }
    ptr = malloc(size);
    if (ptr) {
        return ptr;
    }
    return kmem_early_alloc(size);
}

void
kmem_free(void *ptr)
{
    uintptr_t start = (uintptr_t)&g_kmem_early_arena[0];
    uintptr_t end = start + (uintptr_t)KMEM_EARLY_ARENA_BYTES;
    uintptr_t p = (uintptr_t)ptr;
    if (!ptr) {
        return;
    }
    if (p >= start && p < end) {
        /* TODO(kmem): add reclaim for early-arena allocations if repeated
         * early remove/destroy paths become hot. */
        return;
    }
    free(ptr);
}
