/* Host unit test for the WFS read path (src/drivers/fs_wfs/wfs_read.c), driven
 * against volumes mkfs_wfs populated.
 *
 * This is the whole read path end to end: a name resolves to an object, the
 * object's map resolves a logical block, and the bytes come back. What it
 * mainly exists to pin is the slicing — a read that starts mid-block, ends
 * mid-block, spans several, or runs off the end — because every one of those is
 * an offset arithmetic that looks right and is not.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_dir.h"
#include "wfs_extent.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_read.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
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
    0x2c, 0x6f, 0x91, 0x08, 0xe3, 0x4a, 0x47, 0x7d, 0xa9, 0x51, 0x38, 0xbd, 0x12, 0x0c, 0xe5, 0x66};
#define NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)
#define BS 4096u

/* Deterministic content, so any byte of any file is predictable without holding
 * the file. */
static uint8_t content_byte(uint32_t seed, uint64_t offset) {
    return (uint8_t)((seed * 31u + (uint32_t)(offset * 7u) + (uint32_t)(offset >> 8)) & 0xFFu);
}

typedef struct {
    uint32_t seed;
    uint64_t size;
} gen_t;

static int gen_read(void* ctx, uint64_t offset, void* dst, uint32_t len) {
    gen_t* g = (gen_t*)ctx;
    uint8_t* p = (uint8_t*)dst;
    uint32_t i;

    for (i = 0; i < len; ++i) {
        p[i] = (offset + i) < g->size ? content_byte(g->seed, offset + i) : 0u;
    }
    return 0;
}

/* /big  (three blocks and a bit)   /tiny (inline)   /dir */
enum { E_BIG = 0, E_TINY, E_DIR, E_COUNT };

#define BIG_SIZE (3u * BS + 1000u)
#define BIG_SEED 17u
#define TINY_SIZE 100u
#define TINY_SEED 23u

static gen_t g_gen[E_COUNT];
static wfs_mkfs_entry_t g_entries[E_COUNT];
static wfs_mkfs_node_t g_plan[E_COUNT + 1u];
static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;

static void add_file(uint32_t idx, const char* name, uint64_t size, uint32_t seed) {
    memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
    g_entries[idx].name = name;
    g_entries[idx].name_len = (uint32_t)strlen(name);
    g_entries[idx].parent = WFS_MKFS_ROOT;
    g_entries[idx].mode = 0644u;
    g_entries[idx].size = size;
    g_gen[idx].seed = seed;
    g_gen[idx].size = size;
    g_entries[idx].read = gen_read;
    g_entries[idx].read_ctx = &g_gen[idx];
}

static int setup(void) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mount_ctx_t ctx;
    wasmos_wasm_coroutine_t task;

    add_file(E_BIG, "big", BIG_SIZE, BIG_SEED);
    add_file(E_TINY, "tiny", TINY_SIZE, TINY_SEED);
    memset(&g_entries[E_DIR], 0, sizeof(g_entries[E_DIR]));
    g_entries[E_DIR].name = "dir";
    g_entries[E_DIR].name_len = 3u;
    g_entries[E_DIR].parent = WFS_MKFS_ROOT;
    g_entries[E_DIR].is_dir = 1u;
    g_entries[E_DIR].mode = 0755u;

    if (wfs_stub_build_volume(VOL_16M, BS, k_uuid, NOW_NS, &g_layout) != 0) {
        return -1;
    }
    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = BS;
    params.now_ns = NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;
    if (wfs_mkfs_format_tree(&params, g_entries, E_COUNT, g_plan, &sink, &g_layout) !=
        WASMOS_ERR_NONE) {
        return -1;
    }
    wfs_stub_reset_counters();

    memset(&ctx, 0, sizeof(ctx));
    memset(&g_vol, 0, sizeof(g_vol));
    ctx.vol = &g_vol;
    if (wfs_stub_run_task(&task, wfs_mount_task, &ctx) != 0) {
        return -1;
    }
    return g_vol.mounted ? 0 : -1;
}

/* The object record, plus the inline bytes the read path needs when it has
 * them. */
static wfs_object_ctx_t g_obj;

static int32_t open_object(uint32_t idx) {
    wasmos_wasm_coroutine_t task;

    memset(&g_obj, 0, sizeof(g_obj));
    g_obj.vol = &g_vol;
    g_obj.object_id = g_plan[idx].object_id;
    return wfs_stub_run_task(&task, wfs_object_task, &g_obj);
}

static int32_t do_read(uint64_t offset, uint8_t* dst, uint32_t len, uint32_t* out_done) {
    wfs_read_ctx_t ctx;
    wasmos_wasm_coroutine_t task;
    int32_t rc;

    wfs_read_init(&ctx, &g_vol, &g_obj.out, g_obj.inline_data, offset, dst, len);
    rc = wfs_stub_run_task(&task, wfs_read_task, &ctx);
    if (out_done) {
        *out_done = ctx.done;
    }
    return rc;
}

/* Does `got` hold what the generator would have produced at `offset`? */
static int matches(const uint8_t* got, uint32_t seed, uint64_t offset, uint32_t len,
                   uint64_t size) {
    uint32_t i;

    for (i = 0; i < len; ++i) {
        uint8_t want = (offset + i) < size ? content_byte(seed, offset + i) : 0u;

        if (got[i] != want) {
            return 0;
        }
    }
    return 1;
}

/* ---- whole and partial reads -------------------------------------------- */

static void test_a_whole_multi_block_file_reads_back(void) {
    static uint8_t buf[BIG_SIZE + 16u];
    uint32_t done = 0;

    if (setup() != 0 || open_object(E_BIG) != 0) {
        expect(0, "setup");
        return;
    }
    expect(g_obj.out.size == BIG_SIZE, "the file's size");
    expect(do_read(0u, buf, BIG_SIZE, &done) == 0, "the whole file reads");
    expect(done == BIG_SIZE, "every byte was delivered");
    expect(matches(buf, BIG_SEED, 0u, BIG_SIZE, BIG_SIZE), "and matches what was written");

    wfs_stub_teardown();
}

/* The slicing cases. Each is an offset arithmetic that looks right and is not:
 * a read wholly inside one block, one starting mid-block and ending in the next,
 * one spanning a whole block boundary, and one landing exactly on boundaries. */
static void test_partial_reads_slice_correctly(void) {
    static uint8_t buf[BS * 3u];
    uint32_t done = 0;

    if (setup() != 0 || open_object(E_BIG) != 0) {
        expect(0, "setup");
        return;
    }

    expect(do_read(10u, buf, 100u, &done) == 0 && done == 100u, "wholly inside the first block");
    expect(matches(buf, BIG_SEED, 10u, 100u, BIG_SIZE), "with the right bytes");

    expect(do_read(BS - 50u, buf, 100u, &done) == 0 && done == 100u,
           "straddling the first block boundary");
    expect(matches(buf, BIG_SEED, BS - 50u, 100u, BIG_SIZE), "with the right bytes");

    expect(do_read(BS, buf, BS, &done) == 0 && done == BS,
           "exactly one block, exactly on a boundary");
    expect(matches(buf, BIG_SEED, BS, BS, BIG_SIZE), "with the right bytes");

    expect(do_read(BS - 1u, buf, 2u * BS + 2u, &done) == 0 && done == 2u * BS + 2u,
           "spanning three blocks from one byte before a boundary");
    expect(matches(buf, BIG_SEED, BS - 1u, 2u * BS + 2u, BIG_SIZE), "with the right bytes");

    expect(do_read(0u, buf, 1u, &done) == 0 && done == 1u, "a single byte");
    expect(buf[0] == content_byte(BIG_SEED, 0u), "the first byte of the file");

    /* The last byte, which lives in the file's partial final block. */
    expect(do_read(BIG_SIZE - 1u, buf, 1u, &done) == 0 && done == 1u, "the last byte");
    expect(buf[0] == content_byte(BIG_SEED, BIG_SIZE - 1u), "is the file's last byte");

    wfs_stub_teardown();
}

/* A read running past the end is SHORT, not an error: that is how a reader learns
 * where the object ends. */
static void test_a_read_past_the_end_is_short(void) {
    static uint8_t buf[BS * 2u];
    uint32_t done = 0;

    if (setup() != 0 || open_object(E_BIG) != 0) {
        expect(0, "setup");
        return;
    }
    expect(do_read(BIG_SIZE - 10u, buf, 500u, &done) == 0, "a read overlapping the end succeeds");
    expect(done == 10u, "and delivers only what exists");
    expect(matches(buf, BIG_SEED, BIG_SIZE - 10u, 10u, BIG_SIZE), "with the right bytes");

    expect(do_read(BIG_SIZE, buf, 100u, &done) == 0, "a read starting at the end succeeds");
    expect(done == 0u, "and delivers nothing");

    expect(do_read(BIG_SIZE + 4096u, buf, 100u, &done) == 0, "and well past it too");
    expect(done == 0u, "still nothing");

    /* The final partial block: the file ends mid-block, and the bytes past it in
     * that block must not be handed out. */
    expect(do_read((uint64_t)3u * BS, buf, BS, &done) == 0, "a read of the whole final block");
    expect(done == BIG_SIZE - 3u * BS, "stops at the file's end, not the block's");

    wfs_stub_teardown();
}

static void test_a_zero_length_read_delivers_nothing(void) {
    static uint8_t buf[16];
    uint32_t done = 0;
    uint32_t before;

    if (setup() != 0 || open_object(E_BIG) != 0) {
        expect(0, "setup");
        return;
    }
    before = wfs_stub_reads;
    expect(do_read(0u, buf, 0u, &done) == 0, "a zero-length read succeeds");
    expect(done == 0u, "and delivers nothing");
    expect(wfs_stub_reads == before, "without touching the device");

    wfs_stub_teardown();
}

/* ---- inline content ----------------------------------------------------- */

/* A file small enough to live in its record reads without a device request at
 * all (§7). */
static void test_an_inline_file_reads_without_touching_the_device(void) {
    static uint8_t buf[TINY_SIZE + 16u];
    uint32_t done = 0;
    uint32_t before;

    if (setup() != 0 || open_object(E_TINY) != 0) {
        expect(0, "setup");
        return;
    }
    expect((g_obj.out.flags & WFS_OBJ_INLINE_DATA) != 0u, "the file is inline");

    before = wfs_stub_reads;
    expect(do_read(0u, buf, TINY_SIZE, &done) == 0, "it reads");
    expect(done == TINY_SIZE, "in full");
    expect(matches(buf, TINY_SEED, 0u, TINY_SIZE, TINY_SIZE), "with the right bytes");
    expect(wfs_stub_reads == before, "and no block was requested");

    /* Slicing works the same on the inline path. */
    expect(do_read(30u, buf, 20u, &done) == 0 && done == 20u, "a slice of an inline file");
    expect(matches(buf, TINY_SEED, 30u, 20u, TINY_SIZE), "with the right bytes");

    expect(do_read(TINY_SIZE - 5u, buf, 50u, &done) == 0 && done == 5u,
           "and a read past its end is short");

    wfs_stub_teardown();
}

/* An inline object claiming more content than the record can hold is corrupt: the
 * read would otherwise walk off the end of the record. */
static void test_an_oversized_inline_object_is_refused(void) {
    static uint8_t buf[512];
    wfs_read_ctx_t ctx;
    wasmos_wasm_coroutine_t task;
    struct wfs_object obj;
    static uint8_t inline_bytes[WFS_INLINE_DATA_MAX];

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&obj, 0, sizeof(obj));
    obj.type = WFS_TYPE_FILE;
    obj.flags = WFS_OBJ_INLINE_DATA;
    obj.size = WFS_INLINE_DATA_MAX + 8u;

    wfs_read_init(&ctx, &g_vol, &obj, inline_bytes, 0u, buf, (uint32_t)obj.size);
    expect_rc((wasmos_error_code_t)wfs_stub_run_task(&task, wfs_read_task, &ctx),
              WASMOS_ERR_FS_CORRUPT,
              "an inline object larger than a record");

    /* And one with the flag set but no bytes supplied, which a caller that
     * forgot wfs_object_ctx_t's inline_data would produce. */
    obj.size = 16u;
    wfs_read_init(&ctx, &g_vol, &obj, NULL, 0u, buf, 16u);
    expect_rc((wasmos_error_code_t)wfs_stub_run_task(&task, wfs_read_task, &ctx),
              WASMOS_ERR_FS_BAD_ARGS,
              "an inline object with no bytes supplied");

    wfs_stub_teardown();
}

/* ---- holes -------------------------------------------------------------- */

/* A hole reads as zeroes (§9). mkfs never writes one, so the object is built
 * here with a gap between its extents — which is what a sparse write will
 * produce once writing exists. */
static void test_a_hole_reads_as_zeroes(void) {
    static uint8_t buf[BS * 3u];
    struct wfs_read_probe {
        int unused;
    } probe;
    wfs_read_ctx_t ctx;
    wasmos_wasm_coroutine_t task;
    struct wfs_object sparse;
    uint32_t i;
    int all_zero = 1;

    (void)probe;
    if (setup() != 0 || open_object(E_BIG) != 0) {
        expect(0, "setup");
        return;
    }

    /* Logical 0 maps to the real file's first block; logical 1 maps to nothing;
     * logical 2 maps to the real file's third block. */
    memset(&sparse, 0, sizeof(sparse));
    sparse.type = WFS_TYPE_FILE;
    sparse.size = (uint64_t)3u * BS;
    sparse.extent_count = 2u;
    sparse.extents[0].logical_block = 0u;
    sparse.extents[0].physical_block = g_obj.out.extents[0].physical_block;
    sparse.extents[0].length = 1u;
    sparse.extents[1].logical_block = 2u;
    sparse.extents[1].physical_block = g_obj.out.extents[0].physical_block + 2u;
    sparse.extents[1].length = 1u;

    wfs_read_init(&ctx, &g_vol, &sparse, NULL, 0u, buf, 3u * BS);
    expect(wfs_stub_run_task(&task, wfs_read_task, &ctx) == 0, "a sparse object reads");
    expect(ctx.done == 3u * BS, "in full, hole included");

    expect(matches(buf, BIG_SEED, 0u, BS, BIG_SIZE), "the mapped first block is its data");
    for (i = 0; i < BS; ++i) {
        if (buf[BS + i] != 0u) {
            all_zero = 0;
            break;
        }
    }
    expect(all_zero, "the hole reads as zeroes");
    expect(matches(buf + (size_t)2u * BS, BIG_SEED, (uint64_t)2u * BS, BS, BIG_SIZE),
           "and the block after the hole is its data");

    /* A read entirely inside the hole is still zeroes, and still not an error. */
    wfs_read_init(&ctx, &g_vol, &sparse, NULL, BS + 100u, buf, 200u);
    expect(wfs_stub_run_task(&task, wfs_read_task, &ctx) == 0, "a read inside the hole");
    expect(ctx.done == 200u, "delivers what was asked");
    all_zero = 1;
    for (i = 0; i < 200u; ++i) {
        if (buf[i] != 0u) {
            all_zero = 0;
            break;
        }
    }
    expect(all_zero, "as zeroes");

    wfs_stub_teardown();
}

/* ---- what must be refused ----------------------------------------------- */

/* A directory's bytes are records. Handing them out as file content would leak
 * the on-disk layout to something that asked for a file. */
static void test_reading_a_directory_is_refused(void) {
    static uint8_t buf[64];

    if (setup() != 0 || open_object(E_DIR) != 0) {
        expect(0, "setup");
        return;
    }
    expect(g_obj.out.type == WFS_TYPE_DIR, "the object is a directory");
    expect_rc((wasmos_error_code_t)do_read(0u, buf, 64u, NULL),
              WASMOS_ERR_FS_IS_DIR,
              "reading a directory as a file");

    wfs_stub_teardown();
}

static void test_a_device_error_fails_the_read(void) {
    static uint8_t buf[64];

    if (setup() != 0 || open_object(E_BIG) != 0) {
        expect(0, "setup");
        return;
    }
    wfs_stub_fail_next = 1;
    expect_rc((wasmos_error_code_t)do_read(0u, buf, 64u, NULL),
              WASMOS_ERR_FS_IO,
              "a failed transfer fails the read");

    wfs_stub_teardown();
}

/* ---- a whole path, the way the driver will do it ------------------------ */

/* Name to bytes, with nothing pre-arranged: this is what an FS_IPC read will do
 * once the dispatch layer exists. */
static void test_a_name_resolves_all_the_way_to_bytes(void) {
    static uint8_t buf[256];
    wfs_object_ctx_t root;
    wfs_dir_ctx_t d;
    wasmos_wasm_coroutine_t task;
    uint32_t done = 0;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&root, 0, sizeof(root));
    root.vol = &g_vol;
    root.object_id = WFS_OBJECT_ROOT;
    expect(wfs_stub_run_task(&task, wfs_object_task, &root) == 0, "the root reads");

    wfs_dir_lookup_init(&d, &g_vol, &root.out, "tiny", 4u);
    expect(wfs_stub_run_task(&task, wfs_dir_task, &d) == 0 && d.found == 1u, "tiny resolves");

    memset(&g_obj, 0, sizeof(g_obj));
    g_obj.vol = &g_vol;
    g_obj.object_id = d.object_id;
    expect(wfs_stub_run_task(&task, wfs_object_task, &g_obj) == 0, "its record reads");

    expect(do_read(0u, buf, TINY_SIZE, &done) == 0 && done == TINY_SIZE, "and its bytes read");
    expect(matches(buf, TINY_SEED, 0u, TINY_SIZE, TINY_SIZE),
           "matching what the formatter was handed");

    wfs_stub_teardown();
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_whole_multi_block_file_reads_back),
    WASMOS_TEST_CASE(test_partial_reads_slice_correctly),
    WASMOS_TEST_CASE(test_a_read_past_the_end_is_short),
    WASMOS_TEST_CASE(test_a_zero_length_read_delivers_nothing),
    WASMOS_TEST_CASE(test_an_inline_file_reads_without_touching_the_device),
    WASMOS_TEST_CASE(test_an_oversized_inline_object_is_refused),
    WASMOS_TEST_CASE(test_a_hole_reads_as_zeroes),
    WASMOS_TEST_CASE(test_reading_a_directory_is_refused),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_read),
    WASMOS_TEST_CASE(test_a_name_resolves_all_the_way_to_bytes),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_read: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_read: %d checks passed\n", g_checks);
    return 0;
}
