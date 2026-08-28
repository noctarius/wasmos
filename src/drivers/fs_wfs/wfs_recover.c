/* wfs_recover.c - crash recovery: replaying the metadata journal (§21). */
#include "wfs_recover.h"

#include <stddef.h>

#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_journal.h"
#include "wfs_ops.h"

/* Whether the staged block is a live log record of `sequence`: it verifies as
 * the journal block it was read from, and carries that sequence.
 *
 * A block failing either test is the head of the log — stale content a previous
 * transaction left at the same offset, which is exactly what a log that is
 * consumed in place and never erased is full of (§21 pass 1). */
static int record_is_live(const wfs_volume_t* vol, const uint8_t* d, uint32_t block,
                          uint64_t sequence) {
    if (!wfs_journal_verify(vol->super.uuid, block, d, vol->super.block_size)) {
        return 0;
    }
    return wfs_rd64(d, (uint32_t)offsetof(struct wfs_journal_header, sequence)) == sequence;
}

static uint32_t record_type(const uint8_t* d) {
    return wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_header, type));
}

/* Whether `block` is a legal destination for a replayed image.
 *
 * Block 0 carries the boot area and the primary superblock and is never
 * allocated (§4); a block inside the journal region would have recovery
 * overwrite the very records it is reading. */
static int target_is_legal(const wfs_volume_t* vol, uint64_t block) {
    const wfs_journal_t* j = &vol->journal;

    if (block == 0u || block >= vol->super.total_blocks) {
        return 0;
    }
    return !(block >= j->start && block < (uint64_t)j->start + j->blocks);
}

static int is_revoked(const wfs_replay_ctx_t* ctx, uint32_t block) {
    uint32_t i;

    for (i = 0; i < ctx->revoke_count; ++i) {
        if (ctx->revokes[i] == block) {
            return 1;
        }
    }
    return 0;
}

/* Parse the descriptor in the staged block into the context's target list.
 *
 * The list ends at the record carrying WFS_JOURNAL_TARGET_LAST, or at a record
 * naming block 0 — which no transaction can target, so an all-zero record past
 * the last one terminates a descriptor that names none at all. A revoke-only
 * transaction is exactly that case, and has no record on which to set the flag.
 */
static wasmos_error_code_t parse_descriptor(wfs_replay_ctx_t* ctx, const uint8_t* d) {
    uint32_t off = (uint32_t)offsetof(struct wfs_journal_descriptor, targets);
    uint32_t stride = (uint32_t)sizeof(struct wfs_journal_target);

    ctx->target_count = 0u;
    for (;;) {
        uint64_t target;
        uint32_t flags;

        if (off + stride > ctx->vol->super.block_size) {
            /* Ran off the block without a terminator: the descriptor was not
             * written by anything that agrees with this format. */
            return WASMOS_ERR_FS_JOURNAL;
        }
        target = wfs_rd64(d, off + (uint32_t)offsetof(struct wfs_journal_target, target_block));
        if (target == 0u) {
            return WASMOS_ERR_NONE;
        }
        if (!target_is_legal(ctx->vol, target)) {
            return WASMOS_ERR_FS_CORRUPT;
        }
        if (ctx->target_count >= WFS_TXN_MAX_TARGETS) {
            /* A longer transaction than this driver writes. Refused rather than
             * applied in part, because a partial transaction is the one outcome
             * the journal exists to prevent. */
            return WASMOS_ERR_FS_JOURNAL;
        }
        ctx->targets[ctx->target_count] = (uint32_t)target;
        ctx->checksums[ctx->target_count] =
            wfs_rd32(d, off + (uint32_t)offsetof(struct wfs_journal_target, checksum));
        flags = wfs_rd32(d, off + (uint32_t)offsetof(struct wfs_journal_target, flags));
        ctx->target_count++;
        if (flags & (uint32_t)WFS_JOURNAL_TARGET_LAST) {
            return WASMOS_ERR_NONE;
        }
        off += stride;
    }
}

/* Collect the blocks a revoke record names (§21 pass 2). */
static wasmos_error_code_t parse_revoke(wfs_replay_ctx_t* ctx, const uint8_t* d) {
    uint32_t off = (uint32_t)offsetof(struct wfs_journal_revoke, blocks);
    uint32_t count = wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_revoke, count));
    uint32_t i;

    if (count > WFS_TXN_MAX_REVOKES || off + count * 8u > ctx->vol->super.block_size) {
        return WASMOS_ERR_FS_JOURNAL;
    }
    ctx->revoke_count = 0u;
    for (i = 0; i < count; ++i) {
        uint64_t block = wfs_rd64(d, off + i * 8u);

        if (!target_is_legal(ctx->vol, block)) {
            return WASMOS_ERR_FS_CORRUPT;
        }
        ctx->revokes[ctx->revoke_count++] = (uint32_t)block;
    }
    return WASMOS_ERR_NONE;
}

int32_t wfs_journal_replay_task(void* user, uintptr_t* out_value) {
    wfs_replay_ctx_t* ctx = (wfs_replay_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    wfs_journal_t* j;
    const uint8_t* d;
    int32_t joined = 0;

    (void)out_value;

    j = ctx->vol ? &ctx->vol->journal : 0;

    switch (ctx->pc) {
    case WFS_REPLAY_PC_START:
        if (!ctx->vol || !j) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        /* Recovery addresses log blocks directly. A redirect belongs to an open
         * transaction, and there is none during a mount. */
        wfs_block_set_redirect(b, 0, 0);
        ctx->applied = 0u;
        ctx->committed = 0u;
        ctx->target_count = 0u;
        ctx->revoke_count = 0u;

        /* The mount path normally loads the log before this runs; loading it
         * here too means a caller that did not can still replay. */
        if (!j->loaded) {
            ctx->load.pc = WFS_JLOAD_PC_START;
            ctx->load.vol = ctx->vol;
            ctx->load.err = WASMOS_ERR_NONE;
            wfs_ops_task_reset(&ctx->load_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->load_task, wfs_journal_load_task, &ctx->load)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->load_started = 1u;
        }
        ctx->pc = WFS_REPLAY_PC_LOAD_JOINED;
        /* fall through */

    case WFS_REPLAY_PC_LOAD_JOINED:
        if (ctx->load_started) {
            int jr = wasmos_wasm_coroutine_join(&ctx->load_task, &joined);

            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->load_started = 0u;
            if (jr != 0) {
                ctx->err = (wasmos_error_code_t)jr;
                return jr;
            }
        }
        /* §21: recovery starts at the tail the journal superblock names, with
         * `expected` the sequence it says should be there. */
        ctx->cursor = WFS_TXN_DESCRIPTOR_BLOCK;
        ctx->sequence = j->next_sequence;
        WFS_AWAIT(ctx, wfs_block_read_begin(b, j->start + ctx->cursor), WFS_REPLAY_PC_DESC_READY);
        /* fall through when the block was already staged */

    case WFS_REPLAY_PC_DESC_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        d = wfs_block_data(b);
        if (!record_is_live(ctx->vol, d, j->start + ctx->cursor, ctx->sequence)) {
            /* Nothing at the tail: the volume crashed between transactions, and
             * what is there is stale content an earlier one left behind. */
            return WASMOS_WASM_TASK_COMPLETE;
        }
        if (record_type(d) != (uint32_t)WFS_JOURNAL_DESCRIPTOR) {
            /* A live record of the expected sequence that is not the descriptor
             * opening it: the log was laid out by a writer this one does not
             * agree with, so its extent cannot be computed. */
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        ctx->err = parse_descriptor(ctx, d);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        /* The images follow the descriptor in target order, so the record behind
         * them is the transaction's revoke or its commit. */
        ctx->cursor += 1u + ctx->target_count;

    scan:
        if (ctx->cursor >= j->blocks) {
            /* The transaction runs off the end of the log, so no commit of it
             * can exist. Discarded, like any uncommitted transaction. */
            return WASMOS_WASM_TASK_COMPLETE;
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, j->start + ctx->cursor), WFS_REPLAY_PC_SCAN_READY);
        /* fall through */

    case WFS_REPLAY_PC_SCAN_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        d = wfs_block_data(b);
        if (!record_is_live(ctx->vol, d, j->start + ctx->cursor, ctx->sequence)) {
            /* The transaction never reached its commit. A crash cannot be relied
             * on to record its own failure, which is why there is no ABORT type:
             * a transaction is replayable only when a COMMIT carries its
             * sequence (§14). */
            return WASMOS_WASM_TASK_COMPLETE;
        }
        if (record_type(d) == (uint32_t)WFS_JOURNAL_REVOKE) {
            ctx->err = parse_revoke(ctx, d);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->cursor++;
            goto scan;
        }
        if (record_type(d) != (uint32_t)WFS_JOURNAL_COMMIT) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        /* `target_count` is what confirms every image the transaction promised
         * is present before any of them is applied (§14). */
        if (wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_commit, target_count)) !=
            ctx->target_count) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        ctx->committed = 1u;
        ctx->index = 0u;

    replay:
        /* §21 pass 3. */
        if (ctx->index >= ctx->target_count) {
            goto tail;
        }
        if (is_revoked(ctx, ctx->targets[ctx->index])) {
            /* The comparison §21 states is `revoke_table[block] >= sequence`, and
             * a log holding one transaction collapses it to this: an image
             * journaled by the same transaction that revoked its block must not
             * be replayed either. */
            ctx->index++;
            goto replay;
        }
        WFS_AWAIT(ctx,
                  wfs_block_read_begin(b, j->start + WFS_TXN_DESCRIPTOR_BLOCK + 1u + ctx->index),
                  WFS_REPLAY_PC_IMAGE_READY);
        /* fall through */

    case WFS_REPLAY_PC_IMAGE_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        if (wfs_crc32c(wfs_block_data(b), ctx->vol->super.block_size) !=
            ctx->checksums[ctx->index]) {
            /* Recovery ABORTS rather than applying part of a transaction. The
             * volume mounts read-only and fsck (§24) is what repairs it. */
            WFS_FAIL(ctx, WASMOS_ERR_FS_REPLAY);
        }
        WFS_AWAIT(
            ctx, wfs_block_write_begin(b, ctx->targets[ctx->index]), WFS_REPLAY_PC_TARGET_WRITTEN);
        /* fall through */

    case WFS_REPLAY_PC_TARGET_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->applied++;
        ctx->index++;
        goto replay;

    tail:
        /* §21: recovery ends by setting the tail one past the last replayed
         * transaction. A crash before this repeats the replay from the same
         * tail, which lands the same bytes -- every image is a whole-block
         * overwrite. */
        wfs_journal_build_super(ctx->vol, wfs_block_data(b), ctx->sequence + 1u);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, j->start), WFS_REPLAY_PC_TAIL_WRITTEN);
        /* fall through */

    case WFS_REPLAY_PC_TAIL_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        j->next_sequence = ctx->sequence + 1u;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
