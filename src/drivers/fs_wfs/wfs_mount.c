/* wfs_mount.c - mount, group descriptors, object records.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15, §11, §7, §13.
 */
#include "wfs_mount.h"

#include "wfs_co.h"
#include "wfs_crc32c.h"
#include "wfs_super.h"

/* Records are read out of the staged block field by field rather than by
 * casting it to a struct: the buffer is a byte image whose alignment is the
 * staging buffer's, and the on-disk order is little-endian regardless of the
 * host. */
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

wfs_r_t wfs_group_step(wfs_group_ctx_t* ctx, wfs_block_t* b, const wfs_volume_t* vol) {
    uint32_t per_block;
    uint32_t offset;
    const uint8_t* d;

    WFS_CO_BEGIN(ctx);

    if (ctx->group >= vol->super.group_count) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
    }

    per_block = wfs_group_descs_per_block(vol->super.block_size);
    ctx->block = vol->super.group_table_start + ctx->group / per_block;
    if (ctx->block >= vol->super.group_table_start + vol->super.group_table_blocks) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
    }

    WFS_CO_READ(ctx, b, ctx->block);

    /* Recomputed after the yield: `per_block` and `offset` are C locals and the
     * stack did not survive it. */
    per_block = wfs_group_descs_per_block(vol->super.block_size);
    offset = (ctx->group % per_block) * WFS_GROUP_DESC_SIZE;
    d = wfs_block_data(b) + offset;

    if (rd32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum)) !=
        wfs_checksum_struct(vol->super.uuid,
                            ctx->group,
                            d,
                            WFS_GROUP_DESC_SIZE,
                            (uint32_t)offsetof(struct wfs_group_desc, checksum))) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CHECKSUM);
    }

    ctx->out.block_bitmap = rd64(d, (uint32_t)offsetof(struct wfs_group_desc, block_bitmap));
    ctx->out.object_bitmap = rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_bitmap));
    ctx->out.object_table = rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_table));
    ctx->out.free_blocks = rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks));
    ctx->out.free_objects = rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_objects));
    ctx->out.flags = rd32(d, (uint32_t)offsetof(struct wfs_group_desc, flags));

    /* A descriptor that verifies can still name a block outside the volume, and
     * every allocation in this group would then address it. */
    if (ctx->out.block_bitmap >= vol->super.total_blocks ||
        ctx->out.object_bitmap >= vol->super.total_blocks ||
        ctx->out.object_table >= vol->super.total_blocks) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
    }

    WFS_CO_END(ctx);
}

wfs_r_t wfs_object_step(wfs_object_ctx_t* ctx, wfs_block_t* b, const wfs_volume_t* vol) {
    uint32_t per_block;
    uint32_t offset;
    const uint8_t* d;
    uint32_t i;

    WFS_CO_BEGIN(ctx);

    if (ctx->object_id == WFS_OBJECT_INVALID || ctx->object_id >= vol->super.total_objects) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_NOT_FOUND);
    }

    per_block = wfs_objects_per_block(vol->super.block_size);
    ctx->block = vol->super.object_table_start + ctx->object_id / per_block;
    if (ctx->block >= vol->super.object_table_start + vol->super.object_table_blocks) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
    }

    WFS_CO_READ(ctx, b, ctx->block);

    per_block = wfs_objects_per_block(vol->super.block_size);
    offset = (ctx->object_id % per_block) * WFS_OBJECT_SIZE;
    d = wfs_block_data(b) + offset;

    if (rd32(d, (uint32_t)offsetof(struct wfs_object, checksum)) !=
        wfs_checksum_struct(vol->super.uuid,
                            ctx->object_id,
                            d,
                            WFS_OBJECT_SIZE,
                            (uint32_t)offsetof(struct wfs_object, checksum))) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CHECKSUM);
    }

    /* The record carries its own id, so a record read from the wrong slot is
     * caught even where the checksum somehow is not. */
    if (rd64(d, (uint32_t)offsetof(struct wfs_object, object_id)) != ctx->object_id) {
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
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
    ctx->out.extent_tree_block = rd64(d, (uint32_t)offsetof(struct wfs_object, extent_tree_block));

    for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
        uint32_t e = (uint32_t)offsetof(struct wfs_object, extents) +
                     i * (uint32_t)sizeof(struct wfs_extent);

        ctx->out.extents[i].logical_block =
            rd64(d, e + (uint32_t)offsetof(struct wfs_extent, logical_block));
        ctx->out.extents[i].physical_block =
            rd64(d, e + (uint32_t)offsetof(struct wfs_extent, physical_block));
        ctx->out.extents[i].length = rd32(d, e + (uint32_t)offsetof(struct wfs_extent, length));
    }

    /* An inline-data object stores bytes where extents would be, so a reader
     * that took extent_count at face value would map data as block numbers. */
    if (ctx->out.flags & WFS_OBJ_INLINE_DATA) {
        if (ctx->out.size > WFS_INLINE_DATA_MAX || ctx->out.extent_count != 0u ||
            ctx->out.extent_tree_block != 0u) {
            WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
        }
    } else if (ctx->out.extent_count > WFS_INLINE_EXTENTS && ctx->out.extent_tree_block == 0u) {
        /* More extents than fit inline, and no tree to hold them. */
        WFS_CO_FAIL(ctx, b, WASMOS_ERR_FS_CORRUPT);
    }

    WFS_CO_END(ctx);
}

wfs_r_t wfs_mount_step(wfs_mount_ctx_t* ctx, wfs_block_t* b, wfs_volume_t* vol) {
    wasmos_error_code_t rc;

    WFS_CO_BEGIN(ctx);

    vol->mounted = 0u;

    /* Block 0 carries the reserved boot area and the primary superblock. The
     * read is expressed in blocks at the DEFAULT size because the volume's own
     * block_size is a superblock field: whatever it turns out to be, block 0
     * begins at byte 0 and the superblock lies wholly inside the first 4096
     * bytes, so this read reaches it at every permitted size (§4). */
    WFS_CO_READ(ctx, b, 0u);

    rc = wfs_super_parse(wfs_block_data(b) + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &vol->super);
    if (rc != WASMOS_ERR_NONE) {
        /* TODO: fall back to the backup superblocks (§5) — enumerate the three
         * permitted block sizes, take the highest valid generation. Until then
         * a damaged primary is a failed mount even where a backup would serve. */
        WFS_CO_FAIL(ctx, b, rc);
    }

    rc = wfs_block_set_block_size(b, vol->super.block_size);
    if (rc != WASMOS_ERR_NONE) {
        WFS_CO_FAIL(ctx, b, rc);
    }

    /* Every descriptor is verified before the volume is usable. One that fails
     * names a bitmap block nothing vouches for, and every allocation in its
     * group would address it. */
    ctx->checked_groups = 0u;
    while (ctx->checked_groups < vol->super.group_count) {
        ctx->group.cont = 0;
        ctx->group.group = ctx->checked_groups;
        WFS_CO_AWAIT(ctx, wfs_group_step(&ctx->group, b, vol));
        ctx->checked_groups++;
    }

    /* Journal replay belongs here, before the volume is handed out: a volume
     * that was not unmounted cleanly has metadata in the log that the on-disk
     * structures do not yet reflect (§15, §21).
     *
     * TODO: replay the journal when super.needs_replay is set. Until then a
     * dirty volume mounts read-only rather than silently serving stale
     * metadata, which is the conservative half of the contract. */
    if (vol->super.needs_replay) {
        vol->super.read_only = 1u;
    }

    vol->mounted = 1u;

    WFS_CO_END(ctx);
}
