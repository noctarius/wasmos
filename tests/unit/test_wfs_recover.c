/* Host unit test for crash recovery (wfs_recover.h, §21).
 *
 * A crash is modelled as a STOPPED DEVICE: the transaction runs for a chosen
 * number of block requests and every request past that fails, so the image holds
 * exactly the writes that landed first. The volume is then remounted from that
 * image with a cold block cache, which is what a reboot is.
 *
 * The alternative -- hand-building a log and asserting the reader accepts it --
 * would test the test. These cases run the real writer, so a layout the writer
 * and the reader stop agreeing about fails here rather than at the next crash.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_block.h"
#include "wfs_format.h"
#include "wfs_journal.h"
#include "wfs_mount.h"
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

static uint32_t rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

static uint64_t rd64(const uint8_t* p, uint32_t off) {
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4) << 32);
}

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0xa1, 0x0e, 0x74, 0x33, 0xc9, 0x62, 0x48, 0xd0, 0x95, 0x1b, 0x6f, 0x27, 0x8a, 0x4c, 0xdd, 0x03};
#define TEST_NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

/* The block requests one transaction issues, in order, so a case names the point
 * it crashes at rather than a bare number. Verified by test_wfs_journal's
 * request-sequence assertions and by every case below failing loudly if the
 * writer's I/O pattern changes.
 *
 *   1 the block image into the log
 *   2 the descriptor naming it
 *   3 the revoke record, when the transaction has one
 *   4 the COMMIT block
 *   5 the checkpoint's read of the image
 *   6 the checkpoint's write to the target
 *   7 the tail
 */
#define CRASH_BEFORE_COMMIT 2u
#define CRASH_AFTER_COMMIT 3u
#define CRASH_AFTER_CHECKPOINT 5u
#define CRASH_AFTER_COMMIT_WITH_REVOKE 4u

static wfs_mkfs_layout_t g_layout;
static const uint8_t k_pattern_seed = 0xC3u;

static int32_t mount_volume(wfs_mount_ctx_t* ctx, wfs_volume_t* vol) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    memset(vol, 0, sizeof(*vol));
    ctx->vol = vol;
    return wfs_stub_run_task(&task, wfs_mount_task, ctx);
}

/* A reboot: the device is answering again and no block is cached. */
static int32_t remount(wfs_mount_ctx_t* ctx, wfs_volume_t* vol) {
    wfs_stub_stop_after = 0u;
    wfs_block_invalidate(wfs_stub_block());
    return mount_volume(ctx, vol);
}

static int32_t mark_dirty(wfs_volume_t* vol) {
    wfs_dirty_ctx_t ctx;
    wasmos_wasm_coroutine_t task;

    memset(&ctx, 0, sizeof(ctx));
    ctx.vol = vol;
    return wfs_stub_run_task(&task, wfs_mark_dirty_task, &ctx);
}

static int block_has_pattern(uint32_t block, uint8_t seed) {
    const uint8_t* p = wfs_stub_image + (size_t)block * wfs_stub_block_size;
    uint32_t i;

    for (i = 0; i < wfs_stub_block_size; ++i) {
        if (p[i] != (uint8_t)(seed + (uint8_t)i)) {
            return 0;
        }
    }
    return 1;
}

static void scribble(uint32_t block, uint8_t value) {
    memset(wfs_stub_image + (size_t)block * wfs_stub_block_size, value, wfs_stub_block_size);
}

/* Run one transaction that journals `target` and optionally revokes `revoke`,
 * with the device stopping after `stop_after` requests.
 *
 * Returns the commit task's status, which is the failure the stopped device
 * produced -- a crash has no return value, but the image it leaves is the point.
 */
static void crash_during_transaction(wfs_volume_t* vol, uint32_t target, uint32_t revoke,
                                     uint32_t stop_after) {
    wfs_txstage_ctx_t sc;
    wfs_txcommit_ctx_t cc;
    wasmos_wasm_coroutine_t task;
    uint8_t* d = wfs_block_data(wfs_stub_block());
    uint32_t i;

    expect_rc(wfs_txn_begin(vol), WASMOS_ERR_NONE, "a transaction opens");
    wfs_stub_reset_counters();
    wfs_stub_stop_after = stop_after;

    for (i = 0; i < vol->super.block_size; ++i) {
        d[i] = (uint8_t)(k_pattern_seed + (uint8_t)i);
    }
    memset(&sc, 0, sizeof(sc));
    sc.vol = vol;
    sc.target = target;
    expect(wfs_stub_run_task(&task, wfs_txn_stage_task, &sc) == 0, "the block is journaled");
    if (revoke) {
        expect_rc(wfs_txn_revoke(vol, revoke), WASMOS_ERR_NONE, "a block is revoked");
    }
    memset(&cc, 0, sizeof(cc));
    cc.vol = vol;
    (void)wfs_stub_run_task(&task, wfs_txn_commit_task, &cc);
}

/* Set up a dirty volume and return the target block a case will journal. */
static uint32_t prepare(wfs_mount_ctx_t* m, wfs_volume_t* vol) {
    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return 0u;
    }
    expect(mount_volume(m, vol) == 0, "the fresh volume mounts");
    /* What a real writer does before its first metadata write: the flag is what
     * tells the next mount a replay may be owed (§4). */
    expect(mark_dirty(vol) == 0, "the volume is marked dirty");
    return g_layout.bitmap_start;
}

/* The whole point of the log. A transaction whose COMMIT landed is finished by
 * the next mount, even though the checkpoint never ran. */
static void test_a_committed_transaction_is_replayed_at_the_next_mount(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target = prepare(&m, &vol);

    if (!target) {
        return;
    }
    crash_during_transaction(&vol, target, 0u, CRASH_AFTER_COMMIT);
    expect(!block_has_pattern(target, k_pattern_seed),
           "the crash left the target without the transaction's bytes");

    expect(remount(&m, &vol) == 0, "the volume remounts");
    expect_u32(m.replayed, 1u, "and one image was replayed");
    expect(block_has_pattern(target, k_pattern_seed), "landing the transaction's bytes");
    expect_u32(vol.super.needs_replay, 0u, "the replay is no longer owed");
    expect_rc(m.journal_err, WASMOS_ERR_NONE, "and the log reported nothing");

    /* The gate the phase-2 writers still need: they do not run inside
     * transactions, so a crash may have left metadata the log never recorded and
     * no replay repairs. */
    expect_u32(vol.super.read_only, 1u, "the volume stays read-only");

    wfs_stub_teardown();
}

/* A crash cannot be relied on to record its own failure, which is why there is
 * no ABORT record: a transaction is replayable only when a COMMIT carries its
 * sequence (§14). */
static void test_an_uncommitted_transaction_is_discarded(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target = prepare(&m, &vol);
    const uint8_t* js;

    if (!target) {
        return;
    }
    crash_during_transaction(&vol, target, 0u, CRASH_BEFORE_COMMIT);

    expect(remount(&m, &vol) == 0, "the volume remounts");
    expect_u32(m.replayed, 0u, "nothing is replayed");
    expect(!block_has_pattern(target, k_pattern_seed), "and the target is untouched");

    /* The tail must NOT have advanced past a transaction that was discarded, or
     * the next attempt would write under a sequence recovery no longer looks
     * for. */
    js = wfs_stub_image + (size_t)g_layout.journal_start * wfs_stub_block_size;
    expect(rd64(js, (uint32_t)offsetof(struct wfs_journal_super, first_sequence)) == 1u,
           "the tail still names the sequence the attempt used");
    expect(vol.journal.next_sequence == 1u, "as does the remounted volume");

    wfs_stub_teardown();
}

/* A crash between the checkpoint and the tail write leaves a transaction that is
 * durable and already applied. Replay must run again rather than skip it, and
 * running again must land the same bytes -- every image is a whole-block
 * overwrite, which is what makes recovery repeatable (§21). */
static void test_a_transaction_whose_tail_never_advanced_replays_again(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target = prepare(&m, &vol);

    if (!target) {
        return;
    }
    crash_during_transaction(&vol, target, 0u, CRASH_AFTER_CHECKPOINT);
    expect(block_has_pattern(target, k_pattern_seed), "the checkpoint had already run");

    /* Scribbled over so the assertion below distinguishes a replay from the
     * bytes the checkpoint left. */
    scribble(target, 0xEEu);

    expect(remount(&m, &vol) == 0, "the volume remounts");
    expect_u32(m.replayed, 1u, "the image is applied again");
    expect(block_has_pattern(target, k_pattern_seed), "restoring the transaction's bytes");

    /* Now the tail HAS advanced, so a second mount finds nothing. */
    scribble(target, 0xEEu);
    expect(remount(&m, &vol) == 0, "the volume remounts once more");
    expect_u32(m.replayed, 0u, "and replays nothing this time");
    expect(!block_has_pattern(target, k_pattern_seed), "leaving the block as it found it");

    wfs_stub_teardown();
}

/* §21's comparison is `>=`, not `>`: an image journaled by the same transaction
 * that revoked its block must not be replayed either. Without this a block that
 * stopped being metadata and was handed to a file would have stale metadata
 * written over the file's data. */
static void test_a_revoked_block_is_not_replayed(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target = prepare(&m, &vol);

    if (!target) {
        return;
    }
    crash_during_transaction(&vol, target, target, CRASH_AFTER_COMMIT_WITH_REVOKE);

    expect(remount(&m, &vol) == 0, "the volume remounts");
    expect_u32(m.replayed, 0u, "the revoked image is skipped");
    expect(!block_has_pattern(target, k_pattern_seed), "so the block keeps what it holds now");
    expect_u32(vol.super.needs_replay, 0u, "and the replay is discharged");

    wfs_stub_teardown();
}

/* Recovery ABORTS rather than applying part of a transaction: a partial
 * transaction is the one outcome the journal exists to prevent. The volume then
 * mounts read-only for fsck (§21, §24). */
static void test_a_damaged_image_aborts_the_replay(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target = prepare(&m, &vol);
    uint32_t image;

    if (!target) {
        return;
    }
    crash_during_transaction(&vol, target, 0u, CRASH_AFTER_COMMIT);

    /* One byte of the logged image, which the descriptor's checksum covers. */
    image = g_layout.journal_start + WFS_TXN_DESCRIPTOR_BLOCK + 1u;
    wfs_stub_image[(size_t)image * wfs_stub_block_size + 17u] ^= 0xFFu;

    expect(remount(&m, &vol) == 0, "the volume still mounts");
    expect_rc(m.journal_err, WASMOS_ERR_FS_REPLAY, "the replay reports the damaged image");
    expect_u32(vol.super.read_only, 1u, "the volume is read-only");
    expect_u32(vol.super.needs_replay, 1u, "and the replay is still owed");
    expect(!block_has_pattern(target, k_pattern_seed), "no part of the transaction was applied");

    wfs_stub_teardown();
}

/* §15: a volume unmounted cleanly has nothing in its log to apply, and scanning
 * it on every mount costs a full journal read for no result. */
static void test_a_clean_volume_does_not_scan_the_log(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t i;
    int touched_log = 0;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    wfs_stub_reset_counters();
    expect(mount_volume(&m, &vol) == 0, "the clean volume mounts");
    expect_u32(vol.super.needs_replay, 0u, "with no replay owed");
    expect_u32(vol.super.read_only, 0u, "and writable");

    for (i = 0; i < wfs_stub_req_count && i < WFS_STUB_REQ_LOG_MAX; ++i) {
        if (wfs_stub_req_blocks[i] == g_layout.journal_start + WFS_TXN_DESCRIPTOR_BLOCK) {
            touched_log = 1;
        }
    }
    expect(!touched_log, "and the log itself was never walked");
    /* The journal SUPERBLOCK is still read, because a transaction cannot be
     * opened without the log's geometry and tail. */
    expect_u32(vol.journal.loaded, 1u, "though its superblock was read");

    wfs_stub_teardown();
}

/* An unusable log costs the volume its writability, not its readability: every
 * structure a reader touches is intact, and refusing the mount would deny a
 * volume whose data is fine over a region only a writer needs. */
static void test_a_damaged_log_leaves_the_volume_readable(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    wfs_stub_image[(size_t)g_layout.journal_start * wfs_stub_block_size] ^= 0xFFu;

    expect(mount_volume(&m, &vol) == 0, "the volume mounts");
    expect_u32(vol.mounted, 1u, "and is usable");
    expect_rc(m.journal_err, WASMOS_ERR_FS_JOURNAL, "the log is reported unusable");
    expect_u32(vol.super.read_only, 1u, "so the volume refuses writes");
    expect_u32(vol.super.root_object_id, (uint32_t)WFS_OBJECT_ROOT, "the root is still reachable");
    /* The log, not the read-only gate, is what the caller is told about: a
     * volume read-only for a damaged journal and one read-only for a recovered
     * superblock need different repairs. */
    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_FS_JOURNAL, "and no transaction can be opened on it");

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_committed_transaction_is_replayed_at_the_next_mount),
    WASMOS_TEST_CASE(test_an_uncommitted_transaction_is_discarded),
    WASMOS_TEST_CASE(test_a_transaction_whose_tail_never_advanced_replays_again),
    WASMOS_TEST_CASE(test_a_revoked_block_is_not_replayed),
    WASMOS_TEST_CASE(test_a_damaged_image_aborts_the_replay),
    WASMOS_TEST_CASE(test_a_clean_volume_does_not_scan_the_log),
    WASMOS_TEST_CASE(test_a_damaged_log_leaves_the_volume_readable),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_recover: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_recover: %d checks passed\n", g_checks);
    return 0;
}
