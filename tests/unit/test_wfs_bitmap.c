/* Host unit test for the WFS allocation bitmaps (wfs_bitmap.h, §12).
 *
 * Pure: one bitmap block, no device and no coroutine runtime, because this is
 * where the allocation POLICY lives -- first run long enough for the request,
 * else the longest run available -- and a policy is worth testing without an
 * image under it.
 *
 * A bitmap block is written by mkfs_wfs and read by the driver, so the bit order
 * is part of the on-disk format, not a private convention: least-significant bit
 * first within each byte, a set bit meaning allocated (§12).
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "wfs_bitmap.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

static void expect_u32(uint32_t got, uint32_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %u, want %u\n", what, (unsigned)got, (unsigned)want);
    }
}

#define MAP_BYTES 64u
#define MAP_BITS (MAP_BYTES * 8u)

static uint8_t g_map[MAP_BYTES];

static void map_clear_all(void) {
    memset(g_map, 0, sizeof(g_map));
}

static void map_set_all(void) {
    memset(g_map, 0xFF, sizeof(g_map));
}

/* ---- bit access ---------------------------------------------------------- */

/* The bit order is on-disk format: mkfs_wfs sets bit i as
 * map[i >> 3] |= 1 << (i & 7), so a reader that packed the other way would see a
 * mirrored allocation state within every byte. */
static void test_bits_are_least_significant_first(void) {
    map_clear_all();

    wfs_bitmap_set(g_map, 0u);
    expect_u32(g_map[0], 0x01u, "bit 0 is the low bit of byte 0");
    wfs_bitmap_set(g_map, 7u);
    expect_u32(g_map[0], 0x81u, "bit 7 is the high bit of byte 0");
    wfs_bitmap_set(g_map, 8u);
    expect_u32(g_map[1], 0x01u, "bit 8 starts byte 1");

    expect(wfs_bitmap_test(g_map, 0u) != 0, "bit 0 reads back set");
    expect(wfs_bitmap_test(g_map, 7u) != 0, "bit 7 reads back set");
    expect(wfs_bitmap_test(g_map, 8u) != 0, "bit 8 reads back set");
    expect(wfs_bitmap_test(g_map, 1u) == 0, "an untouched bit reads clear");
}

static void test_clear_releases_only_its_own_bit(void) {
    map_set_all();

    wfs_bitmap_clear(g_map, 3u);
    expect_u32(g_map[0], 0xF7u, "clearing bit 3 leaves the rest of the byte");
    expect(wfs_bitmap_test(g_map, 3u) == 0, "and the bit reads clear");
    expect(wfs_bitmap_test(g_map, 2u) != 0, "its neighbour below is untouched");
    expect(wfs_bitmap_test(g_map, 4u) != 0, "its neighbour above is untouched");
}

/* ---- free counters ------------------------------------------------------- */

/* A free counter is derived from the bitmap and rebuilt by fsck (§12), so this
 * is the definition of that number rather than a convenience. */
static void test_free_count_is_derived_from_the_bits(void) {
    map_clear_all();
    expect_u32(wfs_bitmap_count_free(g_map, MAP_BITS), MAP_BITS, "an empty map is all free");

    map_set_all();
    expect_u32(wfs_bitmap_count_free(g_map, MAP_BITS), 0u, "a full map has nothing free");

    map_clear_all();
    wfs_bitmap_set(g_map, 0u);
    wfs_bitmap_set(g_map, 100u);
    expect_u32(wfs_bitmap_count_free(g_map, MAP_BITS), MAP_BITS - 2u, "two allocated bits");

    /* Only the first `bits` count: the last group of a volume is partial, and
     * counting the padding bits would report free space the device lacks. */
    map_clear_all();
    expect_u32(wfs_bitmap_count_free(g_map, 10u), 10u, "a partial range counts only its own bits");
    wfs_bitmap_set(g_map, 3u);
    expect_u32(wfs_bitmap_count_free(g_map, 10u), 9u, "and reflects allocations inside it");
    wfs_bitmap_set(g_map, 20u);
    expect_u32(wfs_bitmap_count_free(g_map, 10u), 9u, "a bit past the range does not count");
}

/* ---- the run search ----------------------------------------------------- */

static void test_a_request_that_fits_is_taken_whole(void) {
    uint32_t start = 0xFFFFFFFFu;

    map_clear_all();
    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 8u, &start), 8u, "eight blocks are found");
    expect_u32(start, 0u, "at the start of an empty map");

    /* First fit, not best fit: the earliest run long enough wins, which is what
     * keeps an allocation near the metadata that precedes it. */
    map_clear_all();
    wfs_bitmap_set(g_map, 0u);
    wfs_bitmap_set(g_map, 1u);
    expect_u32(
        wfs_bitmap_find_run(g_map, MAP_BITS, 4u, &start), 4u, "a run after the allocated bits");
    expect_u32(start, 2u, "starting at the first clear bit");
}

/* Contiguous first, fragmented as a fallback (§12): when nothing is long enough
 * the LONGEST run is returned so the caller can take it and come back, rather
 * than being told there is no space while space exists. */
static void test_the_longest_run_is_returned_when_nothing_fits(void) {
    uint32_t start = 0xFFFFFFFFu;
    uint32_t i;

    map_set_all();
    /* Two fragments: three bits at 10, five bits at 40. */
    for (i = 10u; i < 13u; ++i) {
        wfs_bitmap_clear(g_map, i);
    }
    for (i = 40u; i < 45u; ++i) {
        wfs_bitmap_clear(g_map, i);
    }

    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 5u, &start), 5u, "the exact fit is taken");
    expect_u32(start, 40u, "which is the later fragment");

    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 100u, &start),
               5u,
               "an oversized request falls back to the longest run");
    expect_u32(start, 40u, "reported at that run's start");

    expect_u32(
        wfs_bitmap_find_run(g_map, MAP_BITS, 3u, &start), 3u, "a smaller request fits earlier");
    expect_u32(start, 10u, "in the first fragment");
}

static void test_a_full_map_yields_nothing(void) {
    uint32_t start = 0x5A5Au;

    map_set_all();
    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 1u, &start), 0u, "a full map has no run");
    expect_u32(start, 0x5A5Au, "and the output is left untouched");
}

static void test_a_single_free_bit_is_a_run_of_one(void) {
    uint32_t start = 0u;

    map_set_all();
    wfs_bitmap_clear(g_map, 33u);
    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 4u, &start), 1u, "one bit is all there is");
    expect_u32(start, 33u, "found at its own index");
}

/* A run must not be reported as continuing past the range: the last group of a
 * volume is partial, and a run that ran off its end would hand out blocks the
 * device does not have. */
static void test_a_run_stops_at_the_range_end(void) {
    uint32_t start = 0u;

    map_clear_all();
    expect_u32(wfs_bitmap_find_run(g_map, 5u, 8u, &start), 5u, "the run is clamped to the range");
    expect_u32(start, 0u, "from the first bit");

    /* A range that ends mid-byte must not pick up the bits above it. */
    map_set_all();
    wfs_bitmap_clear(g_map, 3u);
    wfs_bitmap_clear(g_map, 4u);
    wfs_bitmap_clear(g_map, 5u);
    expect_u32(wfs_bitmap_find_run(g_map, 5u, 8u, &start), 2u, "only the bits inside the range");
    expect_u32(start, 3u, "starting where the clear bits do");
}

static void test_invalid_requests_are_refused(void) {
    uint32_t start = 0x1234u;

    map_clear_all();
    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 0u, &start), 0u, "a zero-length request");
    expect_u32(wfs_bitmap_find_run(g_map, 0u, 4u, &start), 0u, "an empty range");
    expect_u32(wfs_bitmap_find_run(0, MAP_BITS, 4u, &start), 0u, "a NULL map");
    expect_u32(wfs_bitmap_find_run(g_map, MAP_BITS, 4u, 0), 0u, "a NULL output");
    expect_u32(start, 0x1234u, "none of which touch the output");
    expect_u32(wfs_bitmap_count_free(0, MAP_BITS), 0u, "counting a NULL map");
    expect(wfs_bitmap_test(0, 3u) == 0, "testing a NULL map");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_bits_are_least_significant_first),
    WASMOS_TEST_CASE(test_clear_releases_only_its_own_bit),
    WASMOS_TEST_CASE(test_free_count_is_derived_from_the_bits),
    WASMOS_TEST_CASE(test_a_request_that_fits_is_taken_whole),
    WASMOS_TEST_CASE(test_the_longest_run_is_returned_when_nothing_fits),
    WASMOS_TEST_CASE(test_a_full_map_yields_nothing),
    WASMOS_TEST_CASE(test_a_single_free_bit_is_a_run_of_one),
    WASMOS_TEST_CASE(test_a_run_stops_at_the_range_end),
    WASMOS_TEST_CASE(test_invalid_requests_are_refused),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_bitmap: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_bitmap: %d checks passed\n", g_checks);
    return 0;
}
