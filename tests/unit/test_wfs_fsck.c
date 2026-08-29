/* Host unit test for the WFS consistency checker (wfs_fsck.h, §24).
 *
 * The volume under test is one mkfs_wfs actually produced, never a hand-built
 * image: a checker validated against a fixture some test wrote agrees with the
 * test rather than with the format, and would keep passing if the formatter's
 * layout drifted away from the driver's.
 *
 * Two properties matter and they pull in opposite directions. A clean volume
 * must produce NO findings -- a checker that cries wolf is one nobody runs --
 * and each specific damage must produce the finding that names it, rather than
 * some other finding that happens to fire. Every damage case below therefore
 * asserts the counter for its own class, not merely that something was reported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_format.h"
#include "wfs_fsck.h"

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

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0x51, 0x0d, 0x9a, 0x77, 0x2c, 0x41, 0x4e, 0x8b, 0x93, 0x22, 0x1c, 0x6d, 0x05, 0xb8, 0x37, 0xe2};
#define TEST_NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

static wfs_mkfs_layout_t g_layout;

/* fsck's I/O over the fixture's in-memory image. */
static wasmos_error_code_t img_read(void* user, uint32_t block, void* out, uint32_t len) {
    (void)user;
    if ((uint64_t)block * wfs_stub_block_size + len >
        (uint64_t)wfs_stub_blocks * wfs_stub_block_size) {
        return WASMOS_ERR_FS_IO;
    }
    memcpy(out, wfs_stub_image + (size_t)block * wfs_stub_block_size, len);
    return WASMOS_ERR_NONE;
}

static wasmos_error_code_t img_write(void* user, uint32_t block, const void* in, uint32_t len) {
    (void)user;
    if ((uint64_t)block * wfs_stub_block_size + len >
        (uint64_t)wfs_stub_blocks * wfs_stub_block_size) {
        return WASMOS_ERR_FS_IO;
    }
    memcpy(wfs_stub_image + (size_t)block * wfs_stub_block_size, in, len);
    return WASMOS_ERR_NONE;
}

/* Re-seal the superblock after damaging a field.
 *
 * Without this a "wrong counter" case would not test a wrong counter at all: the
 * checksum would fail, the volume would not parse, and the run would stop at the
 * superblock having never reached the counters. */
static void reseal_super(void) {
    uint8_t* sb = wfs_stub_image + WFS_SUPER_OFFSET;
    uint32_t sum = wfs_checksum_struct(
        k_uuid, 0u, sb, WFS_SUPER_SIZE, offsetof(struct wfs_superblock, checksum));

    wfs_wr32(sb, offsetof(struct wfs_superblock, checksum), sum);
}

static int build(void) {
    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return -1;
    }
    return 0;
}

static wasmos_error_code_t run_check(wfs_fsck_report_t* rep) {
    wfs_fsck_io_t io = {img_read, 0, 0}; /* no write_block: check only */

    return wfs_fsck_run(&io, 0, 0, rep);
}

static wasmos_error_code_t run_repair(wfs_fsck_report_t* rep) {
    wfs_fsck_io_t io = {img_read, img_write, 0};

    return wfs_fsck_run(&io, 0, 0, rep);
}

/* A volume straight out of mkfs must be reported consistent. Every other case
 * here is only meaningful against this baseline: a checker that reported
 * findings on a pristine volume could "detect" anything. */
static void test_a_fresh_volume_is_consistent(void) {
    wfs_fsck_report_t rep;

    if (build() != 0) {
        return;
    }
    expect(run_check(&rep) == WASMOS_ERR_NONE, "a fresh volume checks clean");
    expect_u32(rep.super_errors, 0u, "no superblock findings");
    expect_u32(rep.group_errors, 0u, "no group findings");
    expect_u32(rep.object_errors, 0u, "no object findings");
    expect_u32(rep.extent_errors, 0u, "no extent findings");
    expect_u32(rep.dir_errors, 0u, "no directory findings");
    expect_u32(rep.bitmap_errors, 0u, "the bitmaps agree with the walk");
    expect_u32(rep.counter_errors, 0u, "the counters agree with the bitmaps");
    expect(rep.objects_in_use >= 1u, "the root object is accounted for");
    expect(rep.blocks_in_use >= 1u, "the metadata regions are accounted for");
    wfs_stub_teardown();
}

/* A check-only run must not write. The whole point of the default is that a
 * damaged volume can be inspected without being altered. */
static void test_a_check_only_run_changes_nothing(void) {
    wfs_fsck_report_t rep;
    uint8_t* before;
    size_t bytes;

    if (build() != 0) {
        return;
    }
    bytes = (size_t)wfs_stub_blocks * wfs_stub_block_size;
    before = malloc(bytes);
    if (!before) {
        expect(0, "allocate a copy of the image");
        wfs_stub_teardown();
        return;
    }
    memcpy(before, wfs_stub_image, bytes);
    /* Damage a counter so the run has something it WOULD repair. */
    wfs_wr64(wfs_stub_image + WFS_SUPER_OFFSET, offsetof(struct wfs_superblock, free_blocks), 1u);
    reseal_super();
    memcpy(before + WFS_SUPER_OFFSET, wfs_stub_image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE);

    (void)run_check(&rep);
    expect(rep.counter_errors > 0u, "the damaged counter is reported");
    expect_u32(rep.repaired, 0u, "and nothing was repaired");
    expect(memcmp(before, wfs_stub_image, bytes) == 0, "the image is byte-for-byte unchanged");
    free(before);
    wfs_stub_teardown();
}

/* The repair §24 defines: counters are derived, so a wrong one is recomputed
 * from the bitmap rather than believed. */
static void test_a_wrong_free_counter_is_recomputed(void) {
    wfs_fsck_report_t rep;
    uint64_t want;

    if (build() != 0) {
        return;
    }
    want =
        wfs_rd64(wfs_stub_image + WFS_SUPER_OFFSET, offsetof(struct wfs_superblock, free_blocks));
    wfs_wr64(wfs_stub_image + WFS_SUPER_OFFSET,
             offsetof(struct wfs_superblock, free_blocks),
             want + 4321u);
    reseal_super();

    expect(run_repair(&rep) == WASMOS_ERR_NONE, "the repair run completes");
    expect(rep.counter_errors > 0u, "the counter is reported wrong");
    expect(rep.repaired > 0u, "and something was written back");
    expect_u32((uint32_t)wfs_rd64(wfs_stub_image + WFS_SUPER_OFFSET,
                                  offsetof(struct wfs_superblock, free_blocks)),
               (uint32_t)want,
               "the counter is back to what the bitmap says");

    /* And the volume now checks clean, which is the property a repair owes:
     * running it twice must reach a fixed point. */
    expect(run_check(&rep) == WASMOS_ERR_NONE, "the repaired volume checks clean");
    expect_u32(rep.counter_errors, 0u, "with no counter findings left");
    wfs_stub_teardown();
}

/* A block marked free while an object holds it is the discrepancy the bitmaps
 * exist to prevent, and the walk is what settles it. */
static void test_a_bitmap_bit_is_rebuilt_from_the_walk(void) {
    wfs_fsck_report_t rep;
    uint8_t* bitmap;

    if (build() != 0) {
        return;
    }
    bitmap = wfs_stub_image + (size_t)g_layout.bitmap_start * wfs_stub_block_size;
    expect((bitmap[0] & 1u) != 0u, "block 0 starts out marked allocated");
    bitmap[0] &= (uint8_t)~1u; /* claim block 0 is free; it holds the superblock */

    expect(run_repair(&rep) == WASMOS_ERR_NONE, "the repair run completes");
    expect(rep.bitmap_errors > 0u, "the freed bit is reported");
    expect((wfs_stub_image[(size_t)g_layout.bitmap_start * wfs_stub_block_size] & 1u) != 0u,
           "and the bit is set again from the walk");
    wfs_stub_teardown();
}

/* Structural damage is REPORTED, never rewritten -- and it must cost the volume
 * its clean state, or the next mount would write over damage nothing repaired. */
static void test_a_damaged_object_record_is_reported_and_not_cleared(void) {
    wfs_fsck_report_t rep;
    uint8_t* rec;

    if (build() != 0) {
        return;
    }
    /* The ROOT's record, corrupted so its checksum fails. Object id 1, not slot
     * 0: ids 0..15 are reserved (§25), carry no record, and are skipped -- so
     * damaging slot 0 would test nothing. */
    rec = wfs_stub_image + (size_t)g_layout.object_table_start * wfs_stub_block_size +
          WFS_OBJECT_SIZE;
    rec[offsetof(struct wfs_object, mode)] ^= 0xFFu;

    expect(run_repair(&rep) == WASMOS_ERR_FS_CORRUPT, "the run reports corruption");
    expect(rep.object_errors > 0u, "the object record is named");
    expect_u32(rep.cleared_state, 0u, "and the volume is NOT marked clean");
    wfs_stub_teardown();
}

/* A directory whose stride would walk off its block must be refused rather than
 * followed: a zero stride is what makes a scan never terminate. */
static void test_a_bad_directory_stride_is_reported(void) {
    wfs_fsck_report_t rep;
    uint8_t* dir;
    uint32_t block;

    if (build() != 0) {
        return;
    }
    /* The root directory's first data block: mkfs places it immediately after
     * the object table and the bitmaps, and the walk finds it through the root
     * record, so the test only has to damage it. */
    block = g_layout.first_data_block;
    dir = wfs_stub_image + (size_t)block * wfs_stub_block_size;
    wfs_wr16(dir, offsetof(struct wfs_dir_entry, record_length), 0u);

    expect(run_check(&rep) == WASMOS_ERR_FS_CORRUPT, "the run reports corruption");
    expect(rep.dir_errors > 0u, "the directory block is named");
    wfs_stub_teardown();
}

/* The state §4 defines as "mount read-only and run fsck" is what a successful
 * run clears; that is the whole point of running one. */
static void test_a_successful_run_clears_the_state(void) {
    wfs_fsck_report_t rep;
    uint8_t* sb = 0;
    uint32_t sum;

    if (build() != 0) {
        return;
    }
    sb = wfs_stub_image + WFS_SUPER_OFFSET;
    wfs_wr32(sb, offsetof(struct wfs_superblock, state), (uint32_t)WFS_STATE_ERROR);
    sum = wfs_checksum_struct(
        k_uuid, 0u, sb, WFS_SUPER_SIZE, offsetof(struct wfs_superblock, checksum));
    wfs_wr32(sb, offsetof(struct wfs_superblock, checksum), sum);

    expect(run_repair(&rep) == WASMOS_ERR_NONE, "the run completes");
    expect_u32(rep.state_before, (uint32_t)WFS_STATE_ERROR, "it started from ERROR");
    expect_u32(rep.cleared_state, 1u, "and cleared the state");
    expect_u32((uint32_t)wfs_rd32(wfs_stub_image + WFS_SUPER_OFFSET,
                                  offsetof(struct wfs_superblock, state)),
               (uint32_t)WFS_STATE_CLEAN,
               "the volume now says CLEAN on disk");
    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_fresh_volume_is_consistent),
    WASMOS_TEST_CASE(test_a_check_only_run_changes_nothing),
    WASMOS_TEST_CASE(test_a_wrong_free_counter_is_recomputed),
    WASMOS_TEST_CASE(test_a_bitmap_bit_is_rebuilt_from_the_walk),
    WASMOS_TEST_CASE(test_a_damaged_object_record_is_reported_and_not_cleared),
    WASMOS_TEST_CASE(test_a_bad_directory_stride_is_reported),
    WASMOS_TEST_CASE(test_a_successful_run_clears_the_state),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_fsck: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_fsck: %d checks passed\n", g_checks);
    return 0;
}
