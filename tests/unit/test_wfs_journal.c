/* Host unit test for the metadata journal (wfs_journal.h, §14).
 *
 * The assertions are about WHEN a block reaches its address, not merely that it
 * eventually does. A transaction that wrote its blocks in place and also logged
 * them would pass a "the bytes are there afterwards" test while providing no
 * crash safety at all, so every case here checks the image at a point where the
 * checkpoint has not run: the target must still hold what it held before, and a
 * reader inside the transaction must nonetheless see the new content.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_format.h"
#include "wfs_journal.h"
#include "wfs_mount.h"
#include "wfs_ops.h"

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
    0x6c, 0x2a, 0x9f, 0x51, 0x0d, 0x83, 0x4e, 0x17, 0xb1, 0x44, 0x7a, 0x38, 0xe5, 0x90, 0x22, 0xcb};
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

/* Fill the staged block with a recognisable pattern and journal it as the new
 * content of `target`. */
static int32_t stage_pattern(wfs_volume_t* vol, uint32_t target, uint8_t seed) {
    wfs_txstage_ctx_t ctx;
    wasmos_wasm_coroutine_t task;
    uint8_t* d = wfs_block_data(wfs_stub_block());
    uint32_t i;

    for (i = 0; i < vol->super.block_size; ++i) {
        d[i] = (uint8_t)(seed + (uint8_t)i);
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.vol = vol;
    ctx.target = target;
    return wfs_stub_run_task(&task, wfs_txn_stage_task, &ctx);
}

static int32_t commit(wfs_volume_t* vol) {
    wfs_txcommit_ctx_t ctx;
    wasmos_wasm_coroutine_t task;

    memset(&ctx, 0, sizeof(ctx));
    ctx.vol = vol;
    return wfs_stub_run_task(&task, wfs_txn_commit_task, &ctx);
}

/* Whether the image's block `block` carries the pattern stage_pattern wrote. */
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

/* Reading one block through the driver's block client, so a case can observe
 * what a READER inside a transaction sees rather than what the image holds. */
typedef struct {
    int pc;
    uint32_t block;
    wasmos_error_code_t err;
    uint8_t first;
} probe_ctx_t;

static int32_t probe_task(void* user, uintptr_t* out_value) {
    probe_ctx_t* ctx = (probe_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();

    (void)out_value;
    switch (ctx->pc) {
    case 0:
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->block), 1);
        /* fall through */
    case 1:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->first = wfs_block_data(b)[0];
        return WASMOS_WASM_TASK_COMPLETE;
    default:
        return WASMOS_ERR_FS_CORRUPT;
    }
}

static int32_t probe(uint32_t block, uint8_t* out_first) {
    probe_ctx_t ctx;
    wasmos_wasm_coroutine_t task;
    int32_t rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.block = block;
    rc = wfs_stub_run_task(&task, probe_task, &ctx);
    *out_first = ctx.first;
    return rc;
}

/* Every mount reads the log, so a transaction can be opened without a caller
 * having asked for one. A volume whose journal did not load would refuse writes
 * for a reason nothing in the read path explains. */
static void test_a_mount_loads_the_log(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "the fresh volume mounts");
    expect_u32(vol.journal.loaded, 1u, "the log loaded");
    expect_u32(vol.journal.start, g_layout.journal_start, "at the region the superblock names");
    expect_u32(vol.journal.blocks, g_layout.journal_blocks, "for its whole length");
    expect(vol.journal.next_sequence == 1u, "with the first sequence a transaction takes");
    expect_rc(m.journal_err, WASMOS_ERR_NONE, "and nothing to report about it");

    wfs_stub_teardown();
}

/* The property the journal exists for: the target keeps its old content until
 * the transaction commits. A writer that logged its blocks AND wrote them in
 * place would give a crash the half-finished metadata anyway. */
static void test_a_target_is_untouched_until_the_commit(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    target = g_layout.bitmap_start;

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    expect(stage_pattern(&vol, target, 0x41u) == 0, "and journals a block");
    expect(!block_has_pattern(target, 0x41u), "the target still holds what it held before");
    expect_u32(vol.journal.target_count, 1u, "one target is recorded");
    expect_u32(vol.journal.targets[0].target, target, "naming the block");
    expect_u32(vol.journal.targets[0].journal_block,
               g_layout.journal_start + WFS_TXN_DESCRIPTOR_BLOCK + 1u,
               "whose image follows the descriptor");
    expect(block_has_pattern(vol.journal.targets[0].journal_block, 0x41u),
           "and the image is in the log");

    expect(commit(&vol) == 0, "the commit completes");
    expect(block_has_pattern(target, 0x41u), "and only then does the target carry the new bytes");
    expect_u32(wfs_txn_is_open(&vol) ? 1u : 0u, 0u, "the transaction is closed");

    wfs_stub_teardown();
}

/* A reader inside the transaction must see the transaction's own writes.
 * Without that an operation that read back a block it had just journaled --
 * a bitmap touched twice, say -- would work from the pre-transaction content
 * and lose the earlier change. */
static void test_a_reader_inside_a_transaction_sees_its_writes(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target;
    uint8_t first = 0;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    target = g_layout.bitmap_start;

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    expect(stage_pattern(&vol, target, 0x55u) == 0, "and journals a block");

    /* Discarded so the read reaches the device: a cache hit would prove nothing
     * about where the read was sent. */
    wfs_block_invalidate(wfs_stub_block());
    wfs_stub_reset_counters();
    expect(probe(target, &first) == 0, "reading the target inside the transaction succeeds");
    expect_u32(first, 0x55u, "and returns the journaled content");
    expect_u32(wfs_stub_req_count, 1u, "in one request");
    expect_u32(wfs_stub_req_blocks[0],
               vol.journal.targets[0].journal_block,
               "sent to the log block holding the image, not to the target");

    expect(commit(&vol) == 0, "the commit completes");

    /* Once the transaction closes the redirect must be gone, or every later read
     * of that block would be answered out of a log slot the next transaction
     * overwrites. */
    wfs_block_invalidate(wfs_stub_block());
    wfs_stub_reset_counters();
    expect(probe(target, &first) == 0, "reading it afterwards succeeds");
    expect_u32(wfs_stub_req_blocks[0], target, "and addresses the target itself");
    expect_u32(first, 0x55u, "which now holds the checkpointed content");

    wfs_stub_teardown();
}

/* An abandoned transaction must leave the volume exactly as it was. Its log
 * blocks stay behind, and with no commit carrying the sequence they are stale
 * content (§14) -- which is also why the tail must NOT advance past them. */
static void test_an_aborted_transaction_changes_nothing(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    target = g_layout.object_table_start;

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    expect(stage_pattern(&vol, target, 0x77u) == 0, "and journals a block");
    wfs_txn_abort(&vol);

    expect_u32(wfs_txn_is_open(&vol) ? 1u : 0u, 0u, "it is closed");
    expect(!block_has_pattern(target, 0x77u), "the target never received the image");
    expect(vol.journal.next_sequence == 1u,
           "and the sequence is not spent, so a retry overwrites the log in place");

    /* A retry must be able to use the same log space. */
    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a second transaction opens");
    expect(stage_pattern(&vol, target, 0x11u) == 0, "and journals a block");
    expect(commit(&vol) == 0, "and commits");
    expect(block_has_pattern(target, 0x11u), "landing the retry's content");
    expect(vol.journal.next_sequence == 2u, "now the sequence advances");

    wfs_stub_teardown();
}

/* One transaction at a time, and none at all on a volume that refuses writes.
 * Both are refused before any block is touched. */
static void test_a_transaction_is_refused_when_it_cannot_be_run(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t before;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "the first transaction opens");
    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_FS_BUSY, "a second one is refused");
    wfs_txn_abort(&vol);

    vol.super.read_only = 1u;
    before = wfs_stub_req_count;
    expect_rc(
        wfs_txn_begin(&vol), WASMOS_ERR_FS_READ_ONLY, "a read-only volume refuses a transaction");
    expect_u32(wfs_stub_req_count, before, "without touching the device");

    wfs_stub_teardown();
}

/* Staging a target twice REPLACES its image. A second target for the same block
 * would leave recovery applying two images to it in an order the descriptor
 * does not fix. */
static void test_staging_a_target_twice_replaces_its_image(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target;
    uint32_t slot_block;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    target = g_layout.bitmap_start;

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    expect(stage_pattern(&vol, target, 0x20u) == 0, "the block is journaled");
    slot_block = vol.journal.targets[0].journal_block;
    expect(stage_pattern(&vol, target, 0x30u) == 0, "and journaled again");

    expect_u32(vol.journal.target_count, 1u, "still one target");
    expect_u32(vol.journal.targets[0].journal_block, slot_block, "in the same log block");
    expect(block_has_pattern(slot_block, 0x30u), "holding the second image");

    expect(commit(&vol) == 0, "the commit completes");
    expect(block_has_pattern(target, 0x30u), "and the target carries the second image");

    wfs_stub_teardown();
}

/* A transaction larger than the driver carries is refused WHOLE. Splitting it
 * would produce two transactions a crash could separate, which is the outcome
 * the journal exists to prevent. */
static void test_an_oversized_transaction_is_refused_whole(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t i;
    uint32_t base;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    base = g_layout.object_table_start;

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    for (i = 0; i < WFS_TXN_MAX_TARGETS; ++i) {
        if (stage_pattern(&vol, base + i, (uint8_t)(0x80u + i)) != 0) {
            expect(0, "every target up to the bound is journaled");
            break;
        }
    }
    expect_u32(vol.journal.target_count, WFS_TXN_MAX_TARGETS, "the table is full");
    expect_rc((wasmos_error_code_t)stage_pattern(&vol, base + WFS_TXN_MAX_TARGETS, 0x01u),
              WASMOS_ERR_FS_TXN_FULL,
              "one more is refused");
    expect_u32(wfs_txn_is_open(&vol) ? 1u : 0u,
               0u,
               "and the transaction is abandoned rather than committed short");
    expect(!block_has_pattern(base, 0x80u), "so no target received an image");

    wfs_stub_teardown();
}

/* Revokes must not be lost between the in-memory table and the log. The record
 * is what bars an older image of a freed block from replay (§18), so a commit
 * that dropped it would leave the volume replayable into a file's data. */
static void test_a_revoke_reaches_the_log(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    uint32_t target;
    uint32_t freed;
    uint32_t rblock;
    const uint8_t* d;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");
    target = g_layout.bitmap_start;
    freed = g_layout.object_table_start + 3u;

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    expect(stage_pattern(&vol, target, 0x60u) == 0, "a block is journaled");
    expect_rc(wfs_txn_revoke(&vol, freed), WASMOS_ERR_NONE, "a block is revoked");
    expect_rc(wfs_txn_revoke(&vol, freed), WASMOS_ERR_NONE, "revoking it twice is accepted");
    expect_u32(vol.journal.revoke_count, 1u, "and records it once");

    /* The revoke record sits behind the images, so its position is known before
     * the commit runs. */
    rblock = g_layout.journal_start + WFS_TXN_DESCRIPTOR_BLOCK + 1u + 1u;
    expect(commit(&vol) == 0, "the commit completes");

    d = wfs_stub_image + (size_t)rblock * wfs_stub_block_size;
    expect(wfs_journal_verify(vol.super.uuid, rblock, d, vol.super.block_size),
           "the revoke record verifies as the log block it sits in");
    expect_u32(wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_header, type)),
               (uint32_t)WFS_JOURNAL_REVOKE,
               "and is a revoke");
    expect_u32(
        wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_revoke, count)), 1u, "naming one block");
    expect(wfs_rd64(d, (uint32_t)offsetof(struct wfs_journal_revoke, blocks)) == (uint64_t)freed,
           "the one that was freed");

    wfs_stub_teardown();
}

/* The tail is what makes the log usable more than once: recovery starts where it
 * points, so a transaction that did not advance it would be replayed forever and
 * the next one would never be found. */
static void test_a_commit_advances_the_tail(void) {
    wfs_mount_ctx_t m;
    wfs_volume_t vol;
    const uint8_t* js;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, TEST_NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(mount_volume(&m, &vol) == 0, "mount");

    expect_rc(wfs_txn_begin(&vol), WASMOS_ERR_NONE, "a transaction opens");
    expect(stage_pattern(&vol, g_layout.bitmap_start, 0x90u) == 0, "a block is journaled");
    expect(commit(&vol) == 0, "the commit completes");

    js = wfs_stub_image + (size_t)g_layout.journal_start * wfs_stub_block_size;
    expect(wfs_rd64(js, (uint32_t)offsetof(struct wfs_journal_super, first_sequence)) == 2u,
           "the on-disk tail names the next sequence");
    expect_u32(wfs_rd32(js, (uint32_t)offsetof(struct wfs_journal_super, first_block)),
               WFS_TXN_DESCRIPTOR_BLOCK,
               "and still the first log block");
    expect(vol.journal.next_sequence == 2u, "as does the volume in memory");

    /* A REMOUNT is the reader that matters: it must adopt the advanced tail, or
     * the next transaction would be written under a sequence recovery no longer
     * expects. */
    expect(mount_volume(&m, &vol) == 0, "the volume remounts");
    expect(vol.journal.next_sequence == 2u, "with the tail the commit left");

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_mount_loads_the_log),
    WASMOS_TEST_CASE(test_a_target_is_untouched_until_the_commit),
    WASMOS_TEST_CASE(test_a_reader_inside_a_transaction_sees_its_writes),
    WASMOS_TEST_CASE(test_an_aborted_transaction_changes_nothing),
    WASMOS_TEST_CASE(test_a_transaction_is_refused_when_it_cannot_be_run),
    WASMOS_TEST_CASE(test_staging_a_target_twice_replaces_its_image),
    WASMOS_TEST_CASE(test_an_oversized_transaction_is_refused_whole),
    WASMOS_TEST_CASE(test_a_revoke_reaches_the_log),
    WASMOS_TEST_CASE(test_a_commit_advances_the_tail),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_journal: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_journal: %d checks passed\n", g_checks);
    return 0;
}
