/* Host unit test for kmem's three allocation sources and, above all, for how
 * kmem_free tells them apart.
 *
 * kmem serves a request from the slab, from whole frames when the slab refuses
 * it, or from the early bump arena when neither can. Those blocks are freed
 * three different ways and arrive at kmem_free as bare pointers, so the whole
 * correctness of the allocator rests on a discriminator that has no help from
 * the caller.
 *
 * The arena is an address RANGE, which is exact. A page-backed block is
 * recognised from a header written before it -- and that is where this suite
 * earns its keep, because the obvious implementation of that check reads memory
 * in front of a pointer that may have no header at all.
 *
 * This file provides its own slab and frame allocator rather than linking
 * stubs_slab.c: the point of most cases here is a slab that REFUSES, which the
 * shared stub cannot express (its kalloc_small is malloc and never rejects a
 * size), and a frame allocator whose calls can be counted.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kmem.h"
#include "paging.h"
#include "slab.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

/* ---- the allocator under the allocator ------------------------------------ */

/* Whether the slab accepts anything at all, and what it has been asked to do.
 * A real slab refuses a request past its largest size class; `slab_limit` is how
 * a case says where that class ends. */
static size_t g_slab_limit = 128u;
static int g_slab_allocs;
static int g_slab_frees;
static void* g_slab_last_free;

/* Blocks the fake slab hands out. Deliberately NOT page aligned: a slab block
 * must not land where a page-backed block would, or the cases below would stop
 * distinguishing the two. */
static uint8_t g_slab_arena[64u * 1024u] __attribute__((aligned(4096)));
/* Page aligned with a fixed starting offset, so a block's position within its
 * page is deterministic and a case can be sure a slab block does NOT land where
 * a page-backed one begins. */
static size_t g_slab_off = 64u;

void slab_init(void) {}

void* kalloc_small(size_t size) {
    void* p;

    if (size == 0u || size > g_slab_limit) {
        return 0;
    }
    if (g_slab_off + size + 32u > sizeof(g_slab_arena)) {
        return 0;
    }
    /* 32 bytes of slack in front of every block, which is what lets a case
     * write a forged header there without running off the arena or into the
     * block before it. */
    g_slab_off += 32u;
    p = &g_slab_arena[g_slab_off];
    g_slab_off = (g_slab_off + size + 7u) & ~(size_t)7u;
    g_slab_allocs++;
    return p;
}

void kfree_small(void* ptr) {
    g_slab_frees++;
    g_slab_last_free = ptr;
}

/* The frame allocator. Pages come out of a page-aligned static region so a
 * returned "physical" address behaves like one: page aligned, and mappable to a
 * kernel address by OR-ing the higher-half base, which is exactly what
 * kmem_alloc does with it. */
static int g_pfa_allocs;
static int g_pfa_frees;
static uint64_t g_pfa_last_free_base;
static uint64_t g_pfa_last_free_pages;
static uint64_t g_pfa_fail;

static uint8_t g_frames[16u * 4096u] __attribute__((aligned(4096)));
static uint64_t g_frame_next;

/* Always refuses.
 *
 * kmem_alloc turns a physical address into a kernel one by OR-ing
 * KERNEL_HIGHER_HALF_BASE, and no address a host process owns survives that, so
 * the page ALLOCATION path cannot run here at all -- a frame this returned would
 * be dereferenced at a different address and the test would fault rather than
 * assert. Serving nothing keeps that honest: what this file covers is
 * kmem_free's DISCRIMINATOR, and the blocks it is given are built by hand, byte
 * for byte as kmem_alloc lays them out. The allocation path is covered on
 * target, where the mapping is real. */
uint64_t pfa_alloc_pages(uint64_t pages) {
    (void)pages;
    g_pfa_allocs++;
    return 0;
}

void pfa_free_pages(uint64_t base, uint64_t pages) {
    g_pfa_frees++;
    g_pfa_last_free_base = base;
    g_pfa_last_free_pages = pages;
}

static void reset(void) {
    g_slab_limit = 128u;
    g_slab_allocs = 0;
    g_slab_frees = 0;
    g_slab_last_free = 0;
    g_slab_off = 64u;
    g_pfa_allocs = 0;
    g_pfa_frees = 0;
    g_pfa_last_free_base = 0;
    g_pfa_last_free_pages = 0;
    g_pfa_fail = 0;
    g_frame_next = 0;
    memset(g_frames, 0, sizeof(g_frames));
}

/* The header kmem writes in front of a page-backed block. Mirrored rather than
 * shared because kmem.c keeps it private, which is right -- nothing outside the
 * allocator may depend on it. A case forging one is testing the DISCRIMINATOR,
 * so it has to speak the same bytes. */
#define TEST_KMEM_PAGE_MAGIC 0x4B4D454Du
typedef struct {
    uint32_t magic;
    uint32_t pages;
    uint64_t phys;
    uint64_t self; /* the header's own address; what makes the check exact */
} test_kmem_header_t;

/* ---- cases ---------------------------------------------------------------- */

/* A slab block whose preceding bytes happen to spell a page header must still
 * be freed to the slab.
 *
 * kmem_free receives a bare pointer and has to decide which of three allocators
 * owns it. Reading in front of EVERY pointer to look for a page header is both
 * an out-of-bounds read -- those bytes belong to the slab, or to nothing -- and
 * wrong whenever they happen to match: the block is then handed to the frame
 * allocator, which frees memory it never owned while the slab leaks the block.
 *
 * The forged header here is what makes that deterministic instead of a
 * once-in-a-while crash. It was found the other way round: an ASan build of
 * test_service_class_registry reported the read, one run in six.
 *
 * Regression: 2026-09-21-kmem-free-probes-before-every-pointer */
static void test_a_slab_block_is_not_mistaken_for_a_page_backed_one(void) {
    void* p;
    uint32_t i;

    reset();
    p = kmem_alloc(32u);
    expect(p != 0, "the slab serves a small request");
    expect(g_pfa_allocs == 0, "and no frame is taken for it");
    if (!p) {
        return;
    }
    expect(((uintptr_t)p % 4096u) != sizeof(test_kmem_header_t),
           "the block does not sit where a page-backed one begins, so the only "
           "thing that could make it look page-backed is the forgery below");

    /* The magic word at every 4-byte position in the 32 bytes ahead of the
     * block, so a probe finds it wherever it looks. Memory in front of a slab
     * block belongs to the slab and may hold anything; this is the worst
     * anything it could hold, and the case is deliberately independent of where
     * a header would sit. */
    for (i = 0; i < 8u; ++i) {
        uint32_t word = TEST_KMEM_PAGE_MAGIC;
        memcpy((uint8_t*)p - 32u + (size_t)i * 4u, &word, sizeof(word));
    }

    kmem_free(p);
    expect(g_pfa_frees == 0,
           "a slab block is not handed to the frame allocator because the bytes "
           "in front of it look like a header");
    expect(g_slab_frees == 1 && g_slab_last_free == p, "it goes back to the slab");
}

/* The same forgery at the one offset a page-backed block really occupies.
 *
 * A page-backed block begins exactly sizeof(header) into a page, so that offset
 * is the only place the check may look -- but the offset alone does not make a
 * header real. The phys field must name the very page the header sits in, which
 * is what kmem_alloc wrote and what nothing else reproduces by accident. */
static void test_a_forged_header_at_the_right_offset_is_still_rejected(void) {
    uint8_t* page;
    void* p;
    test_kmem_header_t forged;

    reset();
    page = &g_frames[0];
    p = page + sizeof(test_kmem_header_t);

    forged.magic = TEST_KMEM_PAGE_MAGIC;
    forged.pages = 1u;
    forged.phys = 0x1000u;
    /* Names a DIFFERENT header, which is what a stale or coincidental run of
     * bytes would do and what a real header never does. */
    forged.self = (uint64_t)(uintptr_t)&g_frames[(size_t)8u * 4096u];
    memcpy(page, &forged, sizeof(forged));

    kmem_free(p);
    expect(g_pfa_frees == 0, "a header that does not name its own address is not believed");
    expect(g_slab_frees == 1, "and the block falls through to the slab");
}

/* A well-formed page-backed block is recognised and its pages returned.
 *
 * Built by hand, exactly as kmem_alloc lays one out: the header at the head of a
 * page, the block immediately behind it, and `self` naming the header's own
 * address. That last field is the whole of what makes the check exact -- magic
 * and a plausible page count can both occur by accident in memory that belongs
 * to something else, but a pointer to itself cannot. */
static void test_a_well_formed_page_backed_block_returns_its_pages(void) {
    uint8_t* page;
    void* p;
    test_kmem_header_t hdr;

    reset();
    page = &g_frames[0];
    p = page + sizeof(test_kmem_header_t);

    hdr.magic = TEST_KMEM_PAGE_MAGIC;
    hdr.pages = 3u;
    hdr.phys = 0xDEAD000u;
    hdr.self = (uint64_t)(uintptr_t)page;
    memcpy(page, &hdr, sizeof(hdr));

    kmem_free(p);
    expect(g_pfa_frees == 1, "the frame allocator is asked to take it back");
    expect(g_pfa_last_free_base == 0xDEAD000u, "with the physical address the header recorded");
    expect(g_pfa_last_free_pages == 3u, "and every page it took");
    expect(g_slab_frees == 0, "the slab is not asked to free what it never served");
}

/* Neither the slab nor the frame allocator can serve, which is early boot.
 *
 * The arena has no reclaim, so the only thing kmem_free can do with one of its
 * pointers is drop it -- and the thing that must NOT happen is for it to reach
 * either of the other two allocators. */
static void test_an_early_arena_block_is_dropped_by_both_other_allocators(void) {
    void* p;
    uint32_t used_before;

    reset();
    g_slab_limit = 0u; /* nothing is small enough */
    g_pfa_fail = 1u;   /* and no frame can be had */
    used_before = kmem_early_arena_used();

    p = kmem_alloc(64u);
    expect(p != 0, "the arena serves when nothing else can");
    expect(kmem_early_arena_used() > used_before, "and the arena records it");
    if (!p) {
        return;
    }

    kmem_free(p);
    expect(g_pfa_frees == 0, "an arena pointer is not a frame");
    expect(g_slab_frees == 0, "and it is not a slab block either");
}

/* Freeing NULL is a no-op, which callers rely on for their error paths. */
static void test_freeing_null_reaches_nothing(void) {
    reset();
    kmem_free(0);
    expect(g_pfa_frees == 0 && g_slab_frees == 0, "kmem_free(NULL) touches no allocator");
}

/* A zero-size request is refused rather than served a block nothing sized. */
static void test_a_zero_size_request_is_refused(void) {
    reset();
    expect(kmem_alloc(0u) == 0, "kmem_alloc(0) returns NULL");
    expect(g_slab_allocs == 0 && g_pfa_allocs == 0, "without asking anything for it");
}

int main(void) {
    test_a_slab_block_is_not_mistaken_for_a_page_backed_one();
    test_a_forged_header_at_the_right_offset_is_still_rejected();
    test_a_well_formed_page_backed_block_returns_its_pages();
    test_an_early_arena_block_is_dropped_by_both_other_allocators();
    test_freeing_null_reaches_nothing();
    test_a_zero_size_request_is_refused();

    if (g_failures) {
        printf("test_kmem: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_kmem: %d checks passed\n", g_checks);
    return 0;
}
