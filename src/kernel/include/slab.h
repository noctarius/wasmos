/* slab.h - Kernel slab allocator for small fixed-size objects. One global heap,
 * shared by every CPU and independent of the running process, which is what
 * makes it the correct backing for kernel-global metadata.
 *
 * A small set of power-of-two size classes covers requests up to roughly a
 * hundred bytes; there is no large-allocation path, so anything past the
 * largest class simply fails. Callers needing more go to physmem directly. */
#ifndef WASMOS_SLAB_H
#define WASMOS_SLAB_H

#include <stddef.h>

/* Initialize the slab allocator; called once during kernel startup. */
void slab_init(void);

/* Allocate size bytes from the kernel heap.  Returns NULL when size exceeds the
 * largest size class and when no backing frame can be obtained to grow it.
 * Each allocation carries a small header, so the usable payload is a few bytes
 * short of the class size. */
void* kalloc_small(size_t size);

/* Return a previously allocated block to the kernel heap. NULL is ignored, and
 * a pointer whose header magic does not check out is dropped rather than
 * pushed onto a free list. */
void kfree_small(void* ptr);

#endif
