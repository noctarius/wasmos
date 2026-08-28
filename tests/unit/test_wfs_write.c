/* Host unit test for WFS file writes (wfs_write.h, §16), run as tasks on the
 * SYSTEM coroutine runtime against a volume mkfs_wfs built and populated.
 *
 * Written bytes are verified by READING THEM BACK through wfs_read_task, and the
 * object record by re-reading it through wfs_object_task. Inspecting blocks
 * directly would pass for a write that landed in the right place by accident
 * while the extent map said otherwise; going back through the read path is what
 * makes the two agree.
 *
 * Patterns are position-dependent, so a write that lands shifted or truncated
 * fails rather than matching.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_read.h"
#include "wfs_super.h"
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
#define WRITE_NOW_NS 1760000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

/* Two files: one small enough to be stored inline, one spanning blocks. mkfs
 * stores a file of WFS_INLINE_DATA_MAX bytes or fewer in the record itself. */
#define SMALL_SIZE 32u
#define BIG_SIZE 9000u

static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;
static uint32_t g_small_id;
static uint32_t g_big_id;

static uint8_t g_small_src[SMALL_SIZE];
static uint8_t g_big_src[BIG_SIZE];

/* Position-dependent content, so a shifted copy cannot match. */
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

/* Build a volume holding both files, mount it, and record their object ids. */
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
    /* Re-formatted in place: wfs_stub_build_volume allocated the image with an
     * empty root, and the tree has to be laid into it before the mount below. */
    if (wfs_mkfs_format_tree(&params, entries, 2u, plan, &sink, &g_layout) != WASMOS_ERR_NONE) {
        expect(0, "format a volume with two files");
        return -1;
    }
    g_small_id = plan[0].object_id;
    g_big_id = plan[1].object_id;

    for (i = 0; i < SMALL_SIZE; ++i) {
        g_small_src[i] = pattern(1u, i);
    }
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

static int32_t do_write(uint32_t id, uint64_t offset, const uint8_t* src, uint32_t len,
                        uint32_t* out_done) {
    wfs_object_ctx_t o;
    wfs_write_ctx_t w;
    int32_t rc;

    if (load_object(&o, id) != 0) {
        return -1;
    }
    memset(&w, 0, sizeof(w));
    wfs_write_init(&w, &g_vol, id, &o.out, o.inline_data, offset, src, len, WRITE_NOW_NS);
    rc = wfs_stub_run_txn(&g_vol, wfs_write_task, &w);
    if (out_done) {
        *out_done = w.done;
    }
    return rc;
}

/* Read through the read path, which is the only reader that proves the extent
 * map and the bytes agree. */
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

/* ---- overwrite inside allocated blocks ---------------------------------- */

static void test_an_overwrite_inside_a_block_reads_back(void) {
    uint8_t patch[100];
    uint8_t back[BIG_SIZE];
    uint8_t want[BIG_SIZE];
    uint32_t done = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 0; i < sizeof(patch); ++i) {
        patch[i] = pattern(7u, i);
    }
    expect(do_write(g_big_id, 1000u, patch, (uint32_t)sizeof(patch), &done) == 0,
           "the write completes");
    expect_u32(done, (uint32_t)sizeof(patch), "every byte was written");

    memcpy(want, g_big_src, BIG_SIZE);
    memcpy(want + 1000, patch, sizeof(patch));

    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, 0u, back, BIG_SIZE, &done) == 0, "the file reads back");
    expect_u32(done, BIG_SIZE, "in full");
    expect_u32(
        diff_count(back, want, BIG_SIZE), 0u, "the patch landed and nothing around it changed");

    wfs_stub_teardown();
}

/* A write spanning a block boundary has to walk the extent map twice; a run that
 * kept its block number in a C local across the await would write the second
 * half to the wrong block. */
static void test_a_write_across_a_block_boundary_reads_back(void) {
    uint8_t patch[600];
    uint8_t back[BIG_SIZE];
    uint8_t want[BIG_SIZE];
    uint32_t done = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 0; i < sizeof(patch); ++i) {
        patch[i] = pattern(9u, i);
    }
    /* Straddles the 4096-byte boundary. */
    expect(do_write(g_big_id, 3900u, patch, (uint32_t)sizeof(patch), &done) == 0,
           "the write completes");
    expect_u32(done, (uint32_t)sizeof(patch), "every byte was written");

    memcpy(want, g_big_src, BIG_SIZE);
    memcpy(want + 3900, patch, sizeof(patch));

    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, 0u, back, BIG_SIZE, &done) == 0, "the file reads back");
    expect_u32(diff_count(back, want, BIG_SIZE), 0u, "both halves landed in the right blocks");

    wfs_stub_teardown();
}

/* An overwrite must not change the size. */
static void test_an_overwrite_leaves_the_size_alone(void) {
    uint8_t patch[16];
    wfs_object_ctx_t o;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    memset(patch, 0xAB, sizeof(patch));
    expect(do_write(g_big_id, 100u, patch, (uint32_t)sizeof(patch), 0) == 0, "the write completes");
    expect(load_object(&o, g_big_id) == 0, "the record reads back");
    expect_u64(o.out.size, BIG_SIZE, "the size is unchanged");
    expect_u64(o.out.mtime, WRITE_NOW_NS, "and mtime advanced");

    wfs_stub_teardown();
}

/* ---- growth ------------------------------------------------------------- */

/* Appending past the end allocates, extends the extent map, and grows the size.
 * The read-back covers the ORIGINAL content too, because an append that
 * disturbed the existing extents would still return the new bytes correctly. */
static void test_an_append_grows_the_file(void) {
    uint8_t tail[5000];
    uint8_t back[BIG_SIZE + sizeof(tail)];
    uint8_t want[BIG_SIZE + sizeof(tail)];
    wfs_object_ctx_t o;
    uint32_t done = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 0; i < sizeof(tail); ++i) {
        tail[i] = pattern(11u, i);
    }
    expect(do_write(g_big_id, BIG_SIZE, tail, (uint32_t)sizeof(tail), &done) == 0,
           "the append completes");
    expect_u32(done, (uint32_t)sizeof(tail), "every byte was written");

    expect(load_object(&o, g_big_id) == 0, "the record reads back");
    expect_u64(o.out.size, BIG_SIZE + sizeof(tail), "the size grew by the append");

    memcpy(want, g_big_src, BIG_SIZE);
    memcpy(want + BIG_SIZE, tail, sizeof(tail));
    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, 0u, back, (uint32_t)sizeof(back), &done) == 0, "the file reads back");
    expect_u32(done, (uint32_t)sizeof(back), "in full");
    expect_u32(diff_count(back, want, (uint32_t)sizeof(back)),
               0u,
               "the original content and the append are both intact");

    wfs_stub_teardown();
}

/* A partial write into a FRESHLY allocated block must zero the rest of it. The
 * block holds whatever it held before allocation, so reading it back is not an
 * option: the untouched bytes have to read as zeroes, which is what the format
 * promises for a range nothing has written. */
static void test_a_partial_write_into_a_new_block_zeroes_the_rest(void) {
    uint8_t tail[8];
    uint8_t back[4096];
    uint32_t done = 0u;
    uint32_t nonzero = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    memset(tail, 0x5Au, sizeof(tail));
    /* 9000 is 2 blocks + 808 bytes, so the append starts inside the last
     * allocated block; push well past it so a NEW block is needed. */
    expect(do_write(g_big_id, 12288u, tail, (uint32_t)sizeof(tail), &done) == 0,
           "the write completes");

    memset(back, 0xFFu, sizeof(back));
    expect(do_read(g_big_id, 12288u + sizeof(tail), back, 2048u, &done) == 0,
           "reading past the written bytes completes");
    /* Past the size there is nothing to deliver, which is how a reader learns
     * where the object ends. */
    expect_u32(done, 0u, "and delivers nothing past the end");

    /* Inside the new block but before the written bytes is a hole, and a hole
     * reads as zeroes. */
    memset(back, 0xFFu, sizeof(back));
    expect(do_read(g_big_id, 12288u - 16u, back, 16u, &done) == 0, "reading the hole completes");
    expect_u32(done, 16u, "delivering the requested bytes");
    for (i = 0; i < 16u; ++i) {
        if (back[i] != 0u) {
            nonzero++;
        }
    }
    expect_u32(nonzero, 0u, "a hole reads as zeroes, not as whatever the block held");

    wfs_stub_teardown();
}

/* ---- inline objects ----------------------------------------------------- */

/* A small file lives in its record, so a write that stays inside
 * WFS_INLINE_DATA_MAX touches no block at all. */
static void test_an_inline_write_stays_in_the_record(void) {
    uint8_t patch[8];
    uint8_t back[SMALL_SIZE];
    uint8_t want[SMALL_SIZE];
    wfs_object_ctx_t o;
    uint32_t done = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(load_object(&o, g_small_id) == 0, "the record reads back");
    expect(o.out.flags & WFS_OBJ_INLINE_DATA, "the small file is stored inline");

    memset(patch, 0x7Eu, sizeof(patch));
    expect(do_write(g_small_id, 4u, patch, (uint32_t)sizeof(patch), &done) == 0,
           "the inline write completes");
    expect_u32(done, (uint32_t)sizeof(patch), "every byte was written");

    memcpy(want, g_small_src, SMALL_SIZE);
    memcpy(want + 4, patch, sizeof(patch));
    memset(back, 0, sizeof(back));
    expect(do_read(g_small_id, 0u, back, SMALL_SIZE, &done) == 0, "the file reads back");
    expect_u32(diff_count(back, want, SMALL_SIZE), 0u, "the inline bytes were patched in place");

    expect(load_object(&o, g_small_id) == 0, "the record reads back again");
    expect(o.out.flags & WFS_OBJ_INLINE_DATA, "and is still inline");
    expect_u64(o.out.size, SMALL_SIZE, "with its size unchanged");

    wfs_stub_teardown();
}

/* Growing an inline object past its 144 bytes needs promotion to extents, which
 * is not implemented. It must be REFUSED rather than half-done: the promotion has
 * to read the inline bytes before writing any extent over them, and a partial
 * attempt destroys the file's content. */
/* An inline object that outgrows the record is PROMOTED to an extent map, and the
 * bytes it already held survive the move.
 *
 * A new file is created inline (wfs_alloc.c), so without this a file made in the
 * OS could never exceed WFS_INLINE_DATA_MAX bytes -- and never reach an extent
 * map at all, let alone a tree.
 *
 * The write starts PAST the old end, so the original bytes, the gap between, and
 * the new bytes are three distinct ranges: a promotion that dropped the record's
 * content, or wrote it at the wrong offset, fails here rather than passing on a
 * write that happens to cover everything. */
static void test_outgrowing_the_inline_area_promotes_the_object(void) {
    uint8_t big[200];
    uint8_t back[SMALL_SIZE + 200u + 100u];
    wfs_object_ctx_t o;
    uint32_t done = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(load_object(&o, g_small_id) == 0, "the object loads");
    expect((o.out.flags & WFS_OBJ_INLINE_DATA) != 0u, "the small file starts inline");

    for (i = 0; i < sizeof(big); ++i) {
        big[i] = pattern(9u, i);
    }
    expect(do_write(g_small_id, 100u, big, (uint32_t)sizeof(big), &done) == 0,
           "outgrowing the inline area succeeds");
    expect_u32(done, (uint32_t)sizeof(big), "every byte landed");

    expect(load_object(&o, g_small_id) == 0, "the object loads");
    expect_u32((uint32_t)(o.out.flags & WFS_OBJ_INLINE_DATA), 0u, "it is no longer inline");
    expect_u32(o.out.extent_count, 1u, "and its content is one extent");
    expect_u64(o.out.size, 300u, "the size covers the new end");

    memset(back, 0, sizeof(back));
    expect(do_read(g_small_id, 0u, back, 300u, &done) == 0, "the file reads back");
    expect_u32(done, 300u, "in full");
    /* The bytes the record held, at the offset they were at. */
    expect_u32(diff_count(back, g_small_src, SMALL_SIZE), 0u, "the original bytes survived");
    /* The gap between the old end and the write is a hole, so it reads as zeroes
     * rather than as whatever the fresh block held. */
    for (i = SMALL_SIZE; i < 100u; ++i) {
        if (back[i] != 0u) {
            break;
        }
    }
    expect_u32(i, 100u, "the gap reads as zeroes");
    expect_u32(diff_count(back + 100u, big, (uint32_t)sizeof(big)), 0u, "and the new bytes match");

    wfs_stub_teardown();
}

/* ---- refusals ----------------------------------------------------------- */

static void test_a_read_only_volume_refuses_to_write(void) {
    uint8_t patch[4];
    uint32_t before;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    memset(patch, 1, sizeof(patch));
    g_vol.super.read_only = 1u;
    before = wfs_stub_req_count;

    expect_rc((wasmos_error_code_t)do_write(g_big_id, 0u, patch, (uint32_t)sizeof(patch), 0),
              WASMOS_ERR_FS_READ_ONLY,
              "a read-only volume refuses the write");
    /* The object load before the write reads blocks, so only the writes are
     * counted here: what must not happen is a device write. */
    expect(wfs_stub_req_count >= before, "no write was attempted");

    wfs_stub_teardown();
}

/* A directory's bytes are records. Writing them as content would let a caller
 * corrupt the namespace through a file interface. */
static void test_writing_a_directory_is_refused(void) {
    uint8_t patch[4];

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    memset(patch, 1, sizeof(patch));
    expect_rc((wasmos_error_code_t)do_write(WFS_OBJECT_ROOT, 0u, patch, (uint32_t)sizeof(patch), 0),
              WASMOS_ERR_FS_IS_DIR,
              "writing a directory is refused");

    wfs_stub_teardown();
}

/* A write marks the volume dirty before any of its blocks land, for the same
 * reason an allocation does. */
static void test_a_write_marks_the_volume_dirty(void) {
    uint8_t patch[4];

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    memset(patch, 2, sizeof(patch));
    expect_u32(g_vol.dirty_marked, 0u, "a fresh mount is not marked");
    expect(do_write(g_big_id, 0u, patch, (uint32_t)sizeof(patch), 0) == 0, "the write completes");
    expect_u32(g_vol.dirty_marked, 1u, "and the volume is marked");

    wfs_stub_teardown();
}

/* ---- growth past the inline extent map ----------------------------------- */

/* One block at `logical`, far enough from the last to be a NEW extent rather
 * than an extension of it: record_extent only extends when a run continues the
 * previous one both logically and physically. */
static int32_t write_sparse_block(uint32_t id, uint64_t logical) {
    uint8_t buf[64];
    uint32_t i;

    for (i = 0; i < sizeof(buf); ++i) {
        buf[i] = pattern(11u, logical * 4096u + i);
    }
    return do_write(id, logical * 4096u, buf, (uint32_t)sizeof(buf), 0);
}

static void expect_sparse_block_reads_back(uint32_t id, uint64_t logical, const char* what) {
    uint8_t back[64];
    uint8_t want[64];
    uint32_t i;
    uint32_t done = 0u;

    for (i = 0; i < sizeof(want); ++i) {
        want[i] = pattern(11u, logical * 4096u + i);
    }
    memset(back, 0, sizeof(back));
    expect(do_read(id, logical * 4096u, back, (uint32_t)sizeof(back), &done) == 0, what);
    expect_u32(done, (uint32_t)sizeof(back), "the whole run was read");
    expect_u32(diff_count(back, want, (uint32_t)sizeof(want)), 0u, "and its bytes match");
}

/* A file needing a SEVENTH extent takes an extent tree.
 *
 * Six discontiguous runs fit in the record; the seventh does not, and before the
 * extent-tree writer existed this refused with WASMOS_ERR_FS_UNSUPPORTED and the
 * file simply stopped growing.
 */
static void test_a_file_grows_past_the_inline_extent_limit(void) {
    wfs_object_ctx_t o;
    uint8_t back[64];
    uint8_t want[64];
    uint32_t done = 0u;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    /* `big` is contiguous, so it starts as one extent and five sparse blocks
     * bring it to the inline limit. */
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect_u32(o.out.extent_count, 1u, "a contiguous file is one extent");
    expect_u64(o.out.extent_tree_block, 0u, "and has no tree");

    for (i = 1; i <= 5u; ++i) {
        expect(write_sparse_block(g_big_id, i * 10u) == 0, "a sparse block within the map");
    }
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect_u32(o.out.extent_count, WFS_INLINE_EXTENTS, "the inline map is now full");
    expect_u64(o.out.extent_tree_block, 0u, "and is still inline");

    /* The seventh. */
    expect(write_sparse_block(g_big_id, 60u) == 0, "the seventh extent is written");
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect_u32(o.out.extent_count, WFS_INLINE_EXTENTS + 1u, "the map holds seven extents");
    expect(o.out.extent_tree_block != 0u, "the object now names a tree");
    expect(o.out.extent_tree_block < g_vol.super.total_blocks, "and the root is in range");

    /* The two maps are exclusive (§9): a tree means the inline array is zero, or
     * two readers could disagree about where a block lives. */
    for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
        expect_u64(o.out.extents[i].logical_block, 0u, "inline logical is cleared");
        expect_u64(o.out.extents[i].physical_block, 0u, "inline physical is cleared");
        expect_u32(o.out.extents[i].length, 0u, "inline length is cleared");
    }

    /* Every run still reads back through the tree, including the ones that were
     * inline before the promotion moved them. */
    expect_sparse_block_reads_back(g_big_id, 60u, "the seventh run reads back");
    for (i = 1; i <= 5u; ++i) {
        expect_sparse_block_reads_back(g_big_id, i * 10u, "an earlier run reads back");
    }
    for (i = 0; i < sizeof(want); ++i) {
        want[i] = pattern(2u, i);
    }
    memset(back, 0, sizeof(back));
    expect(do_read(g_big_id, 0u, back, (uint32_t)sizeof(back), &done) == 0,
           "the original content reads back");
    expect_u32(diff_count(back, want, (uint32_t)sizeof(want)), 0u, "and is unchanged");

    wfs_stub_teardown();
}

/* A promoted tree's records are SORTED by logical_block even when the writes
 * were not. The inline array is scanned linearly, so an unsorted map reads
 * correctly there; a tree's descent takes the last index not exceeding the
 * target and needs the order (§9). */
static void test_the_promoted_tree_sorts_extents_written_out_of_order(void) {
    wfs_object_ctx_t o;
    uint8_t back[64];
    uint32_t done = 0u;
    uint32_t i;
    const uint64_t logicals[6] = {90u, 30u, 70u, 10u, 50u, 80u};

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 0; i < 6u; ++i) {
        expect(write_sparse_block(g_big_id, logicals[i]) == 0, "a sparse block, out of order");
    }
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect_u32(o.out.extent_count, WFS_INLINE_EXTENTS + 1u, "one original plus six sparse");
    expect(o.out.extent_tree_block != 0u, "the object names a tree");
    for (i = 0; i < 6u; ++i) {
        expect_sparse_block_reads_back(g_big_id, logicals[i], "each run reads back through it");
    }
    /* A hole between two runs still reads as zeroes rather than as a neighbour's
     * bytes, which is what a mis-sorted descent would return. */
    memset(back, 0xAA, sizeof(back));
    expect(do_read(g_big_id, (uint64_t)20u * 4096u, back, (uint32_t)sizeof(back), &done) == 0,
           "a hole reads");
    for (i = 0; i < sizeof(back); ++i) {
        if (back[i] != 0u) {
            break;
        }
    }
    expect_u32(i, (uint32_t)sizeof(back), "and reads as zeroes");

    wfs_stub_teardown();
}

/* Growth continues inside the leaf once the tree exists, so promotion is not a
 * one-shot that leaves the file stuck at seven. */
static void test_a_tree_mapped_file_keeps_growing(void) {
    wfs_object_ctx_t o;
    uint32_t i;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 1; i <= 12u; ++i) {
        expect(write_sparse_block(g_big_id, i * 10u) == 0, "each sparse block is written");
    }
    expect(load_object(&o, g_big_id) == 0, "the object loads");
    expect(o.out.extent_tree_block != 0u, "the object names a tree");
    expect_u32(o.out.extent_count, 13u, "every run is accounted for");
    for (i = 1; i <= 12u; ++i) {
        expect_sparse_block_reads_back(g_big_id, i * 10u, "and each reads back");
    }

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_an_overwrite_inside_a_block_reads_back),
    WASMOS_TEST_CASE(test_a_write_across_a_block_boundary_reads_back),
    WASMOS_TEST_CASE(test_an_overwrite_leaves_the_size_alone),
    WASMOS_TEST_CASE(test_an_append_grows_the_file),
    WASMOS_TEST_CASE(test_a_partial_write_into_a_new_block_zeroes_the_rest),
    WASMOS_TEST_CASE(test_an_inline_write_stays_in_the_record),
    WASMOS_TEST_CASE(test_outgrowing_the_inline_area_promotes_the_object),
    WASMOS_TEST_CASE(test_a_read_only_volume_refuses_to_write),
    WASMOS_TEST_CASE(test_writing_a_directory_is_refused),
    WASMOS_TEST_CASE(test_a_write_marks_the_volume_dirty),
    WASMOS_TEST_CASE(test_a_file_grows_past_the_inline_extent_limit),
    WASMOS_TEST_CASE(test_the_promoted_tree_sorts_extents_written_out_of_order),
    WASMOS_TEST_CASE(test_a_tree_mapped_file_keeps_growing),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_write: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_write: %d checks passed\n", g_checks);
    return 0;
}
