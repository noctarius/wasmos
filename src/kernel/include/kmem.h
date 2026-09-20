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

/* Bytes already handed out of the fixed early arena, and its total size. The
 * arena is never reclaimed, so the first is a high-water mark: a failure path
 * that reports it says whether the kernel ran out of this or of something
 * else. */
uint32_t kmem_early_arena_used(void);
uint32_t kmem_early_arena_bytes(void);
/* How many requests the arena has served, and the largest of them. Together with
 * the used watermark these say whether it was drained by a few large requests
 * the slab could not size, or by many small ones it could not satisfy. */
uint32_t kmem_early_arena_count(void);
uint32_t kmem_early_arena_max_request(void);

#endif
