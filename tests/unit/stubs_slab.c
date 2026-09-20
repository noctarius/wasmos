/* stubs_slab.c - Host-side stand-ins for the kernel allocators kmem.c sits on:
 * the slab (slab.h) and the physical frame allocator (physmem.h).
 *
 * kmem.c allocates container metadata from kalloc_small/kfree_small; the host
 * test build has no physical frame allocator to back a slab, so these forward
 * to the host libc heap. Two differences from the real allocator that the
 * suites depend on: any size succeeds (the slab caps requests at its largest
 * size class), and allocation only fails when the host is out of memory, so
 * kmem's early-arena fallback is never reached in a test. */
#include "slab.h"
#include "physmem.h"

#include <stdlib.h>

/* The frame allocator kmem.c page-backs large requests from. Reporting NO frame
 * is the useful stub, not a host malloc: the returned value is a PHYSICAL
 * address that kmem turns into a pointer through the higher-half alias, which
 * has no meaning on the host. With this returning 0, kmem falls through to its
 * early arena exactly as it did before page-backing existed, so the suites see
 * the behaviour they were written against.
 *
 * It also means the page path itself is not covered here; it is exercised on
 * target, where a mount/unmount loop that used to exhaust the arena at nine
 * cycles now runs twenty-four without one. */
/* WEAK: tests/unit/stubs_xfer_buffer_platform.c defines these too, and some
 * targets link both. The strong definition wins wherever it is present, so this
 * only fills in for the targets that link kmem.c without it. */
__attribute__((weak)) uint64_t pfa_alloc_pages(uint64_t pages) {
    (void)pages;
    return 0;
}

__attribute__((weak)) void pfa_free_pages(uint64_t base, uint64_t pages) {
    (void)base;
    (void)pages;
}

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