/* wfs_extent.c - resolve a logical block through an object's extent map.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §9, §13.
 */
#include "wfs_extent.h"

#include "wfs_crc32c.h"
#include "wfs_ops.h"

/* Records are read out of the staged block field by field rather than by casting
 * it to a struct: the buffer holds a byte image whose alignment is the staging
 * buffer's, and the on-disk order is little-endian whatever the host is. */
static uint32_t rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

static uint64_t rd64(const uint8_t* p, uint32_t off) {
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4) << 32);
}

/* No index chosen yet. Distinct from index 0, which is a legitimate choice. */
#define WFS_NO_INDEX 0xFFFFFFFFu

static uint16_t rd16(const uint8_t* p, uint32_t off) {
    return (uint16_t)((uint32_t)p[off] | ((uint32_t)p[off + 1] << 8));
}

/* Record the extent covering `logical`, or leave `found` clear for a hole.
 *
 * `run` is what remains of the extent from the target onward, which is what lets
 * a caller read a whole run in one request rather than a block at a time. */
static void take_extent(wfs_extent_ctx_t* ctx, uint64_t logical_block, uint64_t physical_block,
                        uint32_t length) {
    uint64_t offset = ctx->logical - logical_block;

    ctx->found = 1u;
    ctx->physical = (uint32_t)(physical_block + offset);
    ctx->run = length - (uint32_t)offset;
}

/* Does this extent cover the target? */
static int covers(const wfs_extent_ctx_t* ctx, uint64_t logical_block, uint32_t length) {
    return length != 0u && ctx->logical >= logical_block &&
           ctx->logical < logical_block + (uint64_t)length;
}

int32_t wfs_extent_task(void* user, uintptr_t* out_value) {
    wfs_extent_ctx_t* ctx = (wfs_extent_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    const uint8_t* node;
    uint32_t entries;
    uint32_t capacity;
    uint32_t depth;
    uint32_t i;
    uint32_t chosen;
    uint32_t slot;
    uint64_t child;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_EXTENT_PC_START:
        ctx->found = 0u;
        ctx->physical = 0u;
        ctx->run = 0u;
        if (!ctx->vol || !ctx->obj) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }

        /* An inline map is answered from the record already in hand, so a small
         * file costs no metadata block at all (§9). The two maps are exclusive:
         * a tree means the inline array is unused. */
        if (ctx->obj->extent_tree_block == 0u) {
            uint32_t n = ctx->obj->extent_count;

            if (n > WFS_INLINE_EXTENTS) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            for (i = 0; i < n; ++i) {
                if (covers(ctx, ctx->obj->extents[i].logical_block, ctx->obj->extents[i].length)) {
                    take_extent(ctx,
                                ctx->obj->extents[i].logical_block,
                                ctx->obj->extents[i].physical_block,
                                ctx->obj->extents[i].length);
                    break;
                }
            }
            return WASMOS_WASM_TASK_COMPLETE; /* covered, or a hole */
        }

        if (ctx->obj->extent_tree_block >= ctx->vol->super.total_blocks) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        ctx->block = (uint32_t)ctx->obj->extent_tree_block;
        ctx->depth_guard = WFS_EXTENT_MAX_DEPTH;
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->block), WFS_EXTENT_PC_NODE_READY);
        /* fall through when the node was already staged */

    case WFS_EXTENT_PC_NODE_READY:
        for (;;) {
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);

            /* The header is validated before any record is believed: magic
             * distinguishes a node from a data block that a corrupt pointer
             * named, and `capacity` is derived from the block size, so a value
             * that disagrees means the node was written by something with a
             * different idea of the layout (§9). */
            if (rd16(node, (uint32_t)offsetof(struct wfs_extent_header, magic)) !=
                WFS_EXTENT_NODE_MAGIC) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            if (rd32(node, (uint32_t)offsetof(struct wfs_extent_header, checksum)) !=
                wfs_checksum_struct(ctx->vol->super.uuid,
                                    ctx->block,
                                    node,
                                    ctx->vol->super.block_size,
                                    (uint32_t)offsetof(struct wfs_extent_header, checksum))) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
            }

            depth = rd16(node, (uint32_t)offsetof(struct wfs_extent_header, depth));
            entries = rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            capacity = rd16(node, (uint32_t)offsetof(struct wfs_extent_header, capacity));

            if (depth == 0u) {
                if (capacity != wfs_extent_leaf_capacity(ctx->vol->super.block_size) ||
                    entries > capacity) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
                }
                for (i = 0; i < entries; ++i) {
                    uint32_t e = (uint32_t)sizeof(struct wfs_extent_header) +
                                 i * (uint32_t)sizeof(struct wfs_extent);
                    uint64_t lb =
                        rd64(node, e + (uint32_t)offsetof(struct wfs_extent, logical_block));
                    uint64_t pb =
                        rd64(node, e + (uint32_t)offsetof(struct wfs_extent, physical_block));
                    uint32_t len = rd32(node, e + (uint32_t)offsetof(struct wfs_extent, length));

                    if (!covers(ctx, lb, len)) {
                        continue;
                    }
                    /* A verified extent can still point outside the volume, and
                     * the read that followed would address whatever the device
                     * returns past its end. */
                    if (pb >= ctx->vol->super.total_blocks ||
                        pb + len > ctx->vol->super.total_blocks) {
                        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
                    }
                    take_extent(ctx, lb, pb, len);
                    break;
                }
                return WASMOS_WASM_TASK_COMPLETE; /* covered, or a hole */
            }

            /* Interior. The child covering the target is the LAST index whose
             * logical_block does not exceed it, because records are sorted and
             * cover disjoint ranges (§9). */
            if (capacity != wfs_extent_interior_capacity(ctx->vol->super.block_size) ||
                entries > capacity || entries == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            if (depth > WFS_EXTENT_MAX_DEPTH || ctx->depth_guard == 0u) {
                /* A descent that does not shrink the depth is a cycle, and a
                 * cycle would spin this task forever against the device. */
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }

            chosen = WFS_NO_INDEX;
            for (i = 0; i < entries; ++i) {
                slot = (uint32_t)sizeof(struct wfs_extent_header) +
                       i * (uint32_t)sizeof(struct wfs_extent_index);
                if (rd64(node, slot + (uint32_t)offsetof(struct wfs_extent_index, logical_block)) >
                    ctx->logical) {
                    break;
                }
                chosen = i;
            }
            if (chosen == WFS_NO_INDEX) {
                /* Every child starts past the target: nothing maps it. */
                return WASMOS_WASM_TASK_COMPLETE;
            }

            slot = (uint32_t)sizeof(struct wfs_extent_header) +
                   chosen * (uint32_t)sizeof(struct wfs_extent_index);
            child = rd64(node, slot + (uint32_t)offsetof(struct wfs_extent_index, child_block));
            if (child == 0u || child >= ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            ctx->block = (uint32_t)child;
            ctx->depth_guard--;

            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->block), WFS_EXTENT_PC_NODE_READY);
            /* Already staged: keep descending without leaving the task. */
        }

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
