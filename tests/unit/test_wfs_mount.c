/* Host unit test for the WFS reactor: the mount, group-descriptor and
 * object-record coroutines (src/drivers/fs_wfs/wfs_mount.c) driven against a
 * volume that mkfs_wfs built.
 *
 * The driver is a stackless-coroutine reactor: a step submits one block request
 * and returns WFS_R_WAIT, and the reactor re-enters it when that request
 * completes. This suite IS the reactor for the duration of a case. It supplies
 * wfs_block_submit_read / _write — the layer's only contact with IPC — serving
 * a RAM image, and everything above them is the driver's own code, unchanged.
 *
 * The sink completes each request before returning WFS_R_WAIT rather than
 * returning DONE directly, so every case drives the real yield-and-resume path
 * instead of a synchronous shortcut that the target will never take.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos_status.h"
#include "wfs_block.h"
#include "wfs_co.h"
#include "wfs_crc32c.h"
#include "wfs_format.h"
#include "wfs_mkfs.h"
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

/* ---- the RAM volume the sink serves -------------------------------------- */

#define REQ_LOG_MAX 64

static uint8_t* g_image;
static uint32_t g_image_blocks;
static uint32_t g_image_block_size;
static uint32_t g_reads; /* requests actually submitted to the device */
static uint32_t g_writes;
static int g_fail_next_io; /* make the next transfer report a device error */

/* Which blocks were requested, in order. A step that let a block number live on
 * the C stack across a yield reads garbage on resume, and the only visible
 * symptom is a request for the wrong block — so the sequence is recorded and
 * asserted rather than merely counted. */
static uint32_t g_req_log[REQ_LOG_MAX];
static uint32_t g_req_count;

static void log_req(uint32_t block) {
    if (g_req_count < REQ_LOG_MAX) {
        g_req_log[g_req_count] = block;
    }
    g_req_count++;
}

wfs_r_t wfs_block_submit_read(wfs_block_t* b, uint32_t block) {
    g_reads++;
    log_req(block);

    b->cur_req_id = b->next_req_id++;
    b->wait_block = block;
    b->wait_is_write = 0;

    if (g_fail_next_io || block >= g_image_blocks) {
        g_fail_next_io = 0;
        wfs_block_complete(b, 0);
        return WFS_R_WAIT;
    }
    memcpy(b->data, g_image + (size_t)block * g_image_block_size, b->block_size);
    wfs_block_complete(b, 1);
    return WFS_R_WAIT;
}

wfs_r_t wfs_block_submit_write(wfs_block_t* b, uint32_t block) {
    g_writes++;
    log_req(block);

    b->cur_req_id = b->next_req_id++;
    b->wait_block = block;
    b->wait_is_write = 1;

    if (g_fail_next_io || block >= g_image_blocks) {
        g_fail_next_io = 0;
        wfs_block_complete(b, 0);
        return WFS_R_WAIT;
    }
    memcpy(g_image + (size_t)block * g_image_block_size, b->data, b->block_size);
    wfs_block_complete(b, 1);
    return WFS_R_WAIT;
}

/* ---- building the volume ------------------------------------------------- */

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0x30, 0x91, 0x4c, 0x02, 0xbb, 0x77, 0x41, 0x18, 0x8e, 0x5a, 0x22, 0xd9, 0x6f, 0x40, 0x13, 0xc7};

static int sink_write(void* ctx, uint32_t block, const void* data, uint32_t len) {
    (void)ctx;
    if (block >= g_image_blocks) {
        return -1;
    }
    memcpy(g_image + (size_t)block * len, data, len);
    return 0;
}

static wfs_mkfs_layout_t g_layout;

static int build_volume(uint64_t size, uint32_t block_size) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;

    memset(&params, 0, sizeof(params));
    params.size_bytes = size;
    params.block_size = block_size;
    params.now_ns = 1750000000000000000ull;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);

    if (wfs_mkfs_plan(&params, &g_layout) != WASMOS_ERR_NONE) {
        return -1;
    }
    free(g_image);
    g_image = (uint8_t*)calloc(g_layout.total_blocks, g_layout.block_size);
    if (!g_image) {
        return -1;
    }
    g_image_blocks = g_layout.total_blocks;
    g_image_block_size = g_layout.block_size;

    sink.ctx = NULL;
    sink.write_block = sink_write;
    if (wfs_mkfs_format(&params, &sink, &g_layout) != WASMOS_ERR_NONE) {
        return -1;
    }

    g_reads = 0;
    g_writes = 0;
    g_req_count = 0;
    g_fail_next_io = 0;
    return 0;
}

static void teardown(void) {
    free(g_image);
    g_image = NULL;
}

/* Drive a step to completion the way the reactor does, and report how many
 * times it had to be re-entered. */
static wfs_r_t drive_mount(wfs_mount_ctx_t* ctx, wfs_block_t* b, wfs_volume_t* vol,
                           uint32_t* resumes) {
    wfs_r_t r;
    uint32_t n = 0;

    for (;;) {
        r = wfs_mount_step(ctx, b, vol);
        if (r != WFS_R_WAIT) {
            break;
        }
        n++;
        if (n > 100000u) {
            break; /* a step that never progresses must not hang the suite */
        }
    }
    if (resumes) {
        *resumes = n;
    }
    return r;
}

static wfs_r_t drive_object(wfs_object_ctx_t* ctx, wfs_block_t* b, const wfs_volume_t* vol) {
    wfs_r_t r;
    uint32_t n = 0;

    while ((r = wfs_object_step(ctx, b, vol)) == WFS_R_WAIT) {
        if (++n > 100000u) {
            break;
        }
    }
    return r;
}

static wfs_r_t drive_group(wfs_group_ctx_t* ctx, wfs_block_t* b, const wfs_volume_t* vol) {
    wfs_r_t r;
    uint32_t n = 0;

    while ((r = wfs_group_step(ctx, b, vol)) == WFS_R_WAIT) {
        if (++n > 100000u) {
            break;
        }
    }
    return r;
}

static void init_block(wfs_block_t* b) {
    wfs_block_configure(b, 1, 2);
}

#define VOL_16M (16ull * 1024ull * 1024ull)

/* ---- mount --------------------------------------------------------------- */

static void test_mount_reads_a_volume_mkfs_wrote(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    uint32_t resumes = 0;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));

    expect(drive_mount(&ctx, &b, &vol, &resumes) == WFS_R_DONE, "mount completes");
    expect(vol.mounted == 1u, "the volume is mounted");

    /* The whole point of the reactor: the step yielded and was re-entered,
     * rather than blocking until it had everything. */
    expect(resumes > 0u, "mount yielded at least once and resumed");
    expect(g_reads > 0u, "mount read from the device");
    expect(g_writes == 0u, "mounting a clean volume writes nothing");

    expect(vol.super.block_size == 4096u, "block size");
    expect(vol.super.total_blocks == g_layout.total_blocks, "block count");
    expect(vol.super.group_count == g_layout.group_count, "group count");
    expect(vol.super.root_object_id == WFS_OBJECT_ROOT, "root object id");
    expect(vol.super.needs_replay == 0u, "a fresh volume needs no replay");
    expect(vol.super.read_only == 0u, "and mounts writable");
    expect(memcmp(vol.super.uuid, k_uuid, WFS_UUID_LEN) == 0, "uuid");

    /* The context is left reusable: cont back to 0 on the completion path. */
    expect(ctx.cont == 0, "the mount context is reset for reuse");

    teardown();
}

/* Regression: 2026-08-24-wfs-yield-local — wfs_group_step held the block number
 * it was about to read in a C LOCAL. A yielding macro returns out of the
 * function and the reactor re-enters at the resume label, so on resume that
 * local was read uninitialised: the staged-block cache check compared against
 * garbage and the step reissued a read for whatever the stack held. The visible
 * cost is a request for the wrong block, so this pins the SEQUENCE of blocks
 * mount asks for, not merely the count. -Wsometimes-uninitialized caught this
 * instance; it cannot see through every arrangement of the same mistake.
 */
static void test_mount_requests_exactly_the_blocks_it_should(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(g_layout.group_count == 1u, "a 16 MiB volume is one group");

    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount completes");

    /* Block 0 for the superblock, then the one descriptor-table block. Nothing
     * else: mount does not read the object table or the bitmaps. */
    expect(g_req_count == 2u, "mount reads exactly two blocks");
    if (g_req_count >= 2u) {
        expect(g_req_log[0] == 0u, "the first read is block 0, for the superblock");
        expect(g_req_log[1] == g_layout.group_table_start,
               "the second read is the group descriptor table");
    }

    teardown();
}

/* The staged block is a one-block cache, so a step that asks for the block
 * already staged must not go to the device. A reactor without that pays a read
 * for every record it touches in the same block. */
static void test_the_staged_block_is_a_cache(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    wfs_object_ctx_t o;
    uint32_t after_mount;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount");

    memset(&o, 0, sizeof(o));
    o.object_id = WFS_OBJECT_ROOT;
    expect(drive_object(&o, &b, &vol) == WFS_R_DONE, "read the root object");
    after_mount = g_reads;

    /* The same record again: it is in the block already staged. */
    memset(&o, 0, sizeof(o));
    o.object_id = WFS_OBJECT_ROOT;
    expect(drive_object(&o, &b, &vol) == WFS_R_DONE, "read the root object again");
    expect(g_reads == after_mount, "a second read of a staged block costs no device request");

    teardown();
}

/* ---- object records ------------------------------------------------------ */

static void test_the_root_object_reads_back_as_mkfs_wrote_it(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    wfs_object_ctx_t o;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount");

    memset(&o, 0, sizeof(o));
    o.object_id = WFS_OBJECT_ROOT;
    expect(drive_object(&o, &b, &vol) == WFS_R_DONE, "the root record reads and verifies");

    expect(o.out.object_id == WFS_OBJECT_ROOT, "object id");
    expect(o.out.type == WFS_TYPE_DIR, "the root is a directory");
    expect(o.out.link_count == 2u, "link count is 2");
    expect(o.out.size == 4096u, "size is one directory block");
    expect(o.out.extent_count == 1u, "one extent");
    expect(o.out.extents[0].physical_block == g_layout.root_data_block,
           "the extent points at the root's data block");
    expect(o.out.extents[0].length == 1u, "the extent is one block");
    expect(o.out.extent_tree_block == 0u, "an inline extent needs no tree");
    expect((o.out.flags & WFS_OBJ_INLINE_DATA) == 0u, "the root is not inline-data");

    teardown();
}

static void test_an_unallocated_object_is_not_found(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    wfs_object_ctx_t o;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount");

    /* Id 0 is never an object, and an id past the table cannot be one. */
    memset(&o, 0, sizeof(o));
    o.object_id = WFS_OBJECT_INVALID;
    expect(drive_object(&o, &b, &vol) == WFS_R_ERR, "object 0 is refused");
    expect_rc(b.err, WASMOS_ERR_FS_NOT_FOUND, "object 0");

    memset(&o, 0, sizeof(o));
    o.object_id = vol.super.total_objects;
    expect(drive_object(&o, &b, &vol) == WFS_R_ERR, "an id past the table is refused");
    expect_rc(b.err, WASMOS_ERR_FS_NOT_FOUND, "an id past the table");

    /* An id inside the table but never allocated has a zeroed record, whose
     * checksum cannot match: it is corrupt, not merely absent. Distinguishing
     * the two is the bitmap's job, not the record reader's. */
    memset(&o, 0, sizeof(o));
    o.object_id = WFS_OBJECT_FIRST;
    expect(drive_object(&o, &b, &vol) == WFS_R_ERR, "an unallocated record does not verify");
    expect_rc(b.err, WASMOS_ERR_FS_CHECKSUM, "an unallocated record");

    teardown();
}

/* A record is checksummed under its object id, so one moved to another slot
 * must not verify there (§13). */
static void test_an_object_record_is_bound_to_its_slot(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    wfs_object_ctx_t o;
    uint8_t* table;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    table = g_image + (size_t)g_layout.object_table_start * g_layout.block_size;
    /* Copy the root's record into the next slot verbatim. */
    memcpy(table + (size_t)2u * WFS_OBJECT_SIZE, table + WFS_OBJECT_SIZE, WFS_OBJECT_SIZE);

    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount");

    memset(&o, 0, sizeof(o));
    o.object_id = 2u;
    expect(drive_object(&o, &b, &vol) == WFS_R_ERR, "a transplanted record is refused");
    expect_rc(b.err, WASMOS_ERR_FS_CHECKSUM, "a record in the wrong slot");

    teardown();
}

/* ---- group descriptors --------------------------------------------------- */

static void test_group_descriptors_read_back(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    wfs_group_ctx_t g;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount");

    memset(&g, 0, sizeof(g));
    g.group = 0u;
    expect(drive_group(&g, &b, &vol) == WFS_R_DONE, "group 0's descriptor verifies");
    expect(g.out.block_bitmap == g_layout.bitmap_start, "it names its block bitmap");
    expect(g.out.object_bitmap == g_layout.bitmap_start + 1u, "it names its object bitmap");
    expect(g.out.object_table == g_layout.object_table_start, "it names its object table slice");
    expect(g.out.free_blocks == g_layout.free_blocks, "one group's free count is the volume's");

    memset(&g, 0, sizeof(g));
    g.group = vol.super.group_count;
    expect(drive_group(&g, &b, &vol) == WFS_R_ERR, "a group past the table is refused");
    expect_rc(b.err, WASMOS_ERR_FS_CORRUPT, "a group index past the count");

    teardown();
}

/* Mount verifies every descriptor before declaring the volume usable: one that
 * fails names a bitmap block nothing vouches for, and every allocation in that
 * group would address it. */
static void test_mount_refuses_a_volume_with_a_bad_descriptor(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    uint8_t* table;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    table = g_image + (size_t)g_layout.group_table_start * g_layout.block_size;
    table[4] = (uint8_t)(table[4] ^ 0x10u);

    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_ERR, "the mount fails");
    expect_rc(b.err, WASMOS_ERR_FS_CHECKSUM, "a corrupted group descriptor");
    expect(vol.mounted == 0u, "and the volume is not marked mounted");

    teardown();
}

/* ---- mount failure paths ------------------------------------------------- */

static void test_mount_refuses_a_device_that_holds_no_volume(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    memset(g_image, 0, g_layout.block_size);

    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_ERR, "the mount fails");
    expect_rc(b.err, WASMOS_ERR_FS_BAD_MAGIC, "an unformatted device");
    expect(vol.mounted == 0u, "nothing is mounted");

    teardown();
}

static void test_a_device_error_fails_the_mount(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    g_fail_next_io = 1;

    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_ERR, "the mount fails");
    expect_rc(b.err, WASMOS_ERR_FS_IO, "a failed transfer");
    /* A failed transfer must not leave its bytes looking cached, or the next
     * step would parse whatever the device left in the buffer. */
    expect(b.staged_block == WFS_BLOCK_NONE, "the cache tag is cleared on failure");
    expect(vol.mounted == 0u, "nothing is mounted");

    teardown();
}

/* A volume that was not unmounted cleanly has metadata in the journal that the
 * on-disk structures do not reflect. Until replay exists it mounts read-only,
 * which is the conservative half of the contract rather than silently serving
 * stale metadata. */
static void test_a_dirty_volume_mounts_read_only(void) {
    wfs_block_t b;
    wfs_volume_t vol;
    wfs_mount_ctx_t ctx;
    uint8_t* sb;
    uint8_t* csum;
    uint32_t c;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    sb = g_image + WFS_SUPER_OFFSET;
    sb[offsetof(struct wfs_superblock, state)] = (uint8_t)WFS_STATE_DIRTY;

    /* Reseal: the state change is inside the checksummed image, so without this
     * the case would exercise the checksum path instead of the dirty-mount one. */
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

    init_block(&b);
    memset(&ctx, 0, sizeof(ctx));
    memset(&vol, 0, sizeof(vol));
    expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "a dirty volume still mounts");
    expect(vol.super.needs_replay == 1u, "and reports that replay is owed");
    expect(vol.super.read_only == 1u, "and is read-only until it happens");

    teardown();
}

/* Every permitted block size must mount, because the block layer adopts the
 * size from the superblock after reading block 0 at the default. */
static void test_every_block_size_mounts(void) {
    static const uint32_t sizes[3] = {4096u, 8192u, 16384u};
    uint32_t i;

    for (i = 0; i < 3u; ++i) {
        wfs_block_t b;
        wfs_volume_t vol;
        wfs_mount_ctx_t ctx;

        if (build_volume(64ull * 1024ull * 1024ull, sizes[i]) != 0) {
            expect(0, "build a volume");
            continue;
        }
        init_block(&b);
        memset(&ctx, 0, sizeof(ctx));
        memset(&vol, 0, sizeof(vol));

        expect(drive_mount(&ctx, &b, &vol, NULL) == WFS_R_DONE, "mount at a permitted block size");
        expect(vol.super.block_size == sizes[i], "the block layer adopted the volume's size");
        expect(b.block_size == sizes[i], "and the staging buffer transfers whole blocks");
        teardown();
    }
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_mount_reads_a_volume_mkfs_wrote),
    WASMOS_TEST_CASE(test_mount_requests_exactly_the_blocks_it_should),
    WASMOS_TEST_CASE(test_the_staged_block_is_a_cache),
    WASMOS_TEST_CASE(test_the_root_object_reads_back_as_mkfs_wrote_it),
    WASMOS_TEST_CASE(test_an_unallocated_object_is_not_found),
    WASMOS_TEST_CASE(test_an_object_record_is_bound_to_its_slot),
    WASMOS_TEST_CASE(test_group_descriptors_read_back),
    WASMOS_TEST_CASE(test_mount_refuses_a_volume_with_a_bad_descriptor),
    WASMOS_TEST_CASE(test_mount_refuses_a_device_that_holds_no_volume),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_mount),
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
