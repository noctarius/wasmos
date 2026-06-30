/* kmem.h - Small shared allocator for kernel container metadata.
 *
 * malloc-backed, with a fixed early-boot arena fallback for allocations that
 * occur before the heap allocator is fully available.  Shared by the generic
 * container primitives (list.c, hashmap.c); not tied to any one of them. */
#ifndef WASMOS_KMEM_H
#define WASMOS_KMEM_H

#include <stddef.h>
#include <stdint.h>

/* Allocate size bytes (8-byte aligned).  Returns NULL on failure. */
void *kmem_alloc(size_t size);

/* Free a pointer previously returned by kmem_alloc (NULL is ignored). */
void kmem_free(void *ptr);

#endif
