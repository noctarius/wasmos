/* wfs_mount.c - mount, group descriptors, object records, as runtime tasks.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15, §11, §7, §13.
 */
#include "wfs_mount.h"

#include "wfs_crc32c.h"
#include "wfs_ops.h"
#include "wfs_super.h"

/* Records are read out of the staged block field by field rather than by
 * casting it to a struct: the buffer holds a byte image whose alignment is the
 * staging buffer's, and the on-disk order is little-endian whatever the host
 * is. */
static uint32_t rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

static uint64_t rd64(const uint8_t* p, uint32_t off) {
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4) << 32);
}

static uint16_t rd16(const uint8_t* p, uint32_t off) {
    return (uint16_t)((uint32_t)p[off] | ((uint32_t)p[off + 1] << 8));
}

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

        if (rd32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum)) !=
            wfs_checksum_struct(ctx->vol->super.uuid,
                                ctx->group,
                                d,
                                WFS_GROUP_DESC_SIZE,
                                (uint32_t)offsetof(struct wfs_group_desc, checksum))) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
        }

        ctx->out.block_bitmap = rd64(d, (uint32_t)offsetof(struct wfs_group_desc, block_bitmap));
        ctx->out.object_bitmap = rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_bitmap));
        ctx->out.object_table = rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_table));
        ctx->out.free_blocks = rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks));
        ctx->out.free_objects = rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_objects));
        ctx->out.flags = rd32(d, (uint32_t)offsetof(struct wfs_group_desc, flags));

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

        if (rd32(d, (uint32_t)offsetof(struct wfs_object, checksum)) !=
            wfs_checksum_struct(ctx->vol->super.uuid,
                                ctx->object_id,
                                d,
                                WFS_OBJECT_SIZE,
                                (uint32_t)offsetof(struct wfs_object, checksum))) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
        }
        /* The record carries its own id, so one read from the wrong slot is
         * caught even where the checksum somehow is not. */
        if (rd64(d, (uint32_t)offsetof(struct wfs_object, object_id)) != ctx->object_id) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }

        ctx->out.object_id = ctx->object_id;
        ctx->out.type = rd16(d, (uint32_t)offsetof(struct wfs_object, type));
        ctx->out.flags = rd16(d, (uint32_t)offsetof(struct wfs_object, flags));
        ctx->out.mode = rd32(d, (uint32_t)offsetof(struct wfs_object, mode));
        ctx->out.uid = rd32(d, (uint32_t)offsetof(struct wfs_object, uid));
        ctx->out.gid = rd32(d, (uint32_t)offsetof(struct wfs_object, gid));
        ctx->out.size = rd64(d, (uint32_t)offsetof(struct wfs_object, size));
        ctx->out.atime = rd64(d, (uint32_t)offsetof(struct wfs_object, atime));
        ctx->out.mtime = rd64(d, (uint32_t)offsetof(struct wfs_object, mtime));
        ctx->out.ctime = rd64(d, (uint32_t)offsetof(struct wfs_object, ctime));
        ctx->out.btime = rd64(d, (uint32_t)offsetof(struct wfs_object, btime));
        ctx->out.link_count = rd32(d, (uint32_t)offsetof(struct wfs_object, link_count));
        ctx->out.extent_count = rd32(d, (uint32_t)offsetof(struct wfs_object, extent_count));
        ctx->out.extent_tree_block =
            rd64(d, (uint32_t)offsetof(struct wfs_object, extent_tree_block));

        for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
            uint32_t e = (uint32_t)offsetof(struct wfs_object, extents) +
                         i * (uint32_t)sizeof(struct wfs_extent);

            ctx->out.extents[i].logical_block =
                rd64(d, e + (uint32_t)offsetof(struct wfs_extent, logical_block));
            ctx->out.extents[i].physical_block =
                rd64(d, e + (uint32_t)offsetof(struct wfs_extent, physical_block));
            ctx->out.extents[i].length = rd32(d, e + (uint32_t)offsetof(struct wfs_extent, length));
        }

        /* An inline-data object stores bytes where extents would be, so a
         * reader taking extent_count at face value would map data as block
         * numbers. */
        if (ctx->out.flags & WFS_OBJ_INLINE_DATA) {
            if (ctx->out.size > WFS_INLINE_DATA_MAX || ctx->out.extent_count != 0u ||
                ctx->out.extent_tree_block != 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
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
        if (ctx->err != WASMOS_ERR_NONE) {
            /* TODO: fall back to the backup superblocks (§5) — enumerate the
             * three permitted block sizes, take the highest valid generation.
             * Until then a damaged primary fails the mount even where a backup
             * would serve. */
            return (int32_t)ctx->err;
        }
        ctx->err = wfs_block_set_block_size(b, ctx->vol->super.block_size);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->next_group = 0u;
        ctx->pc = WFS_MOUNT_PC_GROUP_JOINED;
        /* fall through into the sweep */

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

        /* Journal replay belongs here, before the volume is handed out: a
         * volume not unmounted cleanly has metadata in the log that the on-disk
         * structures do not yet reflect (§15, §21).
         *
         * TODO: replay the journal when super.needs_replay is set. Until then a
         * dirty volume mounts read-only rather than silently serving stale
         * metadata, which is the conservative half of the contract. */
        if (ctx->vol->super.needs_replay) {
            ctx->vol->super.read_only = 1u;
        }
        ctx->vol->mounted = 1u;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
