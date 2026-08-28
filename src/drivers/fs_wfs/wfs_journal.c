/* wfs_journal.c - the write-ahead metadata journal (§14). */
#include "wfs_journal.h"

#include <stddef.h>

#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_ops.h"
#include "wfs_sync.h"

static void zero(uint8_t* p, uint32_t len) {
    uint32_t i;

    for (i = 0; i < len; ++i) {
        p[i] = 0u;
    }
}

/* ---- block sealing ------------------------------------------------------- */

#define JH_CHECKSUM ((uint32_t)offsetof(struct wfs_journal_header, checksum))

void wfs_journal_seal(const uint8_t uuid[WFS_UUID_LEN], uint32_t block, uint8_t* image,
                      uint32_t block_size) {
    wfs_wr32(image, JH_CHECKSUM, 0u);
    wfs_wr32(image, JH_CHECKSUM, wfs_checksum_struct(uuid, block, image, block_size, JH_CHECKSUM));
}

int wfs_journal_verify(const uint8_t uuid[WFS_UUID_LEN], uint32_t block, const uint8_t* image,
                       uint32_t block_size) {
    if (wfs_rd32(image, (uint32_t)offsetof(struct wfs_journal_header, magic)) !=
        WFS_JOURNAL_MAGIC) {
        return 0;
    }
    return wfs_rd32(image, JH_CHECKSUM) ==
           wfs_checksum_struct(uuid, block, image, block_size, JH_CHECKSUM);
}

/* ---- the read redirect --------------------------------------------------- */

/* An open transaction holds its blocks in the log, not at their targets, so a
 * reader inside the transaction is sent to the image (§14). Installed at begin
 * and cleared when the transaction closes. */
static uint32_t journal_redirect(void* user, uint32_t block) {
    const wfs_journal_t* j = (const wfs_journal_t*)user;
    uint32_t i;

    if (!j->open) {
        return block;
    }
    for (i = 0; i < j->target_count; ++i) {
        if (j->targets[i].target == block) {
            return j->targets[i].journal_block;
        }
    }
    return block;
}

/* ---- log geometry -------------------------------------------------------- */

/* A transaction always starts at the first log block: this driver retires each
 * transaction before the next begins, so the tail never moves off block 1
 * (wfs_journal_t). The descriptor sits there, its images follow it in target
 * order, an optional revoke record follows them, and the commit closes it. */
static uint32_t descriptor_block(const wfs_journal_t* j) {
    return j->start + WFS_TXN_DESCRIPTOR_BLOCK;
}

static uint32_t image_block(const wfs_journal_t* j, uint32_t index) {
    return j->start + WFS_TXN_DESCRIPTOR_BLOCK + 1u + index;
}

static uint32_t revoke_block(const wfs_journal_t* j) {
    return image_block(j, j->target_count);
}

static uint32_t commit_block(const wfs_journal_t* j) {
    return image_block(j, j->target_count) + (j->revoke_count ? 1u : 0u);
}

/* ---- journal superblock -------------------------------------------------- */

int32_t wfs_journal_load_task(void* user, uintptr_t* out_value) {
    wfs_jload_ctx_t* ctx = (wfs_jload_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    wfs_journal_t* j;
    const uint8_t* d;
    uint32_t blocks;
    uint32_t first_block;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_JLOAD_PC_START:
        if (!ctx->vol) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        if (ctx->vol->super.journal_blocks == 0u) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        /* Read directly rather than through the redirect: the log describes
         * itself, so no transaction may hold its superblock. */
        WFS_AWAIT(
            ctx, wfs_block_read_begin(b, ctx->vol->super.journal_start), WFS_JLOAD_PC_SUPER_READY);
        /* fall through when the block was already staged */

    case WFS_JLOAD_PC_SUPER_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        j = &ctx->vol->journal;
        d = wfs_block_data(b);

        if (wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_super, magic)) != WFS_JOURNAL_MAGIC ||
            wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_super, version)) != WFS_VERSION) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        /* The journal superblock is sealed over its 32 bytes alone, seeded with
         * its own block number -- the log blocks behind it are sealed over the
         * whole block instead, because their payload follows the header. */
        if (wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_super, checksum)) !=
            wfs_checksum_struct(ctx->vol->super.uuid,
                                ctx->vol->super.journal_start,
                                d,
                                (uint32_t)sizeof(struct wfs_journal_super),
                                (uint32_t)offsetof(struct wfs_journal_super, checksum))) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        if (wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_super, block_size)) !=
            ctx->vol->super.block_size) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }

        blocks = wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_super, blocks));
        first_block = wfs_rd32(d, (uint32_t)offsetof(struct wfs_journal_super, first_block));

        /* A log the superblock and its own header disagree about is not a log
         * this driver can transact in. */
        if (blocks != ctx->vol->super.journal_blocks) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        /* Room for the journal superblock plus one full-sized transaction. A
         * shorter log would let a transaction be refused for its size after part
         * of it had already been written. */
        if (blocks < 1u + WFS_TXN_LOG_BLOCKS(WFS_TXN_MAX_TARGETS, 1u)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }
        /* The tail never moves off the first log block here, so any other value
         * names a log some other writer produced -- one this driver would replay
         * from the wrong offset. */
        if (first_block != WFS_TXN_DESCRIPTOR_BLOCK) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_JOURNAL);
        }

        j->start = ctx->vol->super.journal_start;
        j->blocks = blocks;
        j->next_sequence =
            wfs_rd64(d, (uint32_t)offsetof(struct wfs_journal_super, first_sequence));
        j->open = 0u;
        j->target_count = 0u;
        j->revoke_count = 0u;
        j->loaded = 1u;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

void wfs_journal_build_super(const wfs_volume_t* vol, uint8_t* image, uint64_t first_sequence) {
    const wfs_journal_t* j = &vol->journal;

    zero(image, vol->super.block_size);
    wfs_wr32(image, (uint32_t)offsetof(struct wfs_journal_super, magic), WFS_JOURNAL_MAGIC);
    wfs_wr32(image, (uint32_t)offsetof(struct wfs_journal_super, version), WFS_VERSION);
    wfs_wr32(
        image, (uint32_t)offsetof(struct wfs_journal_super, block_size), vol->super.block_size);
    wfs_wr32(image, (uint32_t)offsetof(struct wfs_journal_super, blocks), j->blocks);
    wfs_wr64(image, (uint32_t)offsetof(struct wfs_journal_super, first_sequence), first_sequence);
    wfs_wr32(
        image, (uint32_t)offsetof(struct wfs_journal_super, first_block), WFS_TXN_DESCRIPTOR_BLOCK);
    /* Sealed over the 32-byte record alone, seeded with its own block number --
     * unlike a log block, whose payload follows its header and whose seal
     * therefore covers the whole block. */
    wfs_wr32(image,
             (uint32_t)offsetof(struct wfs_journal_super, checksum),
             wfs_checksum_struct(vol->super.uuid,
                                 j->start,
                                 image,
                                 (uint32_t)sizeof(struct wfs_journal_super),
                                 (uint32_t)offsetof(struct wfs_journal_super, checksum)));
}

/* ---- transaction lifetime ------------------------------------------------ */

int wfs_txn_is_open(const wfs_volume_t* vol) {
    return vol && vol->journal.open;
}

wasmos_error_code_t wfs_txn_begin(wfs_volume_t* vol) {
    wfs_journal_t* j;

    if (!vol || !vol->mounted) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    j = &vol->journal;
    if (!j->loaded) {
        return WASMOS_ERR_FS_JOURNAL;
    }
    if (vol->super.read_only) {
        return WASMOS_ERR_FS_READ_ONLY;
    }
    if (j->open) {
        return WASMOS_ERR_FS_BUSY;
    }

    /* The sequence is the one the on-disk tail already names, and it advances
     * only when a transaction RETIRES. An abandoned transaction therefore leaves
     * the tail still pointing at the sequence the next attempt will write, so a
     * retry overwrites it in place rather than stranding it beyond a tail that
     * has moved past it. */
    j->sequence = j->next_sequence;
    j->target_count = 0u;
    j->revoke_count = 0u;
    j->counters_dirty = 0u;
    j->stage_err = WASMOS_ERR_NONE;
    j->open = 1u;
    wfs_block_set_redirect(wfs_ops_block(), journal_redirect, j);
    return WASMOS_ERR_NONE;
}

void wfs_txn_abort(wfs_volume_t* vol) {
    wfs_journal_t* j;

    if (!vol || !vol->journal.open) {
        return;
    }
    j = &vol->journal;
    j->open = 0u;
    j->target_count = 0u;
    j->revoke_count = 0u;
    j->counters_dirty = 0u;
    /* The log keeps what was written into it. With no COMMIT carrying the
     * sequence, recovery reads it as stale content and discards it (§14). */
    wfs_block_set_redirect(wfs_ops_block(), 0, 0);
}

void wfs_txn_note_counters(wfs_volume_t* vol) {
    if (vol && vol->journal.open) {
        vol->journal.counters_dirty = 1u;
    }
}

wasmos_error_code_t wfs_txn_revoke(wfs_volume_t* vol, uint32_t block) {
    wfs_journal_t* j;
    uint32_t i;

    if (!vol || !vol->journal.open) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    j = &vol->journal;
    for (i = 0; i < j->revoke_count; ++i) {
        if (j->revokes[i] == block) {
            return WASMOS_ERR_NONE;
        }
    }
    if (j->revoke_count >= WFS_TXN_MAX_REVOKES) {
        return WASMOS_ERR_FS_TXN_FULL;
    }
    j->revokes[j->revoke_count++] = block;
    return WASMOS_ERR_NONE;
}

/* ---- staging one image --------------------------------------------------- */

/* Refuse the stage, recording why for the take and abandoning the transaction. */
static wasmos_future_t* stage_refuse(wfs_volume_t* vol, wasmos_error_code_t err) {
    vol->journal.stage_err = err;
    wfs_txn_abort(vol);
    return 0;
}

wasmos_future_t* wfs_txn_stage_begin(wfs_volume_t* vol, uint32_t target) {
    wfs_block_t* b = wfs_ops_block();
    wfs_journal_t* j;
    uint32_t slot;
    uint32_t i;

    if (!vol || !vol->journal.open) {
        return stage_refuse(vol, WASMOS_ERR_FS_BAD_ARGS);
    }
    j = &vol->journal;
    /* The volume must already say DIRTY on disk. A mount reading a CLEAN volume
     * never looks at the log (§15), so a transaction written before the flag
     * lands is one whose half-finished checkpoint nothing would ever complete. */
    if (!vol->dirty_marked) {
        return stage_refuse(vol, WASMOS_ERR_FS_NOT_READY);
    }
    if (target >= j->start && target < j->start + j->blocks) {
        /* The log cannot journal itself: an image of a log block would be
         * replayed over the very record recovery is reading. */
        return stage_refuse(vol, WASMOS_ERR_FS_BAD_ARGS);
    }

    /* A target staged twice REPLACES its image. A second target for the same
     * block would leave recovery applying two images to it in an order the
     * descriptor does not fix. */
    slot = j->target_count;
    for (i = 0; i < j->target_count; ++i) {
        if (j->targets[i].target == target) {
            slot = i;
            break;
        }
    }
    if (slot >= WFS_TXN_MAX_TARGETS) {
        return stage_refuse(vol, WASMOS_ERR_FS_TXN_FULL);
    }

    /* The image's checksum is taken over the bytes about to be written, so
     * recovery compares against what the log actually received. Plain CRC32C,
     * unseeded (§14): an image is not addressed by the block it is stored in,
     * and the descriptor that names both is itself seeded. */
    j->targets[slot].target = target;
    j->targets[slot].journal_block = image_block(j, slot);
    j->targets[slot].checksum = wfs_crc32c(wfs_block_data(b), vol->super.block_size);
    if (slot == j->target_count) {
        j->target_count++;
    }
    return wfs_block_write_begin(b, j->targets[slot].journal_block);
}

wasmos_error_code_t wfs_txn_stage_take(wfs_volume_t* vol) {
    wasmos_error_code_t err;

    if (!vol) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    /* Reported whether or not the transaction is still open: a refusal aborted
     * it, and the caller has yet to learn why. */
    if (vol->journal.stage_err != WASMOS_ERR_NONE) {
        err = vol->journal.stage_err;
        vol->journal.stage_err = WASMOS_ERR_NONE;
        return err;
    }
    err = wfs_block_take(wfs_ops_block());
    if (err != WASMOS_ERR_NONE) {
        wfs_txn_abort(vol);
    }
    return err;
}

/* ---- one operation, one transaction -------------------------------------- */

wasmos_error_code_t wfs_txn_open(wfs_volume_t* vol) {
    wfs_dirty_ctx_t dirty;
    int32_t status;

    if (!vol) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    /* Before the transaction, not inside it: the superblock's state flag is what
     * makes the next mount read the log at all, and it is deliberately NOT
     * journaled -- a flag that only a replay could apply would be useless to the
     * mount deciding whether to replay. Costs one write per mount. */
    dirty.pc = WFS_DIRTY_PC_START;
    dirty.vol = vol;
    dirty.err = WASMOS_ERR_NONE;
    status = wfs_ops_run(wfs_mark_dirty_task, &dirty);
    if (status != 0) {
        return (wasmos_error_code_t)status;
    }
    return wfs_txn_begin(vol);
}

wasmos_error_code_t wfs_txn_close(wfs_volume_t* vol) {
    wfs_txcommit_ctx_t commit;
    int32_t status;

    if (!vol) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    commit.pc = WFS_TXCOMMIT_PC_START;
    commit.vol = vol;
    commit.index = 0u;
    commit.err = WASMOS_ERR_NONE;
    status = wfs_ops_run(wfs_txn_commit_task, &commit);
    if (status != 0) {
        return (wasmos_error_code_t)status;
    }
    return WASMOS_ERR_NONE;
}

/* ---- commit and checkpoint ----------------------------------------------- */

/* Lay a journal block header into the staged buffer, leaving the rest zeroed.
 * The caller fills the payload and seals. */
static void begin_log_block(uint8_t* d, uint32_t block_size, uint32_t type, uint64_t sequence) {
    zero(d, block_size);
    wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_header, magic), WFS_JOURNAL_MAGIC);
    wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_header, type), type);
    wfs_wr64(d, (uint32_t)offsetof(struct wfs_journal_header, sequence), sequence);
}

int32_t wfs_txn_commit_task(void* user, uintptr_t* out_value) {
    wfs_txcommit_ctx_t* ctx = (wfs_txcommit_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    wfs_journal_t* j;
    uint8_t* d;
    uint32_t i;
    uint32_t off;

    (void)out_value;

    j = ctx->vol ? &ctx->vol->journal : 0;

    switch (ctx->pc) {
    case WFS_TXCOMMIT_PC_START:
        if (!j || !j->open) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        /* An empty transaction has nothing to make durable and nothing to
         * replay; committing it would still cost a descriptor, a commit and a
         * tail write. */
        if (j->target_count == 0u && j->revoke_count == 0u) {
            wfs_txn_abort(ctx->vol);
            return WASMOS_WASM_TASK_COMPLETE;
        }
        /* Every block the transaction occupies must exist in the log before any
         * of it is written, so a transaction is refused whole rather than
         * truncated at the end of the region. */
        if (WFS_TXN_LOG_BLOCKS(j->target_count, j->revoke_count) >= j->blocks) {
            wfs_txn_abort(ctx->vol);
            WFS_FAIL(ctx, WASMOS_ERR_FS_TXN_FULL);
        }

        /* §14 step 1: the descriptor naming every image. It is written after the
         * images rather than before them because the target list is only
         * complete now; what step 1 requires is that both are durable before the
         * COMMIT block, not which lands first. */
        d = wfs_block_data(b);
        begin_log_block(d, ctx->vol->super.block_size, WFS_JOURNAL_DESCRIPTOR, j->sequence);
        off = (uint32_t)offsetof(struct wfs_journal_descriptor, targets);
        if (off + j->target_count * (uint32_t)sizeof(struct wfs_journal_target) >
            ctx->vol->super.block_size) {
            wfs_txn_abort(ctx->vol);
            WFS_FAIL(ctx, WASMOS_ERR_FS_TXN_FULL);
        }
        for (i = 0; i < j->target_count; ++i) {
            uint32_t rec = off + i * (uint32_t)sizeof(struct wfs_journal_target);

            wfs_wr64(d,
                     rec + (uint32_t)offsetof(struct wfs_journal_target, target_block),
                     (uint64_t)j->targets[i].target);
            wfs_wr32(d,
                     rec + (uint32_t)offsetof(struct wfs_journal_target, flags),
                     i + 1u == j->target_count ? (uint32_t)WFS_JOURNAL_TARGET_LAST : 0u);
            wfs_wr32(d,
                     rec + (uint32_t)offsetof(struct wfs_journal_target, checksum),
                     j->targets[i].checksum);
        }
        wfs_journal_seal(ctx->vol->super.uuid, descriptor_block(j), d, ctx->vol->super.block_size);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, descriptor_block(j)), WFS_TXCOMMIT_PC_DESC_WRITTEN);
        /* fall through */

    case WFS_TXCOMMIT_PC_DESC_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }
        if (j->revoke_count == 0u) {
            goto commit;
        }
        /* §18: the blocks that stop being metadata in this transaction. Written
         * inside step 1 with the same sequence, so a transaction that frees a
         * block and one that replays an older image of it cannot disagree. */
        d = wfs_block_data(b);
        begin_log_block(d, ctx->vol->super.block_size, WFS_JOURNAL_REVOKE, j->sequence);
        off = (uint32_t)offsetof(struct wfs_journal_revoke, blocks);
        if (off + j->revoke_count * 8u > ctx->vol->super.block_size) {
            wfs_txn_abort(ctx->vol);
            WFS_FAIL(ctx, WASMOS_ERR_FS_TXN_FULL);
        }
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_revoke, count), j->revoke_count);
        for (i = 0; i < j->revoke_count; ++i) {
            wfs_wr64(d, off + i * 8u, (uint64_t)j->revokes[i]);
        }
        wfs_journal_seal(ctx->vol->super.uuid, revoke_block(j), d, ctx->vol->super.block_size);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, revoke_block(j)), WFS_TXCOMMIT_PC_REVOKE_WRITTEN);
        /* fall through */

    case WFS_TXCOMMIT_PC_REVOKE_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }

    commit:
        /* §14 step 2. The descriptor, the images and any revoke are now on the
         * device, but a volatile write cache can still lose them -- and a COMMIT
         * that reached media ahead of the images it names is precisely the
         * transaction recovery would apply from a log that does not hold it. */
        WFS_AWAIT(ctx, wfs_block_flush_begin(b), WFS_TXCOMMIT_PC_BARRIER_LOG);
        /* fall through */

    case WFS_TXCOMMIT_PC_BARRIER_LOG:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }

        /* §14 step 3. Everything the transaction promised is now in the log, and
         * this block is what makes it replayable. `target_count` lets recovery
         * confirm the descriptor named as many targets as the transaction
         * claimed before any image is applied. */
        d = wfs_block_data(b);
        begin_log_block(d, ctx->vol->super.block_size, WFS_JOURNAL_COMMIT, j->sequence);
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_commit, target_count), j->target_count);
        wfs_journal_seal(ctx->vol->super.uuid, commit_block(j), d, ctx->vol->super.block_size);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, commit_block(j)), WFS_TXCOMMIT_PC_COMMIT_WRITTEN);
        /* fall through */

    case WFS_TXCOMMIT_PC_COMMIT_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }
        /* §14 step 4. The transaction becomes replayable only once its COMMIT is
         * on media: a checkpoint that overwrote a target before that would leave
         * a crash with neither the old block nor a log entitled to restore it. */
        WFS_AWAIT(ctx, wfs_block_flush_begin(b), WFS_TXCOMMIT_PC_BARRIER_COMMIT);
        /* fall through */

    case WFS_TXCOMMIT_PC_BARRIER_COMMIT:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }
        ctx->index = 0u;
        /* Past this point the transaction is DURABLE, so a failure below is not
         * an abort: recovery would finish exactly this work. The volume latches
         * read-only instead, which is what keeps a later write from landing over
         * a transaction the next mount still owes a replay. */

    checkpoint:
        /* §14 step 5: each image to its target block, in the order the
         * descriptor named them. */
        if (ctx->index >= j->target_count) {
            goto tail;
        }
        WFS_AWAIT(ctx,
                  wfs_block_read_begin(b, j->targets[ctx->index].journal_block),
                  WFS_TXCOMMIT_PC_IMAGE_READ);
        /* fall through when the image was already staged */

    case WFS_TXCOMMIT_PC_IMAGE_READ:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            ctx->vol->super.read_only = 1u;
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }
        WFS_AWAIT(ctx,
                  wfs_block_write_begin(b, j->targets[ctx->index].target),
                  WFS_TXCOMMIT_PC_TARGET_WRITTEN);
        /* fall through */

    case WFS_TXCOMMIT_PC_TARGET_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            ctx->vol->super.read_only = 1u;
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }
        ctx->index++;
        goto checkpoint;

    tail:
        /* §14 step 6. The tail must not retire a transaction whose checkpoint is
         * still only in a cache: a crash there would leave the log saying the
         * work is done and the targets still holding what they held. */
        WFS_AWAIT(ctx, wfs_block_flush_begin(b), WFS_TXCOMMIT_PC_BARRIER_DATA);
        /* fall through */

    case WFS_TXCOMMIT_PC_BARRIER_DATA:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            ctx->vol->super.read_only = 1u;
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }

        /* §14 step 7: retire the transaction. The tail names the sequence the
         * NEXT transaction writes, so a crash after this point replays nothing;
         * a crash before it replays this transaction again, which is harmless
         * because every image is a whole-block overwrite. */
        wfs_journal_build_super(ctx->vol, wfs_block_data(b), j->sequence + 1u);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, j->start), WFS_TXCOMMIT_PC_TAIL_WRITTEN);
        /* fall through */

    case WFS_TXCOMMIT_PC_TAIL_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            ctx->vol->super.read_only = 1u;
            wfs_txn_abort(ctx->vol);
            return (int32_t)ctx->err;
        }
        /* Only a RETIRED transaction advances the sequence. An abandoned one
         * leaves it, so the retry overwrites the log in place under a tail that
         * still names it (wfs_txn_begin). */
        j->next_sequence = j->sequence + 1u;
        j->open = 0u;
        j->target_count = 0u;
        j->revoke_count = 0u;
        wfs_block_set_redirect(b, 0, 0);
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
