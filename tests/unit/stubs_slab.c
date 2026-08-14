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

/* Nothing to build: the host heap is already usable, so a suite may skip this
 * call entirely and still allocate, which on target it could not. */
void slab_init(void) {}

/* Returns `size` usable bytes, or NULL only when the host heap is exhausted.
 * Two things a test cannot reach through it: the real allocator refuses any
 * request past its largest size class, and it also fails when no backing frame
 * can be obtained to grow the heap. Neither rejection happens here, so a caller
 * whose fallback path exists for those cases -- kmem's early arena, for one --
 * stays uncovered. The real allocator also spends a few bytes of each block on a
 * header, so the payload it hands back is slightly smaller than requested; this
 * one hands back the full size. size == 0 follows host malloc: it may return
 * either NULL or a unique freeable pointer. */
void* kalloc_small(size_t size) {
    return malloc(size);
}

/* Frees a block obtained from kalloc_small; NULL is ignored. The real
 * implementation additionally validates a per-block header magic and silently
 * drops a pointer that fails it, whereas passing a foreign or already-freed
 * pointer here is undefined behaviour in the host allocator. */
void kfree_small(void* ptr) {
    free(ptr);
}