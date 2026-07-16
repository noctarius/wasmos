/* stubs_slab.c - Host-side stubs for the kernel slab allocator.
 *
 * kmem.c calls kalloc_small/kfree_small (the global kernel heap).  On the host
 * unit-test build there is no slab, so back them with the host libc malloc/free
 * — this matches kmem's pre-slab behaviour and keeps allocations unbounded for
 * tests. */
#include "slab.h"

#include <stdlib.h>

void slab_init(void) {}

void* kalloc_small(size_t size) {
    return malloc(size);
}

void kfree_small(void* ptr) {
    free(ptr);
}