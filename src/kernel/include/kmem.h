/* kmem.h - Small shared allocator for kernel container metadata.
 *
 * Slab-backed (kalloc_small/kfree_small, one global kernel heap), with a fixed
 * early-boot arena fallback for allocations made before the slab allocator is
 * initialized or once it is exhausted.  Shared by the generic container
 * primitives (list.c, hashmap.c); not tied to any one of them.
 *
 * kmem must not use the plain libc malloc/free: on the wasm3 backend that heap
 * is per-process, so kernel-global container nodes allocated from it dangle the
 * moment that process is reaped. */
#ifndef WASMOS_KMEM_H
#define WASMOS_KMEM_H

#include <stddef.h>
#include <stdint.h>

/* Allocate size bytes (8-byte aligned).  Returns NULL on failure. */
void* kmem_alloc(size_t size);

/* Free a pointer previously returned by kmem_alloc (NULL is ignored).
 * Early-arena allocations are never reclaimed; freeing one is a no-op. */
void kmem_free(void* ptr);

#endif
