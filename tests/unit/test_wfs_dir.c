/* Host unit test for the WFS directory scan (src/drivers/fs_wfs/wfs_dir.c).
 *
 * Directory blocks are laid out by this suite rather than by mkfs_wfs, which
 * builds only an empty root: what has to be exercised is a directory with
 * entries in it, spanning more than one block, with records a writer would never
 * produce. The block layout helpers below are the record rules of §10 written
 * out once — stride a multiple of 8, the last record stretched to meet the tail,
 * the tail sealed over the whole block — so a case can then break exactly one of
 * them.
 *
 * The scan runs as a task on the system coroutine runtime over the shared fake
 * block server, and it runs the extent walk as a child, so a directory's blocks
 * are reached the same way a file's would be.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_crc32c.h"
#include "wfs_dir.h"
#include "wfs_endian.h"
#include "wfs_format.h"
#include "wfs_types.h"

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
    0x77, 0x21, 0xbe, 0x0d, 0x44, 0x9a, 0x4c, 0x51, 0x83, 0x2f, 0x1d, 0xc8, 0x05, 0xe6, 0x39, 0xb2};
#define NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;

/* ---- laying out directory blocks ---------------------------------------- */

static uint8_t* image_block(uint32_t block) {
    return wfs_stub_image + (size_t)block * wfs_stub_block_size;
}

/* Seal a directory block: the tail's checksum covers the whole block with its
 * own four bytes zeroed, seeded with the block's number (§13). */
static void seal_dir(uint32_t block) {
    uint8_t* d = image_block(block);
    uint32_t off = wfs_dir_usable_bytes(wfs_stub_block_size) +
                   (uint32_t)offsetof(struct wfs_dir_tail, checksum);

    wfs_wr32(d, off, 0u);
    wfs_wr32(d, off, wfs_checksum_struct(k_uuid, block, d, wfs_stub_block_size, off));
}

typedef struct {
    const char* name;
    uint32_t object_id;
    uint8_t type;
} entry_t;

/* Lay out `.`, `..` and `entries` in `block`, stretch the last record to meet
 * the tail, write the tail, and seal it. */
static void dir_build(uint32_t block, uint32_t self, uint32_t parent, const entry_t* entries,
                      uint32_t n) {
    uint8_t* d = image_block(block);
    uint32_t usable = wfs_dir_usable_bytes(wfs_stub_block_size);
    uint32_t off = 0;
    uint32_t last = 0;
    uint32_t i;
    uint8_t* t;

    memset(d, 0, wfs_stub_block_size);

    wfs_wr64(d, 0u, self);
    wfs_wr16(d, 8u, (uint16_t)wfs_dir_record_length(1u));
    d[10] = 1u;
    d[11] = (uint8_t)WFS_TYPE_DIR;
    d[12] = '.';
    off = wfs_dir_record_length(1u);

    wfs_wr64(d, off, parent);
    wfs_wr16(d, off + 8u, (uint16_t)wfs_dir_record_length(2u));
    d[off + 10u] = 2u;
    d[off + 11u] = (uint8_t)WFS_TYPE_DIR;
    d[off + 12u] = '.';
    d[off + 13u] = '.';
    last = off;
    off += wfs_dir_record_length(2u);

    for (i = 0; i < n; ++i) {
        uint32_t len = (uint32_t)strlen(entries[i].name);
        uint32_t rec = wfs_dir_record_length(len);
        uint32_t k;

        wfs_wr64(d, off, entries[i].object_id);
        wfs_wr16(d, off + 8u, (uint16_t)rec);
        d[off + 10u] = (uint8_t)len;
        d[off + 11u] = entries[i].type;
        for (k = 0; k < len; ++k) {
            d[off + WFS_DIR_ENTRY_HEADER + k] = (uint8_t)entries[i].name[k];
        }
        last = off;
        off += rec;
    }

    /* The last record stretches to the tail, so a scan of the block ends
     * exactly where the tail begins (§10). */
    wfs_wr16(d, last + 8u, (uint16_t)(usable - last));

    t = d + usable;
    wfs_wr64(t, (uint32_t)offsetof(struct wfs_dir_tail, object_id), 0u);
    wfs_wr16(t, (uint32_t)offsetof(struct wfs_dir_tail, record_length), WFS_DIR_TAIL_SIZE);
    t[offsetof(struct wfs_dir_tail, name_length)] = 0u;
    t[offsetof(struct wfs_dir_tail, type)] = (uint8_t)WFS_DIR_TAIL_TYPE;
    seal_dir(block);
}

static uint32_t scratch(uint32_t n) {
    return g_layout.root_data_block + 1u + n;
}

static int setup(void) {
    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &g_layout) != 0) {
        return -1;
    }
    memset(&g_vol, 0, sizeof(g_vol));
    if (wfs_super_parse(wfs_stub_image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &g_vol.super) !=
        WASMOS_ERR_NONE) {
        return -1;
    }
    g_vol.mounted = 1u;
    return 0;
}

/* A directory object over `nblocks` consecutive blocks from `first`. */
static void make_dir(struct wfs_object* obj, uint32_t first, uint32_t nblocks) {
    memset(obj, 0, sizeof(*obj));
    obj->object_id = WFS_OBJECT_ROOT;
    obj->type = WFS_TYPE_DIR;
    obj->link_count = 2u;
    obj->size = (uint64_t)nblocks * wfs_stub_block_size;
    obj->extent_count = 1u;
    obj->extents[0].logical_block = 0u;
    obj->extents[0].physical_block = first;
    obj->extents[0].length = nblocks;
}

static int32_t lookup(wfs_dir_ctx_t* ctx, const struct wfs_object* dir, const char* name) {
    wasmos_wasm_coroutine_t task;

    wfs_dir_lookup_init(ctx, &g_vol, dir, name, name ? (uint32_t)strlen(name) : 0u);
    return wfs_stub_run_task(&task, wfs_dir_task, ctx);
}

/* Advance an already-initialised walk by one entry. */
static int32_t step(wfs_dir_ctx_t* ctx) {
    wasmos_wasm_coroutine_t task;

    ctx->pc = WFS_DIR_PC_MAP;
    ctx->found = 0u;
    return wfs_stub_run_task(&task, wfs_dir_task, ctx);
}

/* ---- lookup ------------------------------------------------------------- */

/* The volume mkfs_wfs wrote, read through the scan: dot and dotdot are the first
 * two records of every directory (§10), and mkfs is what put them there. */
static void test_the_formatted_root_carries_dot_and_dotdot(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    make_dir(&dir, g_layout.root_data_block, 1u);

    expect(lookup(&ctx, &dir, ".") == 0 && ctx.found == 1u, "dot is found");
    expect(ctx.object_id == WFS_OBJECT_ROOT, "dot names the root");
    expect(ctx.type == WFS_TYPE_DIR, "dot is a directory");
    expect(ctx.name_length == 1u && ctx.name[0] == '.', "the name is copied out");

    expect(lookup(&ctx, &dir, "..") == 0 && ctx.found == 1u, "dotdot is found");
    expect(ctx.object_id == WFS_OBJECT_ROOT, "the root's dotdot names the root");

    wfs_stub_teardown();
}

/* A name that is not there is not an error: a lookup that misses is how a create
 * learns it may proceed. */
static void test_a_missing_name_is_not_an_error(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    make_dir(&dir, g_layout.root_data_block, 1u);

    expect(lookup(&ctx, &dir, "nothing") == 0, "the scan completes");
    expect(ctx.found == 0u, "and reports no entry");

    wfs_stub_teardown();
}

static void test_entries_are_found_by_exact_name(void) {
    static const entry_t entries[] = {
        {"README", 20u, WFS_TYPE_FILE},
        {"bin", 21u, WFS_TYPE_DIR},
        {"a-very-long-name-with-dashes", 22u, WFS_TYPE_FILE},
        {"link", 23u, WFS_TYPE_SYMLINK},
    };
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 4u);
    make_dir(&dir, scratch(0), 1u);

    expect(lookup(&ctx, &dir, "README") == 0 && ctx.object_id == 20u, "the first entry");
    expect(ctx.type == WFS_TYPE_FILE, "its type comes from the record");
    expect(lookup(&ctx, &dir, "bin") == 0 && ctx.object_id == 21u, "a middle entry");
    expect(lookup(&ctx, &dir, "a-very-long-name-with-dashes") == 0 && ctx.object_id == 22u,
           "a long name");
    expect(lookup(&ctx, &dir, "link") == 0 && ctx.object_id == 23u, "the last entry");
    expect(ctx.type == WFS_TYPE_SYMLINK, "a symlink's type");

    /* Names are byte strings compared exactly: no case folding, unlike FAT's
     * short names, so these are three different entries. */
    expect(lookup(&ctx, &dir, "readme") == 0 && ctx.found == 0u, "a lowercase name misses");
    expect(lookup(&ctx, &dir, "BIN") == 0 && ctx.found == 0u, "an uppercase name misses");
    /* A prefix is not a match either. */
    expect(lookup(&ctx, &dir, "READ") == 0 && ctx.found == 0u, "a prefix misses");

    wfs_stub_teardown();
}

/* A removed entry keeps its name bytes but zeroes its object_id, which is how an
 * entry is removed without rewriting the block (§10). A scan must skip it. */
static void test_a_removed_entry_is_skipped(void) {
    static const entry_t entries[] = {
        {"gone", 30u, WFS_TYPE_FILE},
        {"kept", 31u, WFS_TYPE_FILE},
    };
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;
    uint8_t* d;
    uint32_t off;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 2u);
    /* Zero "gone"'s object_id in place, leaving the name and the stride. */
    d = image_block(scratch(0));
    off = wfs_dir_record_length(1u) + wfs_dir_record_length(2u);
    wfs_wr64(d, off, 0u);
    seal_dir(scratch(0));

    make_dir(&dir, scratch(0), 1u);
    expect(lookup(&ctx, &dir, "gone") == 0 && ctx.found == 0u, "a removed entry is not found");
    expect(lookup(&ctx, &dir, "kept") == 0 && ctx.object_id == 31u,
           "and the scan continues past it");

    wfs_stub_teardown();
}

/* A directory spanning more than one block. The cursor must cross the boundary,
 * and the second block must be reached through the extent map like any other
 * data block. */
static void test_a_multi_block_directory_is_scanned_through(void) {
    static const entry_t first[] = {{"in-first", 40u, WFS_TYPE_FILE}};
    static const entry_t second[] = {{"in-second", 41u, WFS_TYPE_FILE}};
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, first, 1u);
    dir_build(scratch(1), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, second, 1u);
    make_dir(&dir, scratch(0), 2u);

    expect(lookup(&ctx, &dir, "in-first") == 0 && ctx.object_id == 40u, "an entry in block 0");
    expect(lookup(&ctx, &dir, "in-second") == 0 && ctx.object_id == 41u,
           "an entry in block 1, past the first block's tail");

    /* Both blocks were read, in order, and nothing beyond them. */
    wfs_stub_reset_counters();
    expect(lookup(&ctx, &dir, "in-second") == 0, "look the second up again");
    expect(wfs_stub_req_count == 2u, "the scan read exactly two blocks");
    if (wfs_stub_req_count >= 2u) {
        expect(wfs_stub_req_blocks[0] == scratch(0), "the first block first");
        expect(wfs_stub_req_blocks[1] == scratch(1), "then the second");
    }

    wfs_stub_teardown();
}

/* ---- the readdir walk --------------------------------------------------- */

/* With no name to match, the scan stops at each entry in turn and leaves the
 * cursor past it, which is what a readdir walks with. */
static void test_a_walk_enumerates_every_entry_once(void) {
    static const entry_t entries[] = {
        {"one", 50u, WFS_TYPE_FILE},
        {"two", 51u, WFS_TYPE_FILE},
        {"three", 52u, WFS_TYPE_DIR},
    };
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;
    char seen[8][WFS_NAME_MAX + 1u];
    uint32_t n = 0;
    uint32_t i;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 3u);
    make_dir(&dir, scratch(0), 1u);

    wfs_dir_lookup_init(&ctx, &g_vol, &dir, NULL, 0u);
    for (i = 0; i < 8u; ++i) {
        wasmos_wasm_coroutine_t task;

        if (wfs_stub_run_task(&task, wfs_dir_task, &ctx) != 0) {
            expect(0, "a walk step failed");
            break;
        }
        if (!ctx.found) {
            break;
        }
        memcpy(seen[n], ctx.name, (size_t)ctx.name_length + 1u);
        n++;
        ctx.pc = WFS_DIR_PC_SCAN; /* resume in the block already staged */
        ctx.found = 0u;
    }

    /* Dot, dotdot, then the three entries, in record order. */
    expect(n == 5u, "the walk saw five entries");
    if (n == 5u) {
        expect(strcmp(seen[0], ".") == 0, "dot first");
        expect(strcmp(seen[1], "..") == 0, "dotdot second");
        expect(strcmp(seen[2], "one") == 0, "then the entries in record order");
        expect(strcmp(seen[3], "two") == 0, "second entry");
        expect(strcmp(seen[4], "three") == 0, "third entry");
    }

    wfs_stub_teardown();
}

/* An exhausted walk reports no entry rather than failing, and stays exhausted. */
static void test_a_walk_ends_cleanly(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;
    uint32_t i;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    make_dir(&dir, g_layout.root_data_block, 1u);
    wfs_dir_lookup_init(&ctx, &g_vol, &dir, NULL, 0u);

    for (i = 0; i < 4u; ++i) {
        wasmos_wasm_coroutine_t task;

        if (wfs_stub_run_task(&task, wfs_dir_task, &ctx) != 0) {
            expect(0, "a walk step failed");
            return;
        }
        if (!ctx.found) {
            break;
        }
        ctx.pc = WFS_DIR_PC_SCAN;
        ctx.found = 0u;
    }
    expect(i == 2u, "the formatted root holds exactly dot and dotdot");
    expect(step(&ctx) == 0 && ctx.found == 0u, "and stays exhausted");

    wfs_stub_teardown();
}

/* ---- records that must be refused --------------------------------------- */

/* The tail carries the block's checksum, so a corrupted directory block is
 * refused before any record in it is believed. */
static void test_a_corrupted_directory_block_is_refused(void) {
    static const entry_t entries[] = {{"file", 60u, WFS_TYPE_FILE}};
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 1u);
    image_block(scratch(0))[wfs_dir_record_length(1u) + 12u] ^= 0x20u;
    make_dir(&dir, scratch(0), 1u);

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "file"),
              WASMOS_ERR_FS_CHECKSUM,
              "a flipped bit in a directory block");

    wfs_stub_teardown();
}

/* A stride of zero would make the scan of a block never advance. It is rejected
 * before it is used, so a corrupt block cannot hang the driver. */
static void test_a_zero_stride_is_refused_rather_than_looped_on(void) {
    static const entry_t entries[] = {{"file", 61u, WFS_TYPE_FILE}};
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 1u);
    wfs_wr16(image_block(scratch(0)), 8u, 0u); /* dot's stride */
    seal_dir(scratch(0));
    make_dir(&dir, scratch(0), 1u);

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "file"),
              WASMOS_ERR_FS_CORRUPT,
              "a record_length of zero");

    wfs_stub_teardown();
}

/* A stride that is not a multiple of 8 leaves the next record's object_id
 * misaligned, which is the rule §10 exists to keep. */
static void test_a_misaligned_stride_is_refused(void) {
    static const entry_t entries[] = {{"file", 62u, WFS_TYPE_FILE}};
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 1u);
    wfs_wr16(image_block(scratch(0)), 8u, 20u); /* not a multiple of 8 */
    seal_dir(scratch(0));
    make_dir(&dir, scratch(0), 1u);

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "file"),
              WASMOS_ERR_FS_CORRUPT,
              "a stride that is not a multiple of 8");

    wfs_stub_teardown();
}

/* A record must not straddle into the tail: the tail is not a record and reading
 * it as one would take its checksum bytes for a name. */
static void test_a_record_running_into_the_tail_is_refused(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;
    uint32_t usable;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, NULL, 0u);
    usable = wfs_dir_usable_bytes(wfs_stub_block_size);
    /* Stretch dotdot past the tail. */
    wfs_wr16(image_block(scratch(0)),
             wfs_dir_record_length(1u) + 8u,
             (uint16_t)(usable - wfs_dir_record_length(1u) + 8u));
    seal_dir(scratch(0));
    make_dir(&dir, scratch(0), 1u);

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "x"),
              WASMOS_ERR_FS_CORRUPT,
              "a record stretching past the tail");

    wfs_stub_teardown();
}

/* A name longer than its own record would read bytes belonging to the next. */
static void test_a_name_longer_than_its_record_is_refused(void) {
    static const entry_t entries[] = {{"file", 63u, WFS_TYPE_FILE}};
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, entries, 1u);
    image_block(scratch(0))[10] = 200u; /* dot's name_length, in a 16-byte record */
    seal_dir(scratch(0));
    make_dir(&dir, scratch(0), 1u);

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "file"),
              WASMOS_ERR_FS_CORRUPT,
              "a name_length past the record");

    wfs_stub_teardown();
}

/* ---- the object itself -------------------------------------------------- */

static void test_scanning_a_file_is_refused(void) {
    struct wfs_object obj;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    make_dir(&obj, g_layout.root_data_block, 1u);
    obj.type = WFS_TYPE_FILE;
    expect_rc((wasmos_error_code_t)lookup(&ctx, &obj, "x"),
              WASMOS_ERR_FS_NOT_DIR,
              "a file is not a directory");

    wfs_stub_teardown();
}

/* A directory occupies whole blocks: a partial one could not carry a tail. */
static void test_a_partial_directory_size_is_refused(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    make_dir(&dir, g_layout.root_data_block, 1u);
    dir.size = wfs_stub_block_size + 17u;
    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "x"),
              WASMOS_ERR_FS_CORRUPT,
              "a size that is not a whole number of blocks");

    dir.size = 0u;
    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "x"),
              WASMOS_ERR_FS_CORRUPT,
              "a directory of no blocks");

    wfs_stub_teardown();
}

/* A directory is never sparse, so a hole means a block of entries is missing
 * rather than that it reads as zeroes. */
static void test_a_hole_in_a_directory_is_refused(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    dir_build(scratch(0), WFS_OBJECT_ROOT, WFS_OBJECT_ROOT, NULL, 0u);
    /* Two blocks of size, but only logical block 0 is mapped. */
    make_dir(&dir, scratch(0), 1u);
    dir.size = 2u * (uint64_t)wfs_stub_block_size;

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "absent"),
              WASMOS_ERR_FS_CORRUPT,
              "an unmapped block inside a directory");

    wfs_stub_teardown();
}

static void test_an_overlong_lookup_name_is_refused(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;
    static char big[WFS_NAME_MAX + 8u];

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(big, 'a', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    make_dir(&dir, g_layout.root_data_block, 1u);

    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, big),
              WASMOS_ERR_FS_NAME,
              "a name longer than a record can hold");

    wfs_stub_teardown();
}

static void test_a_device_error_fails_the_scan(void) {
    struct wfs_object dir;
    wfs_dir_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    make_dir(&dir, g_layout.root_data_block, 1u);
    wfs_stub_fail_next = 1;
    expect_rc((wasmos_error_code_t)lookup(&ctx, &dir, "."),
              WASMOS_ERR_FS_IO,
              "a failed transfer fails the scan");

    wfs_stub_teardown();
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_the_formatted_root_carries_dot_and_dotdot),
    WASMOS_TEST_CASE(test_a_missing_name_is_not_an_error),
    WASMOS_TEST_CASE(test_entries_are_found_by_exact_name),
    WASMOS_TEST_CASE(test_a_removed_entry_is_skipped),
    WASMOS_TEST_CASE(test_a_multi_block_directory_is_scanned_through),
    WASMOS_TEST_CASE(test_a_walk_enumerates_every_entry_once),
    WASMOS_TEST_CASE(test_a_walk_ends_cleanly),
    WASMOS_TEST_CASE(test_a_corrupted_directory_block_is_refused),
    WASMOS_TEST_CASE(test_a_zero_stride_is_refused_rather_than_looped_on),
    WASMOS_TEST_CASE(test_a_misaligned_stride_is_refused),
    WASMOS_TEST_CASE(test_a_record_running_into_the_tail_is_refused),
    WASMOS_TEST_CASE(test_a_name_longer_than_its_record_is_refused),
    WASMOS_TEST_CASE(test_scanning_a_file_is_refused),
    WASMOS_TEST_CASE(test_a_partial_directory_size_is_refused),
    WASMOS_TEST_CASE(test_a_hole_in_a_directory_is_refused),
    WASMOS_TEST_CASE(test_an_overlong_lookup_name_is_refused),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_scan),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_dir: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_dir: %d checks passed\n", g_checks);
    return 0;
}
