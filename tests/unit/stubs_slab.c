/* stubs_slab.c - Host-side stand-in for the kernel slab allocator (slab.h).
 *
 * kmem.c allocates container metadata from kalloc_small/kfree_small; the host
 * test build has no physical frame allocator to back a slab, so these forward
 * to the host libc heap. Two differences from the real allocator that the
 * suites depend on: any size succeeds (the slab caps requests at its largest
 * size class), and allocation only fails when the host is out of memory, so
 * kmem's early-arena fallback is never reached in a test. */
#include "slab.h"

#include <stdlib.h>

void slab_init(void) {}

void* kalloc_small(size_t size) {
    return malloc(size);
}

void kfree_small(void* ptr) {
    free(ptr);
}