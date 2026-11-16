/* test_physmem.c — the physical frame allocator (src/kernel/physmem.c).
 *
 * The allocator keeps free memory as a sorted list of extents. The contract a
 * caller relies on is conservation: every frame handed out and freed again is
 * free again, however the frees interleave, so a run of contiguous frames that
 * was available before a burst of small allocations is available after it.
 * These cases fragment a fake memory map one frame at a time and check that
 * contract by the free byte count and by a contiguous allocation spanning all
 * of it.
 *
 * Regression: 2026-11-16-pfa-dropped-free. A free that needed a new extent
 * while the extent table was full was discarded: its frames left the allocator
 * for good. Under a busy boot (the logger's one-page transfer buffer per write
 * among long-lived allocations) the table filled, the losses accumulated, and
 * the process manager's 2 MiB contiguous spawn buffer stopped fitting —
 * every later exec failed with "transfer buffer unavailable" (CI boot-and-init,
 * warp_smp and wasm3_smp, test_logger).
 *
 * src/kernel/physmem.c is linked in whole, built with WASMOS_PHYSMEM_TEST_SEAMS
 * so the frames it keeps its own tables in are host memory; klog is silenced
 * here and kpanic aborts (stubs_kpanic.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "boot.h"
#include "physmem.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

#define PAGE 4096ull
/* One conventional-memory extent, inside the allocator's static 64 MiB refcount
 * window and clear of page 0, the AP trampoline page and the (host-address)
 * kernel image reservation. */
#define MAP_BASE 0x100000ull
#define MAP_PAGES 4096ull

/* physmem.c reserves [__kernel_start, __kernel_end) translated from the higher
 * half; on the host that lands far outside MAP_BASE, so it reserves nothing. */
uint8_t __kernel_start;
uint8_t __kernel_end;

/* Host memory standing in for the fake map's frames: the allocator writes its
 * free-extent table into frames it takes from the map. */
static uint8_t* g_backing;

void* physmem_test_phys_ptr(uint64_t phys) {
    if (phys < MAP_BASE || phys >= MAP_BASE + MAP_PAGES * PAGE) {
        printf("  [FAIL] the allocator dereferenced 0x%llx outside the map\n",
               (unsigned long long)phys);
        abort();
    }
    return g_backing + (phys - MAP_BASE);
}

void klog_write(const char* s) {
    (void)s;
}

void klog_printf(const char* fmt, ...) {
    (void)fmt;
}

typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} efi_desc_t;

static efi_desc_t g_map[1];
static boot_info_t g_boot;
static uint64_t g_frames[MAP_PAGES];

/* Re-initialises the allocator over the single extent. Frames from a previous
 * case keep their refcount, which allocation overwrites; the cases compare
 * against the free byte count measured here, not against pfa_total_bytes. */
static uint64_t reset(void) {
    memset(&g_boot, 0, sizeof(g_boot));
    g_map[0].Type = 7; /* EfiConventionalMemory */
    g_map[0].PhysicalStart = MAP_BASE;
    g_map[0].NumberOfPages = MAP_PAGES;
    g_boot.memory_map = g_map;
    g_boot.memory_map_size = sizeof(g_map);
    g_boot.memory_desc_size = sizeof(g_map[0]);
    pfa_init(&g_boot);
    return pfa_free_bytes();
}

/* Takes every free frame one at a time, in address order. */
static uint64_t take_all_frames(void) {
    uint64_t n = 0;
    while (n < MAP_PAGES) {
        uint64_t frame = pfa_alloc_pages(1);
        if (frame == 0) {
            break;
        }
        g_frames[n++] = frame;
    }
    return n;
}

static void free_every_other(uint64_t n, uint64_t first) {
    for (uint64_t i = first; i < n; i += 2) {
        pfa_free_pages(g_frames[i], 1);
    }
}

static void s_a_checkerboard_of_frees_is_all_free(void) {
    uint64_t baseline = reset();
    uint64_t n = take_all_frames();
    CHECK(n * PAGE == baseline, "every free frame is handed out one at a time");
    CHECK(pfa_free_bytes() == 0, "nothing is free once every frame is taken");

    /* Every second frame: each free is its own extent, far more than the
     * allocator has ever needed at boot. */
    free_every_other(n, 0);
    CHECK(pfa_free_bytes() == ((n + 1) / 2) * PAGE, "every freed frame is counted free");
}

static void s_fragmented_memory_freed_is_contiguous_again(void) {
    uint64_t baseline = reset();
    uint64_t n = take_all_frames();

    free_every_other(n, 0);
    free_every_other(n, 1);
    CHECK(pfa_free_bytes() == baseline, "after the churn the free count is back at baseline");

    /* The spawn buffer case: the whole extent is one contiguous run again. */
    uint64_t run = pfa_alloc_pages(baseline / PAGE);
    CHECK(run != 0, "one allocation spans every frame that was fragmented and freed");
    if (run != 0) {
        pfa_free_pages(run, baseline / PAGE);
    }
}

static void s_a_frame_freed_into_a_fragmented_list_can_be_allocated(void) {
    uint64_t baseline = reset();
    uint64_t n = take_all_frames();
    (void)baseline;

    free_every_other(n, 0);
    /* Each frame freed in the second pass merges two neighbours; none of them
     * may be lost, so 2 MiB (the process manager's spawn buffer) fits. */
    for (uint64_t i = 1; i < 1024 && i < n; i += 2) {
        pfa_free_pages(g_frames[i], 1);
    }
    uint64_t spawn = pfa_alloc_pages(512);
    CHECK(spawn != 0, "a 2 MiB contiguous run fits once its frames are all freed");
}

static void s_an_above_allocation_that_splits_an_extent_keeps_both_remnants(void) {
    (void)reset();
    uint64_t n = take_all_frames();

    /* One contiguous block at the top, then the low half fragmented. */
    for (uint64_t i = n - 64; i < n; ++i) {
        pfa_free_pages(g_frames[i], 1);
    }
    for (uint64_t i = 0; i < n / 2; i += 2) {
        pfa_free_pages(g_frames[i], 1);
    }
    uint64_t before = pfa_free_bytes();

    /* A floor in the middle of the block carves its middle: the block splits. */
    uint64_t frame = pfa_alloc_pages_above(1, g_frames[n - 32]);
    CHECK(frame == g_frames[n - 32], "the run starts at the floor");
    CHECK(pfa_free_bytes() == before - PAGE, "only the allocated frame leaves the free list");
    if (frame != 0) {
        pfa_free_pages(frame, 1);
    }
    CHECK(pfa_free_bytes() == before, "freeing it restores the block");
}

static const wasmos_test_void_case_t k_cases[] = {
    {"a_checkerboard_of_frees_is_all_free", s_a_checkerboard_of_frees_is_all_free},
    {"fragmented_memory_freed_is_contiguous_again", s_fragmented_memory_freed_is_contiguous_again},
    {"a_frame_freed_into_a_fragmented_list_can_be_allocated",
     s_a_frame_freed_into_a_fragmented_list_can_be_allocated},
    {"an_above_allocation_that_splits_an_extent_keeps_both_remnants",
     s_an_above_allocation_that_splits_an_extent_keeps_both_remnants},
};

int main(void) {
    printf("test_physmem\n");
    g_backing = calloc(MAP_PAGES, PAGE);
    if (!g_backing) {
        printf("test_physmem: no host memory for the map\n");
        return 1;
    }

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_physmem: FAIL\n");
        return 1;
    }
    printf("test_physmem: OK\n");
    return 0;
}
