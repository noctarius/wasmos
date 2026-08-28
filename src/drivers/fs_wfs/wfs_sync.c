/* wfs_sync.c - recording a volume's mount state on disk (§4). */
#include "wfs_sync.h"

#include <stddef.h>

#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_ops.h"
#include "wfs_super.h"

/* Seal a superblock image for `location`: zero the checksum field, checksum the
 * whole 1024-byte region, store the result. The primary's location is 0 and a
 * backup's is its own block number (§13), which is what makes a copy that landed
 * at the wrong offset fail to verify rather than read as a neighbour. */
static void seal_super(const uint8_t uuid[WFS_UUID_LEN], uint64_t location, uint8_t* sb) {
    wfs_wr32(sb, (uint32_t)offsetof(struct wfs_superblock, checksum), 0u);
    wfs_wr32(sb,
             (uint32_t)offsetof(struct wfs_superblock, checksum),
             wfs_checksum_struct(uuid,
                                 location,
                                 sb,
                                 WFS_SUPER_SIZE,
                                 (uint32_t)offsetof(struct wfs_superblock, checksum)));
}

/* Apply this task's mutation to the staged superblock image and reseal it.
 *
 * `generation` advances on EVERY superblock write, which is §4's contract and
 * what makes the field order the primary against its backups at all: §5's scan
 * takes the valid copy carrying the highest one, so a generation that never moved
 * left a backup indistinguishable from a current primary.
 */
static void apply_super(wfs_sb_ctx_t* ctx, uint8_t* sb) {
    uint64_t generation = wfs_rd64(sb, (uint32_t)offsetof(struct wfs_superblock, generation));

    wfs_wr32(sb, (uint32_t)offsetof(struct wfs_superblock, state), ctx->state);
    if (ctx->set_counters) {
        wfs_wr64(sb,
                 (uint32_t)offsetof(struct wfs_superblock, free_blocks),
                 (uint64_t)ctx->vol->super.free_blocks);
        wfs_wr64(sb,
                 (uint32_t)offsetof(struct wfs_superblock, free_objects),
                 (uint64_t)ctx->vol->super.free_objects);
    }
    ctx->generation = generation + 1u;
    wfs_wr64(sb, (uint32_t)offsetof(struct wfs_superblock, generation), ctx->generation);
    seal_super(ctx->vol->super.uuid, 0u, sb);
}

/* The group holding the backup this step refreshes, or 0 when the sweep is done.
 *
 * Bounded to the groups §5's scan reaches: a backup the scan will never read
 * cannot help a mount, so writing it costs a device write for nothing. */
static uint32_t backup_group(const wfs_volume_t* vol, uint32_t index) {
    uint32_t group;
    uint32_t seen = 0u;

    for (group = 1u; group < vol->super.group_count; ++group) {
        if (!wfs_super_group_has_backup(group)) {
            continue;
        }
        if (seen == index) {
            return group;
        }
        seen++;
        if (seen >= WFS_SUPER_SCAN_GROUPS) {
            break;
        }
    }
    return 0u;
}

int32_t wfs_super_write_task(void* user, uintptr_t* out_value) {
    wfs_sb_ctx_t* ctx = (wfs_sb_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint8_t* sb;
    uint32_t group;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_SB_PC_START:
        if (!ctx->vol || !ctx->vol->mounted) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        /* Writing a read-only volume's superblock would compound whatever made it
         * read-only: a damaged primary recovered from a backup (§5), or a replay
         * that could not complete. */
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        /* Block 0 carries the primary superblock at a fixed byte offset (§4). */
        WFS_AWAIT(ctx, wfs_block_read_begin(b, 0u), WFS_SB_PC_PRIMARY_READY);
        /* fall through when the block was already staged */

    case WFS_SB_PC_PRIMARY_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        apply_super(ctx, wfs_block_data(b) + WFS_SUPER_OFFSET);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, 0u), WFS_SB_PC_PRIMARY_WRITTEN);
        /* fall through */

    case WFS_SB_PC_PRIMARY_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->vol->super.state = ctx->state;
        ctx->vol->super.generation = ctx->generation;
        if (!ctx->refresh_backups) {
            return WASMOS_WASM_TASK_COMPLETE;
        }
        ctx->backup_index = 0u;
        /* fall through into the sweep */

    backups:
        /* Refreshed on a STATE TRANSITION only, not on every superblock write.
         * The field a stale backup gets wrong that matters is `state`: a copy
         * written once by mkfs says CLEAN forever, and a mount that adopted it
         * would conclude no replay was owed. The counters and the generation are
         * allowed to trail, because §5 orders copies by generation and a trailing
         * one correctly loses. */
        group = backup_group(ctx->vol, ctx->backup_index);
        if (group == 0u) {
            return WASMOS_WASM_TASK_COMPLETE;
        }
        ctx->backup_block = group * ctx->vol->super.blocks_per_group;
        /* Read rather than reusing the staged primary image: a backup occupies
         * byte 0 of its block, where the primary sits at WFS_SUPER_OFFSET behind
         * the boot area (§4, §5), so the two are not the same 1024 bytes of a
         * block and the copy cannot simply be moved across. */
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->backup_block), WFS_SB_PC_BACKUP_READY);
        /* fall through */

    case WFS_SB_PC_BACKUP_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            /* A backup that cannot be refreshed is not a reason to fail the
             * operation the primary already recorded. The primary is what a mount
             * reads first, and it is written. */
            ctx->err = WASMOS_ERR_NONE;
            ctx->backup_index++;
            goto backups;
        }
        /* A backup begins at byte 0 of its block (§5), unlike the primary. */
        sb = wfs_block_data(b);
        wfs_wr32(sb, (uint32_t)offsetof(struct wfs_superblock, state), ctx->state);
        wfs_wr64(sb, (uint32_t)offsetof(struct wfs_superblock, generation), ctx->generation);
        seal_super(ctx->vol->super.uuid, ctx->backup_block, sb);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->backup_block), WFS_SB_PC_BACKUP_WRITTEN);
        /* fall through */

    case WFS_SB_PC_BACKUP_WRITTEN:
        ctx->err = wfs_block_take(b);
        ctx->err = WASMOS_ERR_NONE; /* same reason as the read above */
        ctx->backup_index++;
        goto backups;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

int32_t wfs_mark_dirty_task(void* user, uintptr_t* out_value) {
    wfs_dirty_ctx_t* ctx = (wfs_dirty_ctx_t*)user;
    int32_t joined = 0;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_DIRTY_PC_START:
        if (!ctx->vol || !ctx->vol->mounted) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        /* Once per mount. A caller may start this before every transaction and
         * pay for it only the first time, which is what keeps the flag off the
         * per-transaction cost without the caller tracking it. */
        if (ctx->vol->dirty_marked) {
            return WASMOS_WASM_TASK_COMPLETE;
        }
        /* Marking a read-only volume would make a mount that was never written
         * look like an interrupted write, costing the NEXT mount its writability
         * for nothing. */
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        ctx->write.pc = WFS_SB_PC_START;
        ctx->write.vol = ctx->vol;
        ctx->write.state = (uint32_t)WFS_STATE_DIRTY;
        ctx->write.set_counters = 0u;
        /* A STATE TRANSITION, so the backups follow it. */
        ctx->write.refresh_backups = 1u;
        ctx->write.err = WASMOS_ERR_NONE;
        wfs_ops_task_reset(&ctx->write_task);
        if (!wasmos_async_start(
                wfs_ops_runtime(), &ctx->write_task, wfs_super_write_task, &ctx->write)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->write_started = 1u;
        ctx->pc = WFS_DIRTY_PC_WRITE_JOINED;
        /* fall through */

    case WFS_DIRTY_PC_WRITE_JOINED:
        if (ctx->write_started) {
            int jr = wasmos_wasm_coroutine_join(&ctx->write_task, &joined);

            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->write_started = 0u;
            if (jr != 0) {
                ctx->err = (wasmos_error_code_t)jr;
                return jr;
            }
        }
        /* Recorded only after the write landed: a failed write must leave the
         * caller starting this again rather than believing the volume is
         * marked. */
        ctx->vol->dirty_marked = 1u;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

int32_t wfs_sync_task(void* user, uintptr_t* out_value) {
    wfs_sync_ctx_t* ctx = (wfs_sync_ctx_t*)user;
    int32_t joined = 0;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_SYNC_PC_START:
        if (!ctx->vol || !ctx->vol->mounted) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        if (ctx->state != (uint32_t)WFS_STATE_CLEAN && ctx->state != (uint32_t)WFS_STATE_DIRTY) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        /* Nothing to reconcile and nothing to record. A volume that was mounted
         * and never written is already CLEAN on disk, so an unmount sync over it
         * would spend a write advancing a generation for no change. */
        if (!ctx->vol->journal.counters_dirty && ctx->state == ctx->vol->super.state) {
            return WASMOS_WASM_TASK_COMPLETE;
        }
        ctx->write.pc = WFS_SB_PC_START;
        ctx->write.vol = ctx->vol;
        ctx->write.state = ctx->state;
        ctx->write.set_counters = 1u;
        /* A state TRANSITION carries the backups with it; a sync that leaves the
         * volume dirty does not, because `state` is the only field they hold that
         * a mount must not read stale (wfs_sync.h). */
        ctx->write.refresh_backups = ctx->state != ctx->vol->super.state ? 1u : 0u;
        ctx->write.err = WASMOS_ERR_NONE;
        wfs_ops_task_reset(&ctx->write_task);
        if (!wasmos_async_start(
                wfs_ops_runtime(), &ctx->write_task, wfs_super_write_task, &ctx->write)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->write_started = 1u;
        ctx->pc = WFS_SYNC_PC_WRITE_JOINED;
        /* fall through */

    case WFS_SYNC_PC_WRITE_JOINED:
        if (ctx->write_started) {
            int jr = wasmos_wasm_coroutine_join(&ctx->write_task, &joined);

            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->write_started = 0u;
            if (jr != 0) {
                ctx->err = (wasmos_error_code_t)jr;
                return jr;
            }
        }
        ctx->vol->journal.counters_dirty = 0u;
        /* A volume left CLEAN is not marked dirty again until the next write asks
         * for it, which is what makes the flag mean "mounted for writing" rather
         * than "was written once". */
        ctx->vol->dirty_marked = ctx->state == (uint32_t)WFS_STATE_DIRTY;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
