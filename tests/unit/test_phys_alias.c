/* test_phys_alias.c — the range check wasmos_phys_map makes before it copies
 * from the kernel's higher-half alias (src/kernel/phys_alias.c).
 *
 * The fake page table below maps every page of a 512 MiB window at the x86_64
 * alias base, except one hole, and translates like paging_virt_to_phys: the
 * physical address behind the alias, which is 0 for frame 0, and 0 for
 * anything unmapped.
 *
 * Regression: 2026-11-23-phys-alias-range -- the check computed the range's
 * end as phys + size, which wraps for a phys near 2^64: the window comparison
 * passed, the page loop ran zero times, and wasmos_phys_map's memcpy faulted
 * the kernel for a caller holding mmio.map. It also probed the first page at
 * its frame base, so a range starting inside frame 0 (phys 0x400) read the
 * frame's translation, 0, as "unmapped" and was refused although mapped.
 * Raised on PR #71.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "test_shuffle.h"

#include "phys_alias.h"

#define ALIAS_BASE 0xFFFFFFFF80000000ULL
#define WINDOW (512ULL * 1024ULL * 1024ULL)
#define HOLE 0x100000ULL

static uint64_t fake_virt_to_phys(uint64_t virt) {
    if (virt < ALIAS_BASE || virt - ALIAS_BASE >= WINDOW) {
        return 0;
    }
    uint64_t phys = virt - ALIAS_BASE;
    if ((phys & ~0xFFFULL) == HOLE) {
        return 0;
    }
    return phys;
}

static int mapped(uint64_t phys, uint64_t size) {
    return phys_alias_range_mapped(phys, size, ALIAS_BASE, WINDOW, fake_virt_to_phys);
}

static void test_a_mapped_range_inside_the_window_is_accepted(void) {
    assert(mapped(0x1000u, 0x3000u) == 1);
    assert(mapped(WINDOW - 0x1000u, 0x1000u) == 1);
}

static void test_a_range_past_the_window_is_refused(void) {
    assert(mapped(WINDOW - 0x1000u, 0x2000u) == 0);
    assert(mapped(WINDOW, 0x1000u) == 0);
    assert(mapped(0x100000000ULL, 0x1000u) == 0);
}

static void test_a_range_over_a_hole_is_refused(void) {
    assert(mapped(HOLE - 0x1000u, 0x2000u) == 0);
    assert(mapped(HOLE + 0x800u, 0x100u) == 0);
}

/* The end of [phys, phys + size) does not fit in 64 bits. */
static void test_a_range_whose_end_wraps_is_refused(void) {
    assert(mapped(0xFFFFFFFFFFFFF000ULL, 0x1000u) == 0);
    assert(mapped(0xFFFFFFFFFFFFFFFFULL, 0x1000u) == 0);
}

/* Frame 0 is mapped; its translation is 0, the same value as "unmapped". */
static void test_a_range_starting_inside_frame_zero_is_accepted(void) {
    assert(mapped(0x400u, 0x1000u) == 1);
    assert(mapped(0xFFFu, 0x1u) == 1);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_mapped_range_inside_the_window_is_accepted),
        WASMOS_TEST_CASE(test_a_range_past_the_window_is_refused),
        WASMOS_TEST_CASE(test_a_range_over_a_hole_is_refused),
        WASMOS_TEST_CASE(test_a_range_whose_end_wraps_is_refused),
        WASMOS_TEST_CASE(test_a_range_starting_inside_frame_zero_is_accepted),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_phys_alias: ok\n");
    return 0;
}
