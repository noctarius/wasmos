/* Host unit test for recording a volume's mount state (wfs_sync.h, §4).
 *
 * The assertion that matters is not that a byte changed: it is that a REMOUNT of
 * the image sees the volume as needing replay and mounts it read-only. That is
 * the whole purpose of the flag while phase-2 writes are not crash-safe, and it
 * is what a crash would actually exercise.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_super.h"
#include "wfs_sync.h"

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

static int32_t mount_volume(wfs_mount_ctx_t* ctx, wfs_volume_t* vol) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    memset(vol, 0, sizeof(*vol));
    ctx->vol = vol;
    return wfs_stub_run_task(&task, wfs_mount_task, ctx);
}

static int32_t run_mark(wfs_dirty_ctx_t* ctx, wfs_volume_t* vol) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    ctx->vol = vol;
    return wfs_stub_run_task(&task, wfs_mark_dirty_task, ctx);
}

/* Marking a volume dirty must be visible to the NEXT mount, which is the only
 * reader that matters: a crash is exactly the case where nothing else runs. */
static void test_a_marked_volume_remounts_read_only(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    wfs_dirty_ctx_t d;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "the fresh volume mounts");
    expect_u32(vol.super.needs_replay, 0u, "and needs no replay");
    expect_u32(vol.super.read_only, 0u, "and is writable");

    expect(run_mark(&d, &vol) == 0, "marking it dirty completes");
    expect_u32(vol.dirty_marked, 1u, "and is recorded on the volume");

    /* The remount is the assertion. Reading the byte back would only prove the
     * write landed, not that the mount path acts on it. */
    expect(mount_volume(&m, &vol) == 0, "it still mounts");
    expect_u32(vol.super.state, (uint32_t)WFS_STATE_DIRTY, "the state says dirty");
    expect_u32(vol.super.needs_replay, 1u, "so a replay is owed");
    expect_u32(vol.super.read_only, 1u, "and the volume is read-only until it happens");

    wfs_stub_teardown();
}

/* Once per mount, not once per write: the second call must cost no device I/O. */
static void test_marking_twice_costs_one_write(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    wfs_dirty_ctx_t d;
    uint32_t after_first;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");

    expect(run_mark(&d, &vol) == 0, "the first marking completes");
    after_first = wfs_stub_req_count;
    expect(after_first > 0u, "and reached the device");

    expect(run_mark(&d, &vol) == 0, "the second marking completes");
    expect_u32(wfs_stub_req_count, after_first, "without touching the device again");

    wfs_stub_teardown();
}

/* A read-only volume must not be marked dirty: that would make a mount which was
 * never written look like an interrupted write, and cost the next mount its
 * writability for no reason. */
static void test_a_read_only_volume_is_not_marked(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    wfs_dirty_ctx_t d;
    uint32_t before;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    vol.super.read_only = 1u;
    before = wfs_stub_req_count;

    expect_rc((wasmos_error_code_t)run_mark(&d, &vol),
              WASMOS_ERR_FS_READ_ONLY,
              "a read-only volume refuses to be marked");
    expect_u32(vol.dirty_marked, 0u, "nothing is recorded");
    expect_u32(wfs_stub_req_count, before, "and no block is written");

    wfs_stub_teardown();
}

/* The superblock is resealed, not merely patched: its checksum covers `state`,
 * so a marking that skipped the reseal would leave a volume that no longer
 * validates at all -- which reads as a corrupt filesystem rather than a dirty
 * one. */
static void test_the_marked_superblock_still_validates(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    wfs_dirty_ctx_t d;
    wfs_super_t parsed;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    expect(run_mark(&d, &vol) == 0, "mark it dirty");

    memset(&parsed, 0, sizeof(parsed));
    expect_rc(wfs_super_parse(wfs_stub_image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &parsed),
              WASMOS_ERR_NONE,
              "the on-disk superblock verifies");
    expect_u32(parsed.state, (uint32_t)WFS_STATE_DIRTY, "with the dirty state");

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_marked_volume_remounts_read_only),
    WASMOS_TEST_CASE(test_marking_twice_costs_one_write),
    WASMOS_TEST_CASE(test_a_read_only_volume_is_not_marked),
    WASMOS_TEST_CASE(test_the_marked_superblock_still_validates),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_sync: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_sync: %d checks passed\n", g_checks);
    return 0;
}
