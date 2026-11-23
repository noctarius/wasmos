/* test_phys_map.c — the page span and the per-process window table behind
 * wasmos_phys_map (src/kernel/phys_map.c).
 *
 * Regression: 2026-11-23-phys-alias-range -- the range check computed the end
 * as phys + size, which wraps for a phys near 2^64, so the check passed a range
 * it never examined and a caller holding mmio.map faulted the kernel. Raised on
 * PR #71; the span below must refuse a wrapping range.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "test_shuffle.h"

#include "phys_map.h"

static void release_all(void) {
    for (uint32_t pid = 1; pid <= 4; ++pid) {
        phys_map_window_release_pid(pid, 0);
    }
}

static void test_the_span_covers_the_whole_pages_a_range_touches(void) {
    uint64_t page = 0;
    uint32_t pages = 0;
    assert(phys_map_span(0x7FE1024u, 0x24u, &page, &pages) == 0);
    assert(page == 0x7FE1000u && pages == 1);
    assert(phys_map_span(0x7FE1FF0u, 0x20u, &page, &pages) == 0);
    assert(page == 0x7FE1000u && pages == 2);
    assert(phys_map_span(0x7FE1000u, 0x1000u, &page, &pages) == 0);
    assert(page == 0x7FE1000u && pages == 1);
}

static void test_the_span_refuses_empty_wrapping_and_oversized_ranges(void) {
    uint64_t page = 0;
    uint32_t pages = 0;
    assert(phys_map_span(0, 0x1000u, &page, &pages) == -1);
    assert(phys_map_span(0x1000u, 0, &page, &pages) == -1);
    assert(phys_map_span(0xFFFFFFFFFFFFF000ULL, 0x1000u, &page, &pages) == -1);
    assert(phys_map_span(0xFFFFFFFFFFFFFFFFULL, 0x1000u, &page, &pages) == -1);
    assert(phys_map_span(0x1000u, (uint64_t)PHYS_MAP_MAX_PAGES * 0x1000u, &page, &pages) == 0);
    assert(phys_map_span(0x1001u, (uint64_t)PHYS_MAP_MAX_PAGES * 0x1000u, &page, &pages) == -1);
}

/* A table, a table header and its tail are usually one page apart at most, so
 * acpi-bus asks for ranges an earlier window already covers. */
static void test_a_range_inside_a_held_window_resolves_to_it(void) {
    release_all();
    phys_map_window_t w = {.pid = 1, .offset = 0x40000u, .phys = 0x7FE0000u, .pages = 2};
    assert(phys_map_window_record(&w) == 0);
    uint32_t off = 0;
    assert(phys_map_window_find(1, 0x7FE0024u, 0x24u, &off) == 1);
    assert(off == 0x40024u);
    assert(phys_map_window_find(1, 0x7FE1F00u, 0x100u, &off) == 1);
    assert(off == 0x41F00u);
    release_all();
}

static void test_a_range_outside_or_past_a_window_does_not_resolve(void) {
    release_all();
    phys_map_window_t w = {.pid = 1, .offset = 0x40000u, .phys = 0x7FE0000u, .pages = 1};
    assert(phys_map_window_record(&w) == 0);
    uint32_t off = 0;
    assert(phys_map_window_find(1, 0x7FE0F00u, 0x200u, &off) == 0);
    assert(phys_map_window_find(1, 0x7FDFFF0u, 0x20u, &off) == 0);
    assert(phys_map_window_find(2, 0x7FE0024u, 0x24u, &off) == 0);
    assert(phys_map_window_find(1, 0x7FE0024u, 0, &off) == 0);
    release_all();
}

static uint32_t g_unmapped_pids[8];
static uint32_t g_unmapped_count;

static void record_unmap(const phys_map_window_t* window) {
    g_unmapped_pids[g_unmapped_count++] = window->pid;
    uint32_t off = 0;
    /* The window has left the table before the callback runs. */
    assert(phys_map_window_find(window->pid, window->phys, 1, &off) == 0);
}

static void test_releasing_a_process_unmaps_exactly_its_windows(void) {
    release_all();
    phys_map_window_t a = {.pid = 1, .offset = 0x40000u, .phys = 0x7FE0000u, .pages = 1};
    phys_map_window_t b = {.pid = 1, .offset = 0x42000u, .phys = 0x7FF0000u, .pages = 1};
    phys_map_window_t c = {.pid = 2, .offset = 0x40000u, .phys = 0x7FE0000u, .pages = 1};
    assert(phys_map_window_record(&a) == 0);
    assert(phys_map_window_record(&b) == 0);
    assert(phys_map_window_record(&c) == 0);
    g_unmapped_count = 0;
    phys_map_window_release_pid(1, record_unmap);
    assert(g_unmapped_count == 2);
    assert(g_unmapped_pids[0] == 1 && g_unmapped_pids[1] == 1);
    uint32_t off = 0;
    assert(phys_map_window_find(2, 0x7FE0000u, 1, &off) == 1);
    release_all();
}

static void test_a_full_table_refuses_a_window(void) {
    release_all();
    phys_map_window_t w = {.pid = 3, .offset = 0, .phys = 0x1000u, .pages = 1};
    int recorded = 0;
    while (phys_map_window_record(&w) == 0) {
        recorded++;
        w.phys += 0x1000u;
        assert(recorded < 100000);
    }
    assert(recorded > 0);
    phys_map_window_release_pid(3, 0);
    assert(phys_map_window_record(&w) == 0);
    release_all();
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_the_span_covers_the_whole_pages_a_range_touches),
        WASMOS_TEST_CASE(test_the_span_refuses_empty_wrapping_and_oversized_ranges),
        WASMOS_TEST_CASE(test_a_range_inside_a_held_window_resolves_to_it),
        WASMOS_TEST_CASE(test_a_range_outside_or_past_a_window_does_not_resolve),
        WASMOS_TEST_CASE(test_releasing_a_process_unmaps_exactly_its_windows),
        WASMOS_TEST_CASE(test_a_full_table_refuses_a_window),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_phys_map: ok\n");
    return 0;
}
