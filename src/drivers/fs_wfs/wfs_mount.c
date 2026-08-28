/* wfs_mount.c - mount, group descriptors, object records, as runtime tasks.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15, §11, §7, §13.
 */
#include "wfs_mount.h"

#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_journal.h"
#include "wfs_ops.h"
#include "wfs_recover.h"
#include "wfs_super.h"

int32_t wfs_group_task(void* user, uintptr_t* out_value) {
    wfs_group_ctx_t* ctx = (wfs_group_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t per_block;
    uint32_t offset;
    const uint8_t* d;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_GROUP_PC_START:
        if (!ctx->vol || ctx->group >= ctx->vol->super.group_count) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
        /* In the context, not a local: the await below evaluates it, and no
         * stack survives the resume. */
        ctx->block = ctx->vol->super.group_table_start + ctx->group / per_block;
        if (ctx->block >= ctx->vol->super.group_table_start + ctx->vol->super.group_table_blocks) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->block), WFS_GROUP_PC_BLOCK_READY);
        /* fall through when the block was already staged */

    case WFS_GROUP_PC_BLOCK_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }

        per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
        offset = (ctx->group % per_block) * WFS_GROUP_DESC_SIZE;
        d = wfs_block_data(b) + offset;

        if (wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum)) !=
            wfs_checksum_struct(ctx->vol->super.uuid,
                                ctx->group,
                                d,
                                WFS_GROUP_DESC_SIZE,
                                (uint32_t)offsetof(struct wfs_group_desc, checksum))) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
        }

        ctx->out.block_bitmap =
            wfs_rd64(d, (uint32_t)offsetof(struct wfs_group_desc, block_bitmap));
        ctx->out.object_bitmap =
            wfs_rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_bitmap));
        ctx->out.object_table =
            wfs_rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_table));
        ctx->out.free_blocks = wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks));
        ctx->out.free_objects =
            wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_objects));
        ctx->out.flags = wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, flags));

        /* A descriptor that verifies can still name a block outside the volume,
         * and every allocation in this group would then address it. */
        if (ctx->out.block_bitmap >= ctx->vol->super.total_blocks ||
            ctx->out.object_bitmap >= ctx->vol->super.total_blocks ||
            ctx->out.object_table >= ctx->vol->super.total_blocks) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

int32_t wfs_object_task(void* user, uintptr_t* out_value) {
    wfs_object_ctx_t* ctx = (wfs_object_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t per_block;
    uint32_t offset;
    const uint8_t* d;
    uint32_t i;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_OBJECT_PC_START:
        if (!ctx->vol || ctx->object_id == WFS_OBJECT_INVALID ||
            ctx->object_id >= ctx->vol->super.total_objects) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_NOT_FOUND);
        }
        per_block = wfs_objects_per_block(ctx->vol->super.block_size);
        ctx->block = ctx->vol->super.object_table_start + ctx->object_id / per_block;
        if (ctx->block >=
            ctx->vol->super.object_table_start + ctx->vol->super.object_table_blocks) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->block), WFS_OBJECT_PC_BLOCK_READY);
        /* fall through */

    case WFS_OBJECT_PC_BLOCK_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }

        per_block = wfs_objects_per_block(ctx->vol->super.block_size);
        offset = (ctx->object_id % per_block) * WFS_OBJECT_SIZE;
        d = wfs_block_data(b) + offset;

        if (wfs_rd32(d, (uint32_t)offsetof(struct wfs_object, checksum)) !=
            wfs_checksum_struct(ctx->vol->super.uuid,
                                ctx->object_id,
                                d,
                                WFS_OBJECT_SIZE,
                                (uint32_t)offsetof(struct wfs_object, checksum))) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
        }
        /* The record carries its own id, so one read from the wrong slot is
         * caught even where the checksum somehow is not. */
        if (wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, object_id)) != ctx->object_id) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }

        ctx->out.object_id = ctx->object_id;
        ctx->out.type = wfs_rd16(d, (uint32_t)offsetof(struct wfs_object, type));
        ctx->out.flags = wfs_rd16(d, (uint32_t)offsetof(struct wfs_object, flags));
        ctx->out.mode = wfs_rd32(d, (uint32_t)offsetof(struct wfs_object, mode));
        ctx->out.uid = wfs_rd32(d, (uint32_t)offsetof(struct wfs_object, uid));
        ctx->out.gid = wfs_rd32(d, (uint32_t)offsetof(struct wfs_object, gid));
        ctx->out.size = wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, size));
        ctx->out.atime = wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, atime));
        ctx->out.mtime = wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, mtime));
        ctx->out.ctime = wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, ctime));
        ctx->out.btime = wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, btime));
        ctx->out.link_count = wfs_rd32(d, (uint32_t)offsetof(struct wfs_object, link_count));
        ctx->out.extent_count = wfs_rd32(d, (uint32_t)offsetof(struct wfs_object, extent_count));
        ctx->out.extent_tree_block =
            wfs_rd64(d, (uint32_t)offsetof(struct wfs_object, extent_tree_block));

        for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
            uint32_t e = (uint32_t)offsetof(struct wfs_object, extents) +
                         i * (uint32_t)sizeof(struct wfs_extent);

            ctx->out.extents[i].logical_block =
                wfs_rd64(d, e + (uint32_t)offsetof(struct wfs_extent, logical_block));
            ctx->out.extents[i].physical_block =
                wfs_rd64(d, e + (uint32_t)offsetof(struct wfs_extent, physical_block));
            ctx->out.extents[i].length =
                wfs_rd32(d, e + (uint32_t)offsetof(struct wfs_extent, length));
        }

        /* An inline-data object stores bytes where extents would be, so a
         * reader taking extent_count at face value would map data as block
         * numbers. */
        if (ctx->out.flags & WFS_OBJ_INLINE_DATA) {
            if (ctx->out.size > WFS_INLINE_DATA_MAX || ctx->out.extent_count != 0u ||
                ctx->out.extent_tree_block != 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            /* Kept verbatim: the decode above read these same bytes as block
             * numbers, which is not reversible. */
            for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
                ctx->inline_data[i] = d[(uint32_t)offsetof(struct wfs_object, extents) + i];
            }
        } else if (ctx->out.extent_count > WFS_INLINE_EXTENTS && ctx->out.extent_tree_block == 0u) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

int32_t wfs_mount_task(void* user, uintptr_t* out_value) {
    wfs_mount_ctx_t* ctx = (wfs_mount_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    int32_t joined = 0;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_MOUNT_PC_START:
        ctx->vol->mounted = 0u;
        /* Block 0 carries the boot area and the primary superblock. The read is
         * at the DEFAULT block size because the volume's own block_size is a
         * superblock field; block 0 begins at byte 0 and the superblock lies
         * wholly inside the first 4096 bytes at every permitted size (§4). */
        WFS_AWAIT(ctx, wfs_block_read_begin(b, 0u), WFS_MOUNT_PC_SUPER_READY);
        /* fall through */

    case WFS_MOUNT_PC_SUPER_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->err = wfs_super_parse(
            wfs_block_data(b) + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &ctx->vol->super);
        if (ctx->err == WASMOS_ERR_NONE) {
            ctx->err = wfs_block_set_block_size(b, ctx->vol->super.block_size);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->next_group = 0u;
            ctx->pc = WFS_MOUNT_PC_GROUP_JOINED;
            goto sweep;
        }
        /* The primary did not validate, so scan the backups (§5). Its failure is
         * kept: if no backup validates either, that is the reason worth
         * reporting -- a scan-shaped error would send a reader looking for a
         * backup that this geometry may never have had. */
        ctx->primary_err = ctx->err;
        ctx->scan_index = 0u;
        ctx->scan_started = 0u;
        ctx->scan_have = 0u;
        ctx->pc = WFS_MOUNT_PC_BACKUP_READY;
        /* fall through into the scan */

    case WFS_MOUNT_PC_BACKUP_READY:
        for (;;) {
            uint32_t cand_bs = 0u;
            uint32_t cand_group = 0u;
            uint64_t cand_offset;

            if (ctx->scan_started) {
                ctx->scan_started = 0u;
                /* A candidate read that fails is a candidate that is not there —
                 * past the end of a short volume, most often — and skipping it is
                 * the whole point of a scan. */
                if (wfs_block_take(b) == WASMOS_ERR_NONE &&
                    wfs_super_backup_candidate(ctx->scan_index, &cand_bs, &cand_group)) {
                    wfs_super_t cand;
                    /* A backup is sealed under its own block number in the
                     * volume's block units (§13), which is what makes a wrong
                     * block-size guess self-rejecting. */
                    uint64_t location = (uint64_t)cand_group * WFS_BLOCKS_PER_GROUP(cand_bs);

                    if (wfs_super_parse(wfs_block_data(b), WFS_SUPER_SIZE, location, &cand) ==
                            WASMOS_ERR_NONE &&
                        cand.block_size == cand_bs &&
                        wfs_super_backup_prefer(&ctx->scan_best, ctx->scan_have, &cand)) {
                        ctx->scan_best = cand;
                        ctx->scan_have = 1u;
                    }
                }
                ctx->scan_index++;
            }
            if (!wfs_super_backup_candidate(ctx->scan_index, &cand_bs, &cand_group)) {
                break;
            }
            cand_offset = wfs_super_backup_offset(cand_bs, cand_group);
            /* Read at the DEFAULT block size, which is what the layer is still
             * set to: the volume's own size is exactly what is not known here.
             * Every candidate offset is a multiple of it, and a superblock is
             * 1024 bytes at the start of its block, so one default-sized read
             * covers the copy whatever the volume's real block size turns out to
             * be. */
            if (cand_offset == 0u || (cand_offset / WFS_BLOCK_SIZE_MIN) > (uint64_t)0xFFFFFFFFu) {
                ctx->scan_index++;
                continue;
            }
            ctx->scan_started = 1u;
            WFS_AWAIT(ctx,
                      wfs_block_read_begin(b, (uint32_t)(cand_offset / WFS_BLOCK_SIZE_MIN)),
                      WFS_MOUNT_PC_BACKUP_READY);
        }
        if (!ctx->scan_have) {
            return (int32_t)ctx->primary_err;
        }
        ctx->vol->super = ctx->scan_best;
        /* Recovered, not repaired: the primary is still damaged and this copy's
         * generation may trail it. Writing under that would compound the damage,
         * so the volume serves reads until fsck (§24) rebuilds the primary. */
        ctx->vol->super.read_only = 1u;
        ctx->err = wfs_block_set_block_size(b, ctx->vol->super.block_size);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->next_group = 0u;
        ctx->pc = WFS_MOUNT_PC_GROUP_JOINED;
        /* fall through into the sweep */

    sweep:

    case WFS_MOUNT_PC_GROUP_JOINED:
        /* The descriptor sweep. Each descriptor is read by a CHILD task that
         * this one starts and joins, which is the runtime's own composition
         * rather than a private sub-machine convention: the child's completion
         * future parks this task, and a child that fails rejects that future,
         * so its status arrives as the join's return value with no side
         * channel. */
        for (;;) {
            if (ctx->group_started) {
                int jr = wasmos_wasm_coroutine_join(&ctx->group_task, &joined);

                if (jr == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                ctx->group_started = 0u;
                if (jr != 0) {
                    ctx->err = (wasmos_error_code_t)jr;
                    return jr;
                }
                ctx->next_group++;
            }
            if (ctx->next_group >= ctx->vol->super.group_count) {
                break;
            }
            ctx->group.pc = WFS_GROUP_PC_START;
            ctx->group.vol = ctx->vol;
            ctx->group.group = ctx->next_group;
            ctx->group.err = WASMOS_ERR_NONE;
            wfs_ops_task_reset(&ctx->group_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->group_task, wfs_group_task, &ctx->group)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->group_started = 1u;
        }

        /* The log is read on EVERY mount, clean or not. §15 skips the replay
         * SCAN on a clean volume, not the journal superblock: a transaction
         * cannot be opened without the log's geometry and tail, and reading it
         * costs one block. */
        ctx->jload.pc = WFS_JLOAD_PC_START;
        ctx->jload.vol = ctx->vol;
        ctx->jload.err = WASMOS_ERR_NONE;
        wfs_ops_task_reset(&ctx->jload_task);
        if (!wasmos_async_start(
                wfs_ops_runtime(), &ctx->jload_task, wfs_journal_load_task, &ctx->jload)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->jload_started = 1u;
        ctx->pc = WFS_MOUNT_PC_JLOAD_JOINED;
        /* fall through */

    case WFS_MOUNT_PC_JLOAD_JOINED:
        if (ctx->jload_started) {
            int jr = wasmos_wasm_coroutine_join(&ctx->jload_task, &joined);

            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->jload_started = 0u;
            if (jr != 0) {
                /* An unusable log costs the volume its WRITABILITY, not its
                 * readability: every structure a reader touches is intact, and
                 * refusing the mount outright would deny a volume whose data is
                 * fine over a region only a writer needs. */
                ctx->journal_err = (wasmos_error_code_t)jr;
                ctx->vol->super.read_only = 1u;
                ctx->vol->mounted = 1u;
                return WASMOS_WASM_TASK_COMPLETE;
            }
        }
        if (!ctx->vol->super.needs_replay) {
            ctx->vol->mounted = 1u;
            return WASMOS_WASM_TASK_COMPLETE;
        }
        /* §15, §21: a volume whose state is not CLEAN was mounted for writing
         * and never unmounted, so the log may hold a transaction whose metadata
         * never reached its blocks. It is applied before the volume is handed
         * out -- afterwards a reader would see the superseded metadata. */
        ctx->replay.pc = WFS_REPLAY_PC_START;
        ctx->replay.vol = ctx->vol;
        ctx->replay.err = WASMOS_ERR_NONE;
        wfs_ops_task_reset(&ctx->replay_task);
        if (!wasmos_async_start(
                wfs_ops_runtime(), &ctx->replay_task, wfs_journal_replay_task, &ctx->replay)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->replay_started = 1u;
        ctx->pc = WFS_MOUNT_PC_REPLAY_JOINED;
        /* fall through */

    case WFS_MOUNT_PC_REPLAY_JOINED:
        if (ctx->replay_started) {
            int jr = wasmos_wasm_coroutine_join(&ctx->replay_task, &joined);

            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->replay_started = 0u;
            if (jr != 0) {
                /* §21: a replay that cannot complete leaves the volume for fsck.
                 * Read-only rather than refused, for the same reason as above --
                 * and this time the metadata really may be stale, so the gate is
                 * what stops a writer from building on it. */
                ctx->journal_err = (wasmos_error_code_t)jr;
                ctx->vol->super.read_only = 1u;
                ctx->vol->mounted = 1u;
                return WASMOS_WASM_TASK_COMPLETE;
            }
        }
        ctx->replayed = ctx->replay.applied;
        ctx->vol->super.needs_replay = 0u;
        /* The volume stays READ-ONLY even though the replay succeeded, because
         * the metadata writers do not yet run inside transactions: what a crash
         * left behind is a half-finished allocation the log never recorded, and
         * no replay repairs it. The gate lifts when every writer journals.
         * TODO: clear read_only here once wfs_alloc.c, wfs_write.c,
         * wfs_truncate.c, wfs_extent_write.c and wfs_namespace.c transact. */
        ctx->vol->super.read_only = 1u;
        ctx->vol->mounted = 1u;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
