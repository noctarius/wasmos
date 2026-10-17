/* kmem.c - Small shared allocator for kernel container metadata.
 * Slab-backed (kalloc_small/kfree_small is a single GLOBAL kernel heap), with a
 * fixed early-boot arena fallback for allocations that happen before the slab
 * allocator is initialised or when it is exhausted.  Shared by the generic
 * containers (list.c, hashmap.c).
 *
 * kmem MUST NOT use the plain libc malloc/free: on the wasm3 backend that heap
 * is per-process (bound to the CPU-local wasm3 pid), so kernel-global container
 * nodes allocated from it dangle the moment that process is reaped.  The slab
 * (and the arena fallback) are process-independent, matching the WARP backend
 * where malloc already forwards to kalloc_small. */
#include "kmem.h"
#include "slab.h"
#include "physmem.h"
#include "paging.h"

/* Page-backed block for a request the slab cannot serve.
 *
 * The slab's largest size class is 128 bytes, so EVERY container chunk the
 * kernel grows -- a list_array chunk, a hashmap table -- is larger than it. Those
 * used to fall to the bump arena below and were then held for the life of the
 * kernel, which made the arena a one-way sink for ordinary runtime allocation:
 * 256 KiB of it, drained by a few hundred container growths, after which an
 * unrelated subsystem failed to allocate and reported something that named
 * neither memory nor the arena (a spawn refused as PM_SPAWN_INTERNAL_ERR_ALLOC).
 *
 * Whole pages are taken from the frame allocator instead, with the page count
 * recorded in a header so kmem_free can give them back. The header is placed
 * before the returned pointer and keeps its 8-byte alignment contract.
 *
 * `self` holds the header's own address, and it is what makes the block
 * IDENTIFIABLE rather than merely plausible. kmem_free is handed bare pointers
 * from three allocators and must not read in front of one that has no header, so
 * the check is in two parts: the block's offset within its page, which is
 * arithmetic on the pointer and reads nothing, and then the header itself. Magic
 * and a non-zero page count can both occur in memory belonging to something
 * else; a word that points at itself cannot.
 *
 * The arena is now only what its name says: an EARLY one, for allocations made
 * before the frame allocator and slab can serve anything. */
#define KMEM_PAGE_MAGIC 0x4B4D454Du /* 'KMEM' */
#define KMEM_PAGE_BYTES 4096ULL
typedef struct {
    uint32_t magic;
    uint32_t pages;
    uint64_t phys;
    uint64_t self;
} kmem_page_header_t;

/* Bump-allocated fallback arena for a request nothing else can serve yet:
 * before slab_init and before the frame allocator can hand out a page.
 * kmem_free cannot reclaim from it, so those allocations are held for the life
 * of the kernel -- which is acceptable only because early boot makes a bounded
 * number of them. */
#define KMEM_EARLY_ARENA_BYTES (256u * 1024u)
static uint8_t g_kmem_early_arena[KMEM_EARLY_ARENA_BYTES];
static uint32_t g_kmem_early_arena_off;
/* Shape of what the arena has served, for the failure paths that report it.
 * Counters rather than a log: kmem_alloc runs under its callers' locks, and
 * logging every fallback both storms the console and changes the timing of the
 * exhaustion being measured. */
static uint32_t g_kmem_early_count;
static uint32_t g_kmem_early_max_req;

static void* kmem_early_alloc(size_t size) {
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
            return (void*)aligned;
        }
    }
}

/* Slab first, early arena second.  Returns NULL for a zero size and when both
 * fail — the arena is 256 KiB and, unlike the slab, cannot grow.  The slab caps
 * a single request at its largest size class, so anything bigger comes from the
 * arena and is therefore never reclaimed.
 *
 * The returned block is uninitialised and 8-byte aligned from either source. */
void* kmem_alloc(size_t size) {
    void* ptr = 0;
    uint64_t need = 0;
    uint64_t pages = 0;
    uint64_t phys = 0;
    kmem_page_header_t* hdr = 0;

    if (size == 0) {
        return 0;
    }
    ptr = kalloc_small(size);
    if (ptr) {
        return ptr;
    }
    /* Too big for the slab: take pages, which kmem_free can return. */
    need = (uint64_t)sizeof(kmem_page_header_t) + (uint64_t)size;
    pages = (need + KMEM_PAGE_BYTES - 1u) / KMEM_PAGE_BYTES;
    /* Constrained to the higher-half window: the block is reached at
     * `phys | KERNEL_HIGHER_HALF_BASE`, and a frame above the window has no such
     * alias (paging.h). The unconstrained form can return one, and the header
     * write below would fault. */
    phys = pfa_alloc_pages_below(pages, KERNEL_HIGHER_HALF_WINDOW_BYTES);
    if (phys) {
        hdr = (kmem_page_header_t*)(uintptr_t)(phys | KERNEL_HIGHER_HALF_BASE);
        hdr->magic = KMEM_PAGE_MAGIC;
        hdr->pages = (uint32_t)pages;
        hdr->phys = phys;
        hdr->self = (uint64_t)(uintptr_t)hdr;
        return (void*)(hdr + 1);
    }
    /* No frame either -- early boot, or genuinely out of memory. */
    g_kmem_early_count++;
    if ((uint32_t)size > g_kmem_early_max_req) {
        g_kmem_early_max_req = (uint32_t)size;
    }
    return kmem_early_alloc(size);
}

/* Bytes handed out of the non-reclaimable early arena, for diagnostics.
 *
 * Read from failure paths rather than logged from inside kmem_alloc: the
 * allocator runs under locks its callers hold, and logging from there both
 * storms and changes the timing of the thing being measured. */
uint32_t kmem_early_arena_used(void) {
    return g_kmem_early_arena_off;
}

uint32_t kmem_early_arena_count(void) {
    return g_kmem_early_count;
}

uint32_t kmem_early_arena_max_request(void) {
    return g_kmem_early_max_req;
}

uint32_t kmem_early_arena_bytes(void) {
    return KMEM_EARLY_ARENA_BYTES;
}

/* Routes a bare pointer back to whichever of the three sources served it.
 *
 * The order is not arbitrary. Both tests that come before kfree_small are made
 * from the POINTER, not from memory around it: the arena is an address range,
 * and a page-backed block sits at a known offset into its page. Only once that
 * offset matches is the header read, and by then the bytes being read are
 * inside a page this allocator handed out.
 *
 * Probing in front of every pointer instead reads memory that belongs to the
 * slab, or to nothing -- and hands the block to the frame allocator whenever
 * those bytes happen to spell a header, freeing memory that was never a frame
 * while the slab leaks the block. tests/unit/test_kmem.c holds that case.
 *
 * An arena pointer is dropped: a bump allocator has no reclaim. NULL is ignored.
 * Anything left is the slab's, which rejects a foreign pointer on its own header
 * rather than corrupting a free list. */
void kmem_free(void* ptr) {
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
    /* kmem_alloc puts the header at the head of a page and returns the byte
     * after it, so this offset is the only one a page-backed block can have. */
    if ((p % KMEM_PAGE_BYTES) == (uintptr_t)sizeof(kmem_page_header_t)) {
        kmem_page_header_t* hdr = ((kmem_page_header_t*)ptr) - 1;
        if (hdr->magic == KMEM_PAGE_MAGIC && hdr->pages > 0u &&
            hdr->self == (uint64_t)(uintptr_t)hdr) {
            uint64_t phys = hdr->phys;
            uint32_t pages = hdr->pages;
            hdr->magic = 0u;
            hdr->self = 0u;
            pfa_free_pages(phys, (uint64_t)pages);
            return;
        }
    }
    kfree_small(ptr);
}
