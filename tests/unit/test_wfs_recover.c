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
#include "wfs_endian.h"
#include "wfs_format.h"
#include "wfs_journal.h"
#include "wfs_bitmap.h"
#include "wfs_crc32c.h"
#include "wfs_mount.h"
#include "wfs_namespace.h"
#include "wfs_ops.h"
#include "wfs_path.h"
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
    0xa1, 0x0e, 0x74, 0x33, 0xc9, 0x62, 0x48, 0xd0, 0x95, 0x1b, 0x6f, 0x27, 0x8a, 0x4c, 0xdd, 0x03};
#define TEST_NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)
/* Large enough that group 1 exists and so carries a backup (§5): a group spans
 * 128 MiB at a 4096-byte block size. */
#define VOL_132M (132ull * 1024ull * 1024ull)

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

/* Journal the staged block, in the shape every participant uses: begin the
 * stage, await it, take it. Nothing here is a shortcut around the driver's own
 * path -- a helper that wrote the log block itself would stop testing the one
 * thing that matters, which is where a metadata write actually lands. */
typedef struct {
    int pc;
    wfs_volume_t* vol;
    uint32_t target;
    wasmos_error_code_t err;
} stage_ctx_t;

static int32_t stage_task(void* user, uintptr_t* out_value) {
    stage_ctx_t* ctx = (stage_ctx_t*)user;

    (void)out_value;
    switch (ctx->pc) {
    case 0:
        WFS_AWAIT(ctx, wfs_txn_stage_begin(ctx->vol, ctx->target), 1);
        /* fall through */
    case 1:
        ctx->err = wfs_txn_stage_take(ctx->vol);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        return WASMOS_WASM_TASK_COMPLETE;
    default:
        return WASMOS_ERR_FS_CORRUPT;
    }
}

static void crash_during_transaction(wfs_volume_t* vol, uint32_t target, uint32_t revoke,
                                     uint32_t stop_after) {
    stage_ctx_t sc;
    wasmos_wasm_coroutine_t task;
    uint8_t* d = wfs_block_data(wfs_stub_block());
    uint32_t i;

    expect_rc(wfs_txn_open(vol), WASMOS_ERR_NONE, "a transaction opens");
    wfs_stub_reset_counters();
    wfs_stub_stop_after = stop_after;

    for (i = 0; i < vol->super.block_size; ++i) {
        d[i] = (uint8_t)(k_pattern_seed + (uint8_t)i);
    }
    memset(&sc, 0, sizeof(sc));
    sc.vol = vol;
    sc.target = target;
    expect(wfs_stub_run_task(&task, stage_task, &sc) == 0, "the block is journaled");
    if (revoke) {
        expect_rc(wfs_txn_revoke(vol, revoke), WASMOS_ERR_NONE, "a block is revoked");
    }
    (void)wfs_txn_close(vol);
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

    /* And WRITABLE, which is what having replayed it buys: the transaction
     * either landed whole or was discarded whole, so there is no half-applied
     * state a mount would have to refuse to write over. */
    expect_u32(vol.super.read_only, 0u, "the volume is writable again");

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
    expect(wfs_rd64(js, (uint32_t)offsetof(struct wfs_journal_super, first_sequence)) == 1u,
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
    wfs_super_t parsed;
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
    expect(!block_has_pattern(target, k_pattern_seed), "no part of the transaction was applied");

    /* Regression: 2026-08-28-wfs-error-state-never-recorded
     *
     * §4 defines WFS_STATE_ERROR as "an inconsistency was detected; mount
     * read-only and run fsck", and a replay that cannot complete is exactly that
     * (§21). Nothing ever wrote it: the driver set read_only in MEMORY and the
     * next mount, reading a superblock that still said DIRTY, treated the volume
     * as merely unclean -- so it attempted the same doomed replay again, and
     * would have gone on doing so on every boot forever. */
    memset(&parsed, 0, sizeof(parsed));
    expect_rc(wfs_super_parse(wfs_stub_image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &parsed),
              WASMOS_ERR_NONE,
              "the superblock still verifies");
    expect_u32(parsed.state, (uint32_t)WFS_STATE_ERROR, "and records the volume as damaged");

    /* And the NEXT mount acts on it: read-only without re-running the replay,
     * which is the whole point of recording it. */
    expect(remount(&m, &vol) == 0, "it mounts again");
    expect_u32(vol.super.read_only, 1u, "still read-only");
    expect_u32(m.replayed, 0u, "and the doomed replay is not attempted again");
    expect_rc(m.journal_err, WASMOS_ERR_NONE, "so the log reports nothing this time");

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
    /* An operation is told the volume is READ-ONLY, which is the fact that
     * governs it. WHY it is read-only -- a damaged log here, a superblock
     * recovered from a backup elsewhere (§5) -- is the mount's report above, and
     * the two need different repairs. */
    expect_rc(
        wfs_txn_open(&vol), WASMOS_ERR_FS_READ_ONLY, "and no transaction can be opened on it");

    wfs_stub_teardown();
}

/* Overwrite a backup superblock's `state` and reseal it under its own block
 * number (§13), so a case can present the scan with a copy that disagrees with
 * the volume it came from. */
static void stamp_backup_state(uint32_t group, uint32_t state) {
    uint8_t* sb = wfs_stub_image + (size_t)wfs_super_backup_offset(4096u, group);
    uint32_t location = group * WFS_BLOCKS_PER_GROUP(4096u);
    uint8_t* csum = sb + offsetof(struct wfs_superblock, checksum);
    uint32_t i;
    uint32_t c;

    for (i = 0; i < 4u; ++i) {
        sb[offsetof(struct wfs_superblock, state) + i] = (uint8_t)((state >> (i * 8u)) & 0xFFu);
        csum[i] = 0u;
    }
    c = wfs_checksum_struct(
        k_uuid, location, sb, WFS_SUPER_SIZE, (uint32_t)offsetof(struct wfs_superblock, checksum));
    for (i = 0; i < 4u; ++i) {
        csum[i] = (uint8_t)((c >> (i * 8u)) & 0xFFu);
    }
}

/* ---- the writers, crashed at every step -------------------------------- */

/* Resolve `name` in the root of the mounted volume, the way a client does.
 *
 * Returns 1 when it names an object, 0 when it is absent, and -1 for anything
 * else -- a lookup that fails for a reason other than absence is a damaged
 * directory, which is one of the outcomes under test. */
static int lookup(wfs_volume_t* vol, const char* name, uint32_t* out_id) {
    static wfs_path_ctx_t path;
    wasmos_wasm_coroutine_t task;

    if (wfs_path_init_from(&path, vol, WFS_OBJECT_ROOT, name, (uint32_t)strlen(name)) !=
        WASMOS_ERR_NONE) {
        return -1;
    }
    if (wfs_stub_run_task(&task, wfs_path_task, &path) != 0) {
        return -1;
    }
    if (!path.found) {
        return 0;
    }
    *out_id = path.object_id;
    return 1;
}

/* Group 0's bitmaps, straight out of the image: the block bitmap first, the
 * object bitmap behind it. */
static const uint8_t* block_bitmap(void) {
    return wfs_stub_image + (size_t)g_layout.bitmap_start * wfs_stub_block_size;
}

static const uint8_t* object_bitmap(void) {
    return wfs_stub_image + (size_t)(g_layout.bitmap_start + 1u) * wfs_stub_block_size;
}

/* Whether group 0's free counters still agree with its bitmaps.
 *
 * The bitmaps are authoritative and the counters are derived (§12), so a
 * disagreement is precisely what a crash BETWEEN the two writes used to leave --
 * the allocator's own comment called it a stale counter fsck rebuilds. A
 * transaction has no between: both blocks land or neither does. This is the half
 * of the sweep that a non-journaled writer fails.
 */
static int counters_agree(const wfs_volume_t* vol) {
    const uint8_t* desc;
    uint32_t per_block = wfs_group_descs_per_block(vol->super.block_size);
    uint32_t bits;

    (void)per_block;
    desc = wfs_stub_image + (size_t)g_layout.group_table_start * wfs_stub_block_size;
    bits = vol->super.blocks_per_group;
    if (bits > vol->super.total_blocks) {
        bits = vol->super.total_blocks;
    }
    if (wfs_bitmap_count_free(block_bitmap(), bits) !=
        wfs_rd32(desc, (uint32_t)offsetof(struct wfs_group_desc, free_blocks))) {
        return 0;
    }
    return wfs_bitmap_count_free(object_bitmap(), (uint32_t)vol->super.total_objects) ==
           wfs_rd32(desc, (uint32_t)offsetof(struct wfs_group_desc, free_objects));
}

/* Block requests one uninterrupted create takes, which is what bounds the sweep
 * below. Measured rather than written down, so the sweep cannot silently stop
 * short of the operation as the writer changes. Returns 0 when the fixture could
 * not be built, having reported why. */
static uint32_t uninterrupted_create_steps(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t id = 0u;
    uint32_t steps;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return 0u;
    }
    expect(mount_volume(&m, &vol) == 0, "the fresh volume mounts");
    wfs_stub_reset_counters();
    expect_rc(wfs_ns_create(&vol,
                            WFS_OBJECT_ROOT,
                            "made.txt",
                            8u,
                            (uint16_t)WFS_TYPE_FILE,
                            0644u,
                            TEST_NOW_NS,
                            &id),
              WASMOS_ERR_NONE,
              "an uninterrupted create succeeds");
    steps = wfs_stub_req_count;
    expect(steps > 0u, "and takes at least one block request");
    wfs_stub_teardown();
    return steps;
}

/* Stop a real namespace operation at EVERY step it takes, and require the volume
 * that comes back to be consistent at each one.
 *
 * This is the assertion that the writers actually journal, and picking a single
 * crash point would not be: a create touches an object bitmap, an object record,
 * a group descriptor and a directory block, and it is the gaps BETWEEN them that
 * a transaction closes. Every gap is therefore visited.
 *
 * Two things are required of the volume at each one. The name resolves to an
 * object the bitmap agrees is allocated, or it is absent -- never an entry
 * naming a record a later allocation would hand to a second file. And group 0's
 * free counters still agree with its bitmaps, which is the half a non-journaled
 * writer fails: it writes the bitmap and then the descriptor, and a crash
 * between them leaves the counter stale by construction.
 *
 * Both outcomes must actually occur over the sweep, or the case would pass on an
 * operation that never got anywhere.
 */
static void test_a_create_is_all_or_nothing_at_every_crash_point(void) {
    uint32_t stop;
    uint32_t steps;
    uint32_t saw_absent = 0u;
    uint32_t saw_present = 0u;
    uint32_t inconsistent = 0u;

    steps = uninterrupted_create_steps();
    if (steps == 0u) {
        return;
    }

    for (stop = 1u; stop <= steps; ++stop) {
        wfs_mount_ctx_t m;
        wfs_volume_t vol;
        uint32_t id = 0u;
        int found;

        if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
            expect(0, "build a volume");
            return;
        }
        if (mount_volume(&m, &vol) != 0) {
            expect(0, "the fresh volume mounts");
            wfs_stub_teardown();
            return;
        }
        wfs_stub_reset_counters();
        wfs_stub_stop_after = stop;
        (void)wfs_ns_create(&vol,
                            WFS_OBJECT_ROOT,
                            "made.txt",
                            8u,
                            (uint16_t)WFS_TYPE_FILE,
                            0644u,
                            TEST_NOW_NS,
                            &id);
        if (remount(&m, &vol) != 0) {
            expect(0, "the volume remounts after the crash");
            wfs_stub_teardown();
            return;
        }
        if (!counters_agree(&vol)) {
            inconsistent++;
        }
        found = lookup(&vol, "made.txt", &id);
        if (found < 0) {
            inconsistent++;
        } else if (found == 0) {
            saw_absent++;
        } else if (!wfs_bitmap_test(object_bitmap(), id)) {
            /* A name resolving to an object the bitmap calls free is exactly the
             * corruption the transaction exists to prevent: the next allocation
             * would hand that record to a second file. */
            inconsistent++;
        } else {
            saw_present++;
        }
        wfs_stub_teardown();
    }

    expect_u32(inconsistent, 0u, "no crash point leaves the volume inconsistent");
    expect(saw_absent > 0u, "some crash points leave the file absent");
    expect(saw_present > 0u, "and some leave it fully created");
}

/* Regression: 2026-08-28-wfs-backup-state-stale
 *
 * A backup superblock is written by mkfs and says WFS_STATE_CLEAN. The live volume
 * goes DIRTY the moment it is mounted for writing, and its backups only catch up
 * when the state changes. A mount that falls back to one therefore cannot trust
 * what it reads there: a backup saying CLEAN made the mount conclude that no
 * replay was owed and serve metadata the log had already superseded -- the one
 * outcome §15's state check exists to prevent.
 *
 * The log here holds a COMMITTED transaction that was never checkpointed, so a
 * mount that skips the replay is distinguishable from one that runs it: the
 * target block carries the transaction's bytes only if the replay actually ran. */
static void test_a_backup_superblock_does_not_suppress_the_replay(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target;

    if (wfs_stub_build_volume(VOL_132M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume with two groups");
        return;
    }
    expect(g_layout.group_count >= 2u, "the fixture carries a backup");
    expect(mount_volume(&m, &vol) == 0, "the fresh volume mounts");
    expect(mark_dirty(&vol) == 0, "the volume is marked dirty");
    target = g_layout.bitmap_start;

    crash_during_transaction(&vol, target, 0u, CRASH_AFTER_COMMIT);
    expect(!block_has_pattern(target, k_pattern_seed), "the checkpoint never ran");

    /* Force the mount down the backup path, with the state the backup carries
     * deliberately reset to CLEAN -- the stale value the fix must not act on. */
    memset(wfs_stub_image + WFS_SUPER_OFFSET, 0, WFS_SUPER_SIZE);
    stamp_backup_state(1u, (uint32_t)WFS_STATE_CLEAN);

    expect(remount(&m, &vol) == 0, "the volume mounts from the backup");
    expect_u32(m.replayed, 1u, "and the replay RAN despite the backup saying clean");
    expect(block_has_pattern(target, k_pattern_seed), "landing the committed transaction");
    expect_u32(vol.super.read_only, 1u, "the volume is still read-only for fsck");

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
    WASMOS_TEST_CASE(test_a_backup_superblock_does_not_suppress_the_replay),
    WASMOS_TEST_CASE(test_a_create_is_all_or_nothing_at_every_crash_point),
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
