/* Host unit test for WFS truncation (wfs_truncate.h, §16).
 *
 * Verified through the READ path and by re-reading the object record, for the
 * same reason the write suite is: a size field that agrees with a wrong extent
 * map is not evidence. Freed blocks are checked against the BITMAP, which is the
 * authority the free counters derive from.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_bitmap.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_read.h"
#include "wfs_super.h"
#include "wfs_truncate.h"
#include "wfs_write.h"

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

static void expect_u64(uint64_t got, uint64_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %llu, want %llu\n",
               what,
               (unsigned long long)got,
               (unsigned long long)want);
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
#define TRUNC_NOW_NS 1770000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

#define SMALL_SIZE 32u
#define BIG_SIZE 9000u

static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;
static uint32_t g_small_id;
static uint32_t g_big_id;
static uint8_t g_big_src[BIG_SIZE];

static uint8_t pattern(uint32_t seed, uint64_t at) {
    return (uint8_t)(seed * 31u + (uint32_t)(at & 0xFFu) + (uint32_t)((at >> 8) & 0xFFu));
}

static int read_small(void* c, uint64_t off, void* dst, uint32_t len) {
    uint32_t i;
    (void)c;
    for (i = 0; i < len; ++i) {
        ((uint8_t*)dst)[i] = pattern(1u, off + i);
    }
    return 0;
}

static int read_big(void* c, uint64_t off, void* dst, uint32_t len) {
    uint32_t i;
    (void)c;
    for (i = 0; i < len; ++i) {
        ((uint8_t*)dst)[i] = pattern(2u, off + i);
    }
    return 0;
}

static int setup(void) {
    static wfs_mkfs_entry_t entries[2];
    static wfs_mkfs_node_t plan[2];
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mount_ctx_t m;
    wasmos_wasm_coroutine_t task;
    uint32_t i;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return -1;
    }
    memset(entries, 0, sizeof(entries));
    memset(plan, 0, sizeof(plan));
    entries[0].name = "small";
    entries[0].name_len = 5u;
    entries[0].parent = WFS_MKFS_ROOT;
    entries[0].mode = 0644u;
    entries[0].size = SMALL_SIZE;
    entries[0].read = read_small;
    entries[1].name = "big";
    entries[1].name_len = 3u;
    entries[1].parent = WFS_MKFS_ROOT;
    entries[1].mode = 0644u;
    entries[1].size = BIG_SIZE;
    entries[1].read = read_big;

    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    params.now_ns = TEST_NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = 0;
    sink.write_block = wfs_stub_sink_write;
    if (wfs_mkfs_format_tree(&params, entries, 2u, plan, &sink, &g_layout) != WASMOS_ERR_NONE) {
        expect(0, "format a volume with two files");
        return -1;
    }
    g_small_id = plan[0].object_id;
    g_big_id = plan[1].object_id;
    for (i = 0; i < BIG_SIZE; ++i) {
        g_big_src[i] = pattern(2u, i);
    }

    memset(&m, 0, sizeof(m));
    memset(&g_vol, 0, sizeof(g_vol));
    m.vol = &g_vol;
    if (wfs_stub_run_task(&task, wfs_mount_task, &m) != 0) {
        expect(0, "mount the volume");
        return -1;
    }
    return 0;
}

static int32_t load_object(wfs_object_ctx_t* o, uint32_t id) {
    wasmos_wasm_coroutine_t task;

    memset(o, 0, sizeof(*o));
    o->vol = &g_vol;
    o->object_id = id;
    return wfs_stub_run_task(&task, wfs_object_task, o);
}

static int32_t do_truncate(uint32_t id, uint64_t new_size) {
    wfs_object_ctx_t o;
    wfs_trunc_ctx_t t;
    wasmos_wasm_coroutine_t task;

    if (load_object(&o, id) != 0) {
        return -1;
    }
    memset(&t, 0, sizeof(t));
    wfs_truncate_init(&t, &g_vol, id, &o.out, o.inline_data, new_size, TRUNC_NOW_NS);
    return wfs_stub_run_task(&task, wfs_truncate_task, &t);
}

static int32_t do_read(uint32_t id, uint64_t offset, uint8_t* dst, uint32_t len,
                       uint32_t* out_done) {
    wfs_object_ctx_t o;
    wfs_read_ctx_t r;
    wasmos_wasm_coroutine_t task;
    int32_t rc;

    if (load_object(&o, id) != 0) {
        return -1;
    }
    memset(&r, 0, sizeof(r));
    wfs_read_init(&r, &g_vol, &o.out, o.inline_data, offset, dst, len);
    rc = wfs_stub_run_task(&task, wfs_read_task, &r);
    if (out_done) {
        *out_done = r.done;
    }
    return rc;
}

static const uint8_t* group_bitmap(uint32_t group) {
    return wfs_stub_image + (size_t)(g_layout.bitmap_start + 2u * group) * wfs_stub_block_size;
}

static uint32_t diff_count(const uint8_t* a, const uint8_t* b, uint32_t len) {
    uint32_t n = 0u;
    uint32_t i;

    for (i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            n++;
        }
    }
    return n;
}

/* ---- shrinking ---------------------------------------------------------- */

/* Shrinking to a block boundary drops whole blocks and releases them. */
static void test_shrinking_frees_the_blocks_it_drops(void) {
    wfs_object_ctx_t before;
    wfs_object_ctx_t after;
    uint32_t dropped;
    uint32_t i;
    uint32_t still_set = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(load_object(&before, g_big_id) == 0, "the record reads");
    expect_u32(before.out.extent_count, 1u, "the file has one extent");
    /* 9000 bytes is three blocks; keeping 4096 keeps one. */
    dropped = (uint32_t)before.out.extents[0].physical_block + 1u;

    expect(do_truncate(g_big_id, 4096u) == 0, "the truncation completes");

    expect(load_object(&after, g_big_id) == 0, "the record reads back");
    expect_u64(after.out.size, 4096u, "the size is the new one");
    expect_u32(after.out.extents[0].length, 1u, "the extent shrank to one block");
    expect_u64(after.out.mtime, TRUNC_NOW_NS, "and mtime advanced");

    for (i = 0; i < 2u; ++i) {
        if (wfs_bitmap_test(group_bitmap(0u), dropped + i)) {
            still_set++;
        }
    }
    expect_u32(still_set, 0u, "the dropped blocks are free in the bitmap");

    wfs_stub_teardown();
}

/* Truncating to zero drops the map entirely. */
static void test_truncating_to_zero_drops_every_extent(void) {
    wfs_object_ctx_t o;
    uint32_t done = 0u;
    uint8_t back[16];

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(do_truncate(g_big_id, 0u) == 0, "the truncation completes");
    expect(load_object(&o, g_big_id) == 0, "the record reads back");
    expect_u64(o.out.size, 0u, "the size is zero");
    expect_u32(o.out.extent_count, 0u, "and no extent remains");

    expect(do_read(g_big_id, 0u, back, (uint32_t)sizeof(back), &done) == 0, "a read completes");
    expect_u32(done, 0u, "delivering nothing");

    wfs_stub_teardown();
}

/* The bytes before the new end must survive untouched. A truncation that trimmed
 * one block too many would still report the right size. */
static void test_shrinking_keeps_the_bytes_it_kept(void) {
    uint8_t back[4096];
    uint32_t done = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(do_truncate(g_big_id, 4000u) == 0, "the truncation completes");

    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, 0u, back, 4000u, &done) == 0, "the file reads back");
    expect_u32(done, 4000u, "in full");
    expect_u32(diff_count(back, g_big_src, 4000u), 0u, "the surviving bytes are unchanged");

    wfs_stub_teardown();
}

/* Shrinking then growing must NOT resurrect the removed bytes. The block the new
 * end falls inside is still allocated, so its tail has to be zeroed: otherwise a
 * grow reads back content the truncation was supposed to have removed. */
static void test_a_shrink_then_grow_reads_zeroes_not_old_content(void) {
    uint8_t back[2048];
    uint32_t done = 0u;
    uint32_t nonzero = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    /* 2000 is inside the first block, so that block stays allocated. */
    expect(do_truncate(g_big_id, 2000u) == 0, "the shrink completes");
    expect(do_truncate(g_big_id, 3000u) == 0, "the grow completes");

    memset(back, 0xFFu, sizeof(back));
    expect(do_read(g_big_id, 2000u, back, 1000u, &done) == 0, "the regrown range reads");
    expect_u32(done, 1000u, "delivering the requested bytes");
    for (i = 0; i < 1000u; ++i) {
        if (back[i] != 0u) {
            nonzero++;
        }
    }
    expect_u32(nonzero, 0u, "the regrown range reads as zeroes, not as the old content");

    wfs_stub_teardown();
}

/* ---- growing ------------------------------------------------------------ */

/* Growing allocates nothing: the new range is a hole and reads as zeroes, so a
 * grown file is sparse until something writes into it. */
static void test_growing_allocates_nothing_and_reads_zeroes(void) {
    wfs_object_ctx_t o;
    uint32_t free_before;
    uint8_t back[4096];
    uint32_t done = 0u;
    uint32_t nonzero = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    free_before = g_vol.super.free_blocks;

    expect(do_truncate(g_big_id, BIG_SIZE + 8192u) == 0, "the grow completes");
    expect(load_object(&o, g_big_id) == 0, "the record reads back");
    expect_u64(o.out.size, BIG_SIZE + 8192u, "the size grew");
    expect_u32(g_vol.super.free_blocks, free_before, "no block was allocated");

    memset(back, 0xFFu, sizeof(back));
    expect(do_read(g_big_id, BIG_SIZE + 100u, back, 2048u, &done) == 0, "the new range reads");
    expect_u32(done, 2048u, "delivering the requested bytes");
    for (i = 0; i < 2048u; ++i) {
        if (back[i] != 0u) {
            nonzero++;
        }
    }
    expect_u32(nonzero, 0u, "and it reads as zeroes");

    wfs_stub_teardown();
}

/* Truncating to the size it already has changes nothing and must not free. */
static void test_truncating_to_the_same_size_is_a_no_op(void) {
    wfs_object_ctx_t o;
    uint32_t free_before;
    uint8_t back[BIG_SIZE];
    uint32_t done = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    free_before = g_vol.super.free_blocks;
    expect(do_truncate(g_big_id, BIG_SIZE) == 0, "the truncation completes");
    expect_u32(g_vol.super.free_blocks, free_before, "nothing was freed");

    expect(load_object(&o, g_big_id) == 0, "the record reads back");
    expect_u64(o.out.size, BIG_SIZE, "the size is unchanged");
    expect_u32(o.out.extent_count, 1u, "and so is the extent map");

    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, 0u, back, BIG_SIZE, &done) == 0, "the file reads back");
    expect_u32(diff_count(back, g_big_src, BIG_SIZE), 0u, "with its content intact");

    wfs_stub_teardown();
}

/* ---- inline objects ----------------------------------------------------- */

static void test_shrinking_an_inline_object_clears_its_tail(void) {
    wfs_object_ctx_t o;
    uint8_t back[SMALL_SIZE];
    uint32_t done = 0u;
    uint32_t nonzero = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(do_truncate(g_small_id, 8u) == 0, "the truncation completes");
    expect(load_object(&o, g_small_id) == 0, "the record reads back");
    expect_u64(o.out.size, 8u, "the size is the new one");
    expect(o.out.flags & WFS_OBJ_INLINE_DATA, "and it is still inline");

    /* Grow it back: the bytes between must be zeroes, not the old content. */
    expect(do_truncate(g_small_id, 24u) == 0, "the grow completes");
    memset(back, 0xFFu, sizeof(back));
    expect(do_read(g_small_id, 8u, back, 16u, &done) == 0, "the regrown range reads");
    expect_u32(done, 16u, "delivering the requested bytes");
    for (i = 0; i < 16u; ++i) {
        if (back[i] != 0u) {
            nonzero++;
        }
    }
    expect_u32(nonzero, 0u, "the regrown inline range reads as zeroes");

    wfs_stub_teardown();
}

/* An inline object grown past its 144 bytes needs the promotion the writer does
 * not have either, so it is refused rather than half-done. */
static void test_growing_an_inline_object_past_its_area_is_refused(void) {
    wfs_object_ctx_t o;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc((wasmos_error_code_t)do_truncate(g_small_id, WFS_INLINE_DATA_MAX + 1u),
              WASMOS_ERR_FS_UNSUPPORTED,
              "growing an inline object past its area is refused");
    expect(load_object(&o, g_small_id) == 0, "the record reads back");
    expect_u64(o.out.size, SMALL_SIZE, "with its size untouched");

    wfs_stub_teardown();
}

/* ---- refusals ----------------------------------------------------------- */

static void test_a_read_only_volume_refuses_to_truncate(void) {
    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    g_vol.super.read_only = 1u;
    expect_rc((wasmos_error_code_t)do_truncate(g_big_id, 0u),
              WASMOS_ERR_FS_READ_ONLY,
              "a read-only volume refuses the truncation");

    wfs_stub_teardown();
}

static void test_truncating_a_directory_is_refused(void) {
    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc((wasmos_error_code_t)do_truncate(WFS_OBJECT_ROOT, 0u),
              WASMOS_ERR_FS_IS_DIR,
              "truncating a directory is refused");

    wfs_stub_teardown();
}

/* Freed blocks must become allocatable again, which is the point of freeing them
 * and the property a bitmap-only update would fake. */
static void test_freed_blocks_can_be_allocated_again(void) {
    wfs_object_ctx_t o;
    uint32_t free_before;
    uint32_t free_after;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(load_object(&o, g_big_id) == 0, "the record reads");
    free_before = g_vol.super.free_blocks;

    expect(do_truncate(g_big_id, 0u) == 0, "the truncation completes");
    free_after = g_vol.super.free_blocks;
    expect(free_after > free_before, "the free count grew by what was released");
    expect_u32(free_after - free_before, 3u, "which is the three blocks the file held");

    wfs_stub_teardown();
}

/* ---- extent trees ------------------------------------------------------- */

static int32_t do_write(uint32_t id, uint64_t offset, const uint8_t* src, uint32_t len) {
    wfs_object_ctx_t o;
    wfs_write_ctx_t w;
    wasmos_wasm_coroutine_t task;

    if (load_object(&o, id) != 0) {
        return -1;
    }
    memset(&w, 0, sizeof(w));
    wfs_write_init(&w, &g_vol, id, &o.out, o.inline_data, offset, src, len, TRUNC_NOW_NS);
    return wfs_stub_run_task(&task, wfs_write_task, &w);
}

/* Blocks marked used in the volume's only group, so a release can be checked as
 * a return to a baseline rather than against block numbers the test would have
 * to predict. A 16 MiB volume at 4096 bytes is 4096 blocks and one group. */
static uint32_t used_blocks(void) {
    const uint8_t* bm = group_bitmap(0u);
    uint32_t n = 0u;
    uint32_t i;
    uint32_t bit;

    for (i = 0; i < wfs_stub_block_size; ++i) {
        for (bit = 0; bit < 8u; ++bit) {
            if (bm[i] & (uint8_t)(1u << bit)) {
                n++;
            }
        }
    }
    return n;
}

/* One block at `logical`, far from its neighbours so it is a new extent. */
static int32_t write_sparse_block(uint32_t id, uint64_t logical) {
    uint8_t buf[64];
    uint32_t i;

    for (i = 0; i < sizeof(buf); ++i) {
        buf[i] = pattern(11u, logical * 4096u + i);
    }
    return do_write(id, logical * 4096u, buf, (uint32_t)sizeof(buf));
}

/* Empty `big` first, then lay SEVEN sparse runs at logical 10,20,...,70 -- one
 * past the inline limit, so the object takes a tree.
 *
 * Emptying it first is what makes the block accounting meaningful: *out_baseline
 * is the volume with this object owning nothing, so a later return to it proves
 * the tree's runs AND its leaf were released and nothing else was. */
static int build_tree(uint32_t* out_baseline) {
    wfs_object_ctx_t o;
    uint32_t i;

    if (do_truncate(g_big_id, 0u) != 0) {
        expect(0, "the object empties");
        return -1;
    }
    *out_baseline = used_blocks();
    for (i = 1u; i <= 7u; ++i) {
        if (write_sparse_block(g_big_id, i * 10u) != 0) {
            expect(0, "a sparse block is written");
            return -1;
        }
    }
    if (load_object(&o, g_big_id) != 0 || o.out.extent_tree_block == 0u) {
        expect(0, "the object has an extent tree");
        return -1;
    }
    return 0;
}

/* Truncating a tree-mapped object to zero releases its data AND its leaf, and
 * puts the object back on an inline map.
 *
 * Before the extent-tree writer existed this could not arise; with a writer and
 * no trim it would leave a file that cannot be emptied and whose blocks nothing
 * reclaims. */
static void test_truncating_a_tree_mapped_file_to_zero_frees_everything(void) {
    wfs_object_ctx_t o;
    uint32_t baseline;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    if (build_tree(&baseline) != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(used_blocks() > baseline, "the tree and its runs consumed blocks");

    expect(do_truncate(g_big_id, 0u) == 0, "the truncation completes");
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect_u64(o.out.size, 0u, "the file is empty");
    expect_u32(o.out.extent_count, 0u, "no extent is left");
    expect_u64(o.out.extent_tree_block, 0u, "and the map is inline again");
    /* The leaf block is included: a trim that freed only the data would leave
     * this one block short of the baseline forever. */
    expect_u32(used_blocks(), baseline, "every block came back, the leaf included");

    wfs_stub_teardown();
}

/* A partial shrink keeps the runs below the cut and drops the rest. Block
 * aligned, because zeroing a tail inside a block needs a descent the truncate
 * task cannot yet make. */
static void test_shrinking_a_tree_mapped_file_keeps_what_is_below_the_cut(void) {
    wfs_object_ctx_t o;
    uint8_t back[64];
    uint8_t want[64];
    uint32_t done = 0u;
    uint32_t baseline = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    if (build_tree(&baseline) != 0) {
        wfs_stub_teardown();
        return;
    }
    /* Runs sit at logical 10,20,...,70. Cutting at 35 blocks keeps 10,20,30. */
    expect(do_truncate(g_big_id, (uint64_t)35u * 4096u) == 0, "the shrink completes");
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect_u64(o.out.size, 35u * 4096u, "the size is the new one");
    expect(o.out.extent_tree_block != 0u, "the tree survives a partial shrink");
    expect_u32(o.out.extent_count, 3u, "only the runs below the cut are left");

    for (i = 0; i < sizeof(want); ++i) {
        want[i] = pattern(11u, (uint64_t)30u * 4096u + i);
    }
    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, (uint64_t)30u * 4096u, back, (uint32_t)sizeof(back), &done) == 0,
           "a surviving run reads");
    expect_u32(diff_count(back, want, (uint32_t)sizeof(want)), 0u, "and its bytes are intact");

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_shrinking_frees_the_blocks_it_drops),
    WASMOS_TEST_CASE(test_truncating_to_zero_drops_every_extent),
    WASMOS_TEST_CASE(test_shrinking_keeps_the_bytes_it_kept),
    WASMOS_TEST_CASE(test_a_shrink_then_grow_reads_zeroes_not_old_content),
    WASMOS_TEST_CASE(test_growing_allocates_nothing_and_reads_zeroes),
    WASMOS_TEST_CASE(test_truncating_to_the_same_size_is_a_no_op),
    WASMOS_TEST_CASE(test_shrinking_an_inline_object_clears_its_tail),
    WASMOS_TEST_CASE(test_growing_an_inline_object_past_its_area_is_refused),
    WASMOS_TEST_CASE(test_a_read_only_volume_refuses_to_truncate),
    WASMOS_TEST_CASE(test_truncating_a_directory_is_refused),
    WASMOS_TEST_CASE(test_freed_blocks_can_be_allocated_again),
    WASMOS_TEST_CASE(test_truncating_a_tree_mapped_file_to_zero_frees_everything),
    WASMOS_TEST_CASE(test_shrinking_a_tree_mapped_file_keeps_what_is_below_the_cut),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_truncate: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_truncate: %d checks passed\n", g_checks);
    return 0;
}
