/* slab.h - Kernel slab allocator for small fixed-size objects.
 * kalloc_small/kfree_small are the general kernel heap interfaces.
 * Backed by a slab cache for sizes up to a few hundred bytes; larger
 * allocations fall through to physmem page allocation. */
#ifndef WASMOS_SLAB_H
#define WASMOS_SLAB_H

#include <stddef.h>

/* Initialize the slab allocator; called once during kernel startup. */
void slab_init(void);

/* Allocate size bytes from the kernel heap.  Returns NULL on exhaustion. */
void *kalloc_small(size_t size);

/* Return a previously allocated block to the kernel heap. */
void kfree_small(void *ptr);

#endif
