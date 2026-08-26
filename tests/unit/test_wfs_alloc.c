/* Host unit test for WFS block allocation (wfs_alloc.h, §12), run as a task on
 * the SYSTEM coroutine runtime against a volume mkfs_wfs built.
 *
 * The bitmaps are authoritative, so the assertions are made against the BITMAP
 * in the image rather than against the free counters: a counter that agrees with
 * a wrong bitmap is not evidence of anything. The counters are checked too, as
 * derived values that must follow.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_alloc.h"
#include "wfs_bitmap.h"
#include "wfs_crc32c.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_super.h"

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

static void expect_rc(wasmos_error_code_t got, wasmos_error_code_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %s (%d), want %s (%d)\n",
               what,
               wasmos_strerror(got),
               (int)got,
               wasmos_strerror(want),
               (int)want);
    }
}

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0x30, 0x91, 0x4c, 0x02, 0xbb, 0x77, 0x41, 0x18, 0x8e, 0x5a, 0x22, 0xd9, 0x6f, 0x40, 0x13, 0xc7};
#define TEST_NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

static wfs_mkfs_layout_t g_layout;
static wfs_mount_ctx_t g_mount;
static wfs_volume_t g_vol;

/* Build a volume and mount it, which is the precondition for every case here. */
static int setup(void) {
    wasmos_wasm_coroutine_t task;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return -1;
    }
    memset(&g_mount, 0, sizeof(g_mount));
    memset(&g_vol, 0, sizeof(g_vol));
    g_mount.vol = &g_vol;
    if (wfs_stub_run_task(&task, wfs_mount_task, &g_mount) != 0) {
        expect(0, "mount the volume");
        return -1;
    }
    return 0;
}

static int32_t run_alloc(wfs_alloc_ctx_t* ctx, uint32_t want, uint32_t prefer_group) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    ctx->vol = &g_vol;
    ctx->want = want;
    ctx->prefer_group = prefer_group;
    return wfs_stub_run_task(&task, wfs_alloc_blocks_task, ctx);
}

/* The group's block bitmap, straight out of the image. */
static const uint8_t* group_bitmap(uint32_t group) {
    return wfs_stub_image + (size_t)(g_layout.bitmap_start + 2u * group) * wfs_stub_block_size;
}

/* ---- allocation --------------------------------------------------------- */

static void test_an_allocation_marks_the_bitmap(void) {
    wfs_alloc_ctx_t a;
    uint32_t i;
    uint32_t marked = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(run_alloc(&a, 4u, 0u) == 0, "the allocation completes");
    expect_u32(a.length, 4u, "four blocks were allocated");
    expect(a.first_block >= g_layout.first_data_block, "outside the metadata regions");
    expect(a.first_block + a.length <= g_vol.super.total_blocks, "and inside the volume");

    for (i = 0; i < a.length; ++i) {
        if (wfs_bitmap_test(group_bitmap(0u), a.first_block + i)) {
            marked++;
        }
    }
    expect_u32(marked, a.length, "every allocated block is set in the bitmap");

    wfs_stub_teardown();
}

/* Two allocations must not overlap. The bitmap is what prevents it, so the
 * second call has to see the first call's bits. */
static void test_two_allocations_do_not_overlap(void) {
    wfs_alloc_ctx_t a;
    wfs_alloc_ctx_t b;
    uint32_t a_first;
    uint32_t a_len;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(run_alloc(&a, 3u, 0u) == 0, "the first allocation completes");
    a_first = a.first_block;
    a_len = a.length;

    expect(run_alloc(&b, 3u, 0u) == 0, "the second allocation completes");
    expect(b.first_block >= a_first + a_len || b.first_block + b.length <= a_first,
           "the second run does not overlap the first");

    wfs_stub_teardown();
}

/* The counters are derived from the bitmap (§12), so they must follow it. */
static void test_the_free_counters_follow_the_bitmap(void) {
    wfs_alloc_ctx_t a;
    uint32_t before_super;
    uint32_t bits;
    uint32_t counted;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    before_super = g_vol.super.free_blocks;

    expect(run_alloc(&a, 5u, 0u) == 0, "the allocation completes");
    expect_u32(
        g_vol.super.free_blocks, before_super - a.length, "the volume counter drops by the run");

    /* And the group descriptor's counter equals what its bitmap says, which is
     * the definition rather than a second opinion. */
    bits = g_layout.blocks_per_group;
    if (bits > g_vol.super.total_blocks) {
        bits = g_vol.super.total_blocks;
    }
    counted = wfs_bitmap_count_free(group_bitmap(0u), bits);
    expect_u32(
        g_layout.group_count, 1u, "the fixture is a single group, so its counter is the volume's");
    expect_u32(counted, g_vol.super.free_blocks, "the descriptor's bitmap agrees with the counter");

    wfs_stub_teardown();
}

/* A request larger than any single run is answered with the longest run
 * available, not refused: §12 falls back to fragments, and the caller comes back
 * for the remainder. */
static void test_an_oversized_request_returns_a_shorter_run(void) {
    wfs_alloc_ctx_t a;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(run_alloc(&a, g_vol.super.total_blocks + 100u, 0u) == 0, "the allocation completes");
    expect(a.length > 0u, "something was allocated");
    expect(a.length < g_vol.super.total_blocks, "but less than the whole volume");

    wfs_stub_teardown();
}

/* A full volume is NO_SPACE, and nothing is handed out. */
static void test_a_full_volume_reports_no_space(void) {
    wfs_alloc_ctx_t a;
    uint8_t* map;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    /* Fill the bitmap rather than allocating until empty: the point is the
     * allocator's answer when nothing is free, not how many calls that takes. */
    map = wfs_stub_image + (size_t)(g_layout.bitmap_start) * wfs_stub_block_size;
    for (i = 0; i < wfs_stub_block_size; ++i) {
        map[i] = 0xFFu;
    }

    expect_rc((wasmos_error_code_t)run_alloc(&a, 1u, 0u),
              WASMOS_ERR_FS_NO_SPACE,
              "a full volume reports no space");
    expect_u32(a.length, 0u, "and hands out nothing");

    wfs_stub_teardown();
}

/* A read-only volume refuses a write before it touches a bitmap. That gate has
 * two sources — a journal replay owed, and a primary recovered from a backup —
 * so it is checked on super.read_only rather than on either cause, which is what
 * keeps a backup-mounted volume from becoming writable by omission. */
static void test_a_read_only_volume_refuses_to_allocate(void) {
    wfs_alloc_ctx_t a;
    uint32_t before;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    g_vol.super.read_only = 1u;
    before = wfs_stub_req_count;

    expect_rc((wasmos_error_code_t)run_alloc(&a, 1u, 0u),
              WASMOS_ERR_FS_READ_ONLY,
              "a read-only volume refuses the allocation");
    expect_u32(a.length, 0u, "and hands out nothing");
    expect_u32(wfs_stub_req_count, before, "without reading or writing a block");

    wfs_stub_teardown();
}

/* Metadata must never be handed out. mkfs marks every metadata block, the root's
 * data, and any backup superblock as allocated, so an allocator that respects the
 * bitmap cannot return one — this checks the property rather than the mechanism,
 * because it is the property a corrupted volume would violate. */
static void test_allocation_never_returns_a_metadata_block(void) {
    wfs_alloc_ctx_t a;
    uint32_t round;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (round = 0; round < 8u; ++round) {
        if (run_alloc(&a, 2u, 0u) != 0) {
            break;
        }
        expect(a.first_block >= g_layout.first_data_block,
               "an allocated run starts past the metadata regions");
    }

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_an_allocation_marks_the_bitmap),
    WASMOS_TEST_CASE(test_two_allocations_do_not_overlap),
    WASMOS_TEST_CASE(test_the_free_counters_follow_the_bitmap),
    WASMOS_TEST_CASE(test_an_oversized_request_returns_a_shorter_run),
    WASMOS_TEST_CASE(test_a_full_volume_reports_no_space),
    WASMOS_TEST_CASE(test_a_read_only_volume_refuses_to_allocate),
    WASMOS_TEST_CASE(test_allocation_never_returns_a_metadata_block),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_alloc: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_alloc: %d checks passed\n", g_checks);
    return 0;
}
