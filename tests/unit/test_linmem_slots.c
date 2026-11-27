/* test_linmem_slots.c — the frames linmem_slot_commit (src/kernel/linmem_slots.c)
 * backs a linear-memory slot with.
 *
 * Linear memory, under either backend, lives at or above WASMOS_BUFFER_PHYS_LIMIT
 * and transfer-buffer backing below it (memory.h): WARP's ensureLinearSize
 * zero-fills linear memory through the kernel direct map, and the floor is what
 * keeps that fill off a live transfer buffer.
 *
 * Regression: 2026-11-27-linmem-commit-floor. linmem_slot_commit took its frames
 * from pfa_alloc_pages, which carves the lowest free extent, so on a machine
 * with free RAM under 64 MiB slot-backed linear memory came out of the buffer
 * zone, where a linear-memory zero-fill can land on a transfer buffer another
 * process is reading.
 *
 * The fake allocator below is first-fit like the kernel's: it hands out its
 * lowest free frame, and its free RAM starts under the floor. The fake
 * paging_map_4k records the frame it was offered and refuses the mapping, so
 * commit returns before it zero-fills a higher-half VA the host cannot map; the
 * frame offered is the one the slot would have been backed with.
 */

#include <stdio.h>
#include <string.h>

#include "linmem_slots.h"
#include "memory.h"
#include "paging.h"
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
/* Free RAM of the fake: [RAM_BASE, RAM_END), straddling the floor. */
#define RAM_BASE 0x200000ull
static uint64_t g_ram_end;
/* Next free frame from the bottom, and from the floor up (first-fit within the
 * range a caller allows). */
static uint64_t g_next_low;
static uint64_t g_next_high;

static uint64_t g_offered_phys;
static unsigned g_map_calls;
static unsigned g_frees;
static uint64_t g_freed_phys;

static void reset(uint64_t ram_end) {
    g_ram_end = ram_end;
    g_next_low = RAM_BASE;
    g_next_high = WASMOS_BUFFER_PHYS_LIMIT;
    g_offered_phys = 0;
    g_map_calls = 0;
    g_frees = 0;
    g_freed_phys = 0;
}

static uint64_t take_from(uint64_t* next, uint64_t limit, uint64_t pages) {
    uint64_t base = *next;
    if (base + pages * PAGE > limit) {
        return 0;
    }
    *next = base + pages * PAGE;
    return base;
}

uint64_t pfa_alloc_pages(uint64_t pages) {
    uint64_t phys = take_from(&g_next_low, WASMOS_BUFFER_PHYS_LIMIT, pages);
    return phys ? phys : take_from(&g_next_high, g_ram_end, pages);
}

uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr) {
    return take_from(&g_next_low, max_addr, pages);
}

uint64_t pfa_alloc_pages_above(uint64_t pages, uint64_t min_addr) {
    if (min_addr > g_next_high) {
        g_next_high = min_addr;
    }
    return take_from(&g_next_high, g_ram_end, pages);
}

void pfa_free_pages(uint64_t base, uint64_t pages) {
    (void)pages;
    g_frees++;
    g_freed_phys = base;
}

int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt;
    (void)flags;
    g_map_calls++;
    g_offered_phys = phys;
    return -1;
}

int paging_unmap_4k(uint64_t virt) {
    (void)virt;
    return 0;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    (void)virt;
    return 0;
}

static void test_commit_backs_linear_memory_above_the_floor(void) {
    reset(WASMOS_BUFFER_PHYS_LIMIT + 16 * PAGE);
    int rc = linmem_slot_commit(linmem_slot_va(0), 0, 1);
    CHECK(rc == -1, "the refused mapping is reported");
    CHECK(g_map_calls == 1, "one frame is offered for the page");
    CHECK(g_offered_phys >= WASMOS_BUFFER_PHYS_LIMIT,
          "the frame offered lies at or above WASMOS_BUFFER_PHYS_LIMIT");
    CHECK(g_frees == 1 && g_freed_phys == g_offered_phys,
          "the frame of the refused mapping is returned");
}

static void test_commit_fails_when_no_frame_lies_above_the_floor(void) {
    /* Free RAM ends at the floor: every free frame is in the buffer zone. */
    reset(WASMOS_BUFFER_PHYS_LIMIT);
    int rc = linmem_slot_commit(linmem_slot_va(0), 0, 1);
    CHECK(rc == -1, "a commit with no frame above the floor fails");
    CHECK(g_map_calls == 0, "no buffer-zone frame is mapped as linear memory");
}

int main(void) {
    test_commit_backs_linear_memory_above_the_floor();
    test_commit_fails_when_no_frame_lies_above_the_floor();
    printf("test_linmem_slots: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
