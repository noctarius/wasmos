/* Host unit test for the WFS driver operations: mount, group descriptors and
 * object records, run as tasks on the SYSTEM coroutine runtime against a volume
 * mkfs_wfs built.
 *
 * Nothing here reimplements scheduling. The suite links the real
 * coroutine_wasm.c and ipc_future_wasm.c and drives them through
 * wfs_stub_run_task, so what is exercised is the runtime's own parking and
 * resumption. The device below the driver is the shared fake BLOCK server in
 * stubs_wfs_block_server.c, which also puts the request encoding under test.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
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

static int build_volume(uint64_t size, uint32_t block_size) {
    return wfs_stub_build_volume(size, block_size, k_uuid, TEST_NOW_NS, &g_layout);
}

static int32_t run_mount(wfs_mount_ctx_t* ctx, wfs_volume_t* vol) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    memset(vol, 0, sizeof(*vol));
    ctx->vol = vol;
    return wfs_stub_run_task(&task, wfs_mount_task, ctx);
}

static int32_t run_object(wfs_object_ctx_t* ctx, const wfs_volume_t* vol, uint32_t object_id) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    ctx->vol = vol;
    ctx->object_id = object_id;
    return wfs_stub_run_task(&task, wfs_object_task, ctx);
}

static int32_t run_group(wfs_group_ctx_t* ctx, const wfs_volume_t* vol, uint32_t group) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    ctx->vol = vol;
    ctx->group = group;
    return wfs_stub_run_task(&task, wfs_group_task, ctx);
}

/* ---- mount --------------------------------------------------------------- */

static void test_mount_reads_a_volume_mkfs_wrote(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(run_mount(&ctx, &vol) == 0, "the mount task completes");
    expect(vol.mounted == 1u, "the volume is mounted");
    expect(wfs_stub_reads > 0u, "it read from the device");

    expect(vol.super.block_size == 4096u, "block size");
    expect(vol.super.total_blocks == g_layout.total_blocks, "block count");
    expect(vol.super.group_count == g_layout.group_count, "group count");
    expect(vol.super.root_object_id == WFS_OBJECT_ROOT, "root object id");
    expect(vol.super.needs_replay == 0u, "a fresh volume needs no replay");
    expect(vol.super.read_only == 0u, "and mounts writable");
    expect(memcmp(vol.super.uuid, k_uuid, WFS_UUID_LEN) == 0, "uuid");

    wfs_stub_teardown();
}

/* Regression: 2026-08-24-wfs-yield-local — the block number a step was about to
 * read was held in a C LOCAL. The runtime preserves no stack across a resume, so
 * on the resume path that local was read uninitialised and the request went out
 * for whatever the stack held. The visible cost is a read of the wrong block, so
 * this pins the SEQUENCE of blocks mount asks for rather than only the count. It
 * also covers the request encoding: the fake server divides the lba back out by
 * the sector count, so a wrong scaling shows up here too.
 */
static void test_mount_requests_exactly_the_blocks_it_should(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(g_layout.group_count == 1u, "a 16 MiB volume is one group");
    expect(run_mount(&ctx, &vol) == 0, "mount");

    /* Block 0 for the superblock, then the one descriptor-table block. Mount
     * reads neither the object table nor the bitmaps. */
    expect(wfs_stub_req_count == 2u, "mount reads exactly two blocks");
    if (wfs_stub_req_count >= 2u) {
        expect(wfs_stub_req_blocks[0] == 0u, "the first read is block 0, for the superblock");
        expect(wfs_stub_req_blocks[1] == g_layout.group_table_start,
               "the second read is the group descriptor table");
    }
    expect(wfs_stub_last_sectors == 4096u / WFS_SECTOR_BYTES,
           "a filesystem block is requested as its whole run of sectors");

    wfs_stub_teardown();
}

/* The staged block is a one-block cache, so a read of the block already staged
 * must not reach the device. */
static void test_the_staged_block_is_a_cache(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wfs_object_ctx_t o;
    uint32_t after_first;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(run_mount(&ctx, &vol) == 0, "mount");

    expect(run_object(&o, &vol, WFS_OBJECT_ROOT) == 0, "read the root object");
    after_first = wfs_stub_reads;
    expect(run_object(&o, &vol, WFS_OBJECT_ROOT) == 0, "read it again");
    expect(wfs_stub_reads == after_first, "a second read of a staged block costs no request");

    wfs_stub_teardown();
}

/* ---- object records ------------------------------------------------------ */

static void test_the_root_object_reads_back_as_mkfs_wrote_it(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wfs_object_ctx_t o;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(run_mount(&ctx, &vol) == 0, "mount");
    expect(run_object(&o, &vol, WFS_OBJECT_ROOT) == 0, "the root record reads and verifies");

    expect(o.out.object_id == WFS_OBJECT_ROOT, "object id");
    expect(o.out.type == WFS_TYPE_DIR, "the root is a directory");
    expect(o.out.link_count == 2u, "link count is 2");
    expect(o.out.size == 4096u, "size is one directory block");
    expect(o.out.extent_count == 1u, "one extent");
    expect(o.out.extents[0].physical_block == g_layout.root_data_block,
           "the extent points at the root's data block");
    expect(o.out.extents[0].length == 1u, "the extent is one block");
    expect(o.out.extent_tree_block == 0u, "an inline extent needs no tree");
    expect(o.out.mtime == TEST_NOW_NS, "timestamps round-trip");

    wfs_stub_teardown();
}

static void test_an_unallocated_object_is_refused(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wfs_object_ctx_t o;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(run_mount(&ctx, &vol) == 0, "mount");

    /* A task that fails rejects its completion future, so the join reports the
     * packed code directly — no separate status channel. */
    expect_rc((wasmos_error_code_t)run_object(&o, &vol, WFS_OBJECT_INVALID),
              WASMOS_ERR_FS_NOT_FOUND,
              "object 0 is refused");
    expect_rc((wasmos_error_code_t)run_object(&o, &vol, vol.super.total_objects),
              WASMOS_ERR_FS_NOT_FOUND,
              "an id past the table is refused");
    /* An id inside the table but never allocated has a zeroed record, whose
     * checksum cannot match: corrupt, not merely absent. Telling the two apart
     * is the bitmap's job, not the record reader's. */
    expect_rc((wasmos_error_code_t)run_object(&o, &vol, WFS_OBJECT_FIRST),
              WASMOS_ERR_FS_CHECKSUM,
              "an unallocated record does not verify");

    wfs_stub_teardown();
}

/* A record is checksummed under its object id, so one moved to another slot must
 * not verify there (§13). */
static void test_an_object_record_is_bound_to_its_slot(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wfs_object_ctx_t o;
    uint8_t* table;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    table = wfs_stub_image + (size_t)g_layout.object_table_start * g_layout.block_size;
    memcpy(table + (size_t)2u * WFS_OBJECT_SIZE, table + WFS_OBJECT_SIZE, WFS_OBJECT_SIZE);

    expect(run_mount(&ctx, &vol) == 0, "mount");
    expect_rc((wasmos_error_code_t)run_object(&o, &vol, 2u),
              WASMOS_ERR_FS_CHECKSUM,
              "a transplanted record is refused");

    wfs_stub_teardown();
}

/* ---- group descriptors --------------------------------------------------- */

static void test_group_descriptors_read_back(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wfs_group_ctx_t g;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(run_mount(&ctx, &vol) == 0, "mount");

    expect(run_group(&g, &vol, 0u) == 0, "group 0's descriptor verifies");
    expect(g.out.block_bitmap == g_layout.bitmap_start, "it names its block bitmap");
    expect(g.out.object_bitmap == g_layout.bitmap_start + 1u, "it names its object bitmap");
    expect(g.out.object_table == g_layout.object_table_start, "it names its object table slice");
    expect(g.out.free_blocks == g_layout.free_blocks, "one group's free count is the volume's");

    expect_rc((wasmos_error_code_t)run_group(&g, &vol, vol.super.group_count),
              WASMOS_ERR_FS_CORRUPT,
              "a group past the table is refused");

    wfs_stub_teardown();
}

/* Mount verifies every descriptor before declaring the volume usable, and the
 * child task's failure reaches it through the join. */
static void test_mount_refuses_a_volume_with_a_bad_descriptor(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    uint8_t* table;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    table = wfs_stub_image + (size_t)g_layout.group_table_start * g_layout.block_size;
    table[4] = (uint8_t)(table[4] ^ 0x10u);

    expect_rc((wasmos_error_code_t)run_mount(&ctx, &vol),
              WASMOS_ERR_FS_CHECKSUM,
              "a corrupted group descriptor fails the mount");
    expect(vol.mounted == 0u, "and the volume is not marked mounted");

    wfs_stub_teardown();
}

/* ---- mount failure paths ------------------------------------------------- */

static void test_mount_refuses_a_device_that_holds_no_volume(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    memset(wfs_stub_image, 0, g_layout.block_size);

    expect_rc((wasmos_error_code_t)run_mount(&ctx, &vol),
              WASMOS_ERR_FS_BAD_MAGIC,
              "an unformatted device");
    expect(vol.mounted == 0u, "nothing is mounted");

    wfs_stub_teardown();
}

/* A device error must reach the awaiting task. The future bridge rejects on
 * BLOCK_IPC_ERROR, so the failure arrives at the await rather than being
 * swallowed into a silent retry. */
static void test_a_device_error_fails_the_mount(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    wfs_stub_fail_next = 1;

    expect_rc((wasmos_error_code_t)run_mount(&ctx, &vol),
              WASMOS_ERR_FS_IO,
              "a failed transfer fails the mount");
    /* And it must not leave the device's leftover bytes looking cached. */
    expect(wfs_stub_block()->staged_block == WFS_BLOCK_NONE, "the cache tag is cleared on failure");
    expect(vol.mounted == 0u, "nothing is mounted");

    wfs_stub_teardown();
}

/* A send that cannot be issued must fail the same way: the bridge returns an
 * already-rejected future rather than NULL, so the await path is identical. */
static void test_a_failed_send_fails_the_mount(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    wfs_stub_send_status = -1;

    expect(run_mount(&ctx, &vol) != 0, "an unsendable request fails the mount");
    expect(vol.mounted == 0u, "nothing is mounted");
    wfs_stub_send_status = 0;

    wfs_stub_teardown();
}

/* Until replay exists a volume that was not unmounted cleanly mounts read-only
 * rather than serving metadata the log has superseded. */
static void test_a_dirty_volume_mounts_read_only(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    uint8_t* sb;
    uint8_t* csum;
    uint32_t c;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    sb = wfs_stub_image + WFS_SUPER_OFFSET;
    sb[offsetof(struct wfs_superblock, state)] = (uint8_t)WFS_STATE_DIRTY;

    /* Reseal: the state is inside the checksummed image, so without this the
     * case would exercise the checksum path instead of the dirty-mount one. */
    csum = sb + offsetof(struct wfs_superblock, checksum);
    csum[0] = 0;
    csum[1] = 0;
    csum[2] = 0;
    csum[3] = 0;
    c = wfs_checksum_struct(
        k_uuid, 0u, sb, WFS_SUPER_SIZE, (uint32_t)offsetof(struct wfs_superblock, checksum));
    csum[0] = (uint8_t)(c & 0xFFu);
    csum[1] = (uint8_t)((c >> 8) & 0xFFu);
    csum[2] = (uint8_t)((c >> 16) & 0xFFu);
    csum[3] = (uint8_t)((c >> 24) & 0xFFu);

    expect(run_mount(&ctx, &vol) == 0, "a dirty volume still mounts");
    expect(vol.super.needs_replay == 1u, "and reports that replay is owed");
    expect(vol.super.read_only == 1u, "and is read-only until it happens");

    wfs_stub_teardown();
}

/* Every permitted block size must mount: the block layer adopts the volume's
 * size after reading block 0 at the default. */
static void test_every_block_size_mounts(void) {
    static const uint32_t sizes[3] = {4096u, 8192u, 16384u};
    uint32_t i;

    for (i = 0; i < 3u; ++i) {
        wfs_mount_ctx_t ctx;
        wfs_volume_t vol;

        if (build_volume(64ull * 1024ull * 1024ull, sizes[i]) != 0) {
            expect(0, "build a volume");
            continue;
        }
        expect(run_mount(&ctx, &vol) == 0, "mount at a permitted block size");
        expect(vol.super.block_size == sizes[i], "the volume's size was adopted");
        expect(wfs_stub_block()->block_size == sizes[i], "and whole blocks are transferred");
        expect(wfs_stub_last_sectors == sizes[i] / WFS_SECTOR_BYTES,
               "the request names the block's whole run of sectors");
        wfs_stub_teardown();
    }
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_mount_reads_a_volume_mkfs_wrote),
    WASMOS_TEST_CASE(test_mount_requests_exactly_the_blocks_it_should),
    WASMOS_TEST_CASE(test_the_staged_block_is_a_cache),
    WASMOS_TEST_CASE(test_the_root_object_reads_back_as_mkfs_wrote_it),
    WASMOS_TEST_CASE(test_an_unallocated_object_is_refused),
    WASMOS_TEST_CASE(test_an_object_record_is_bound_to_its_slot),
    WASMOS_TEST_CASE(test_group_descriptors_read_back),
    WASMOS_TEST_CASE(test_mount_refuses_a_volume_with_a_bad_descriptor),
    WASMOS_TEST_CASE(test_mount_refuses_a_device_that_holds_no_volume),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_mount),
    WASMOS_TEST_CASE(test_a_failed_send_fails_the_mount),
    WASMOS_TEST_CASE(test_a_dirty_volume_mounts_read_only),
    WASMOS_TEST_CASE(test_every_block_size_mounts),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_mount: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_mount: %d checks passed\n", g_checks);
    return 0;
}
