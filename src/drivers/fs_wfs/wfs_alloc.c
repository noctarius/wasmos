/* wfs_alloc.c - block allocation over the group bitmaps (§12). */
#include "wfs_alloc.h"

#include <stddef.h>

#include "wfs_bitmap.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_mount.h"
#include "wfs_ops.h"

/* Little-endian field access. On-disk fields are little-endian regardless of the
 * host, so they are assembled byte-wise rather than cast over.
 *
 * TODO: rd32/rd64 are duplicated in wfs_mount.c, wfs_dir.c, wfs_extent.c and
 * wfs_super.c as well as here; the writers below are new. A shared header would
 * hold one copy, at the cost of touching every one of those files. */
static uint32_t rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Blocks of `group` that exist on the device. Every group but the last is full;
 * the last is partial on a volume whose size is not a whole number of groups.
 * mkfs_wfs marks the past-the-end bits allocated, but clamping here means the
 * allocator does not depend on a formatter having done so. */
static uint32_t group_bits(const wfs_volume_t* vol, uint32_t group) {
    uint32_t per_group = WFS_BLOCKS_PER_GROUP(vol->super.block_size);
    uint32_t base = group * per_group;

    if (base >= vol->super.total_blocks) {
        return 0u;
    }
    if (vol->super.total_blocks - base < per_group) {
        return vol->super.total_blocks - base;
    }
    return per_group;
}

int32_t wfs_alloc_blocks_task(void* user, uintptr_t* out_value) {
    wfs_alloc_ctx_t* ctx = (wfs_alloc_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    int32_t joined = 0;
    uint32_t per_block;
    uint32_t offset;
    uint8_t* d;
    uint32_t i;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_ALLOC_PC_START:
        if (!ctx->vol || !ctx->vol->mounted || ctx->want == 0u ||
            ctx->vol->super.group_count == 0u) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        /* Refused before any block is touched. The gate has two sources — a
         * journal replay owed, and a primary recovered from a backup — so it is
         * read from super.read_only rather than from either cause, which is what
         * keeps a backup-mounted volume from becoming writable by omission. */
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        ctx->group = ctx->prefer_group < ctx->vol->super.group_count ? ctx->prefer_group : 0u;
        ctx->tried = 0u;
        ctx->pc = WFS_ALLOC_PC_DESC_JOINED;
        /* fall through into the sweep */

    case WFS_ALLOC_PC_DESC_JOINED:
        /* One pass over the volume, starting at the preferred group: locality
         * first, another group as the last fallback (§12). `tried` bounds it so a
         * full filesystem terminates rather than circling. */
        for (;;) {
            if (ctx->desc_started) {
                int jr = wasmos_wasm_coroutine_join(&ctx->desc_task, &joined);

                if (jr == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                ctx->desc_started = 0u;
                if (jr != 0 || joined != 0) {
                    /* A descriptor that does not verify is not a reason to hand
                     * out blocks from its group. */
                    WFS_FAIL(ctx, joined ? (wasmos_error_code_t)joined : WASMOS_ERR_FS_CORRUPT);
                }
                ctx->bitmap_block = (uint32_t)ctx->desc.out.block_bitmap;
                per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
                ctx->desc_block = ctx->vol->super.group_table_start + ctx->group / per_block;
                ctx->pc = WFS_ALLOC_PC_BITMAP_READY;
                WFS_AWAIT(
                    ctx, wfs_block_read_begin(b, ctx->bitmap_block), WFS_ALLOC_PC_BITMAP_READY);
                return wfs_alloc_blocks_task(user, out_value);
            }
            if (ctx->tried >= ctx->vol->super.group_count) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NO_SPACE);
            }
            ctx->tried++;
            wfs_ops_task_reset(&ctx->desc_task);
            memset(&ctx->desc, 0, sizeof(ctx->desc));
            ctx->desc.vol = ctx->vol;
            ctx->desc.group = ctx->group;
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->desc_task, wfs_group_task, &ctx->desc)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->desc_started = 1u;
        }

    case WFS_ALLOC_PC_BITMAP_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->run_length = wfs_bitmap_find_run(
            wfs_block_data(b), group_bits(ctx->vol, ctx->group), ctx->want, &ctx->run_start);
        if (ctx->run_length == 0u) {
            /* Nothing here; try the next group, wrapping so a preferred group
             * late in the volume still sees the ones before it. */
            ctx->group = (ctx->group + 1u) % ctx->vol->super.group_count;
            ctx->pc = WFS_ALLOC_PC_DESC_JOINED;
            return wfs_alloc_blocks_task(user, out_value);
        }
        for (i = 0; i < ctx->run_length; ++i) {
            wfs_bitmap_set(wfs_block_data(b), ctx->run_start + i);
        }
        /* The bitmap goes first, because it is what the next mount believes. A
         * crash after this and before the counter update leaves a stale counter,
         * which fsck rebuilds; the reverse order would leave the counter claiming
         * an allocation the bitmap does not record, and a later allocator would
         * hand the same blocks out again. */
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->bitmap_block), WFS_ALLOC_PC_BITMAP_WRITTEN);
        /* fall through */

    case WFS_ALLOC_PC_BITMAP_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->desc_block), WFS_ALLOC_PC_DESC_READY);
        /* fall through */

    case WFS_ALLOC_PC_DESC_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
        offset = (ctx->group % per_block) * WFS_GROUP_DESC_SIZE;
        d = wfs_block_data(b) + offset;

        wr32(d,
             (uint32_t)offsetof(struct wfs_group_desc, free_blocks),
             rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks)) - ctx->run_length);
        /* Reseal: free_blocks is inside the checksummed record, and a descriptor
         * whose checksum no longer matches fails the next mount. */
        wr32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum), 0u);
        wr32(d,
             (uint32_t)offsetof(struct wfs_group_desc, checksum),
             wfs_checksum_struct(ctx->vol->super.uuid,
                                 ctx->group,
                                 d,
                                 WFS_GROUP_DESC_SIZE,
                                 (uint32_t)offsetof(struct wfs_group_desc, checksum)));
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->desc_block), WFS_ALLOC_PC_DESC_WRITTEN);
        /* fall through */

    case WFS_ALLOC_PC_DESC_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        /* The volume's own counter is adjusted in memory only. It is derived
         * (§12), the superblock is resealed and written when the volume is
         * synced, and writing block 0 per allocation would cost a device write
         * for a number fsck rebuilds anyway.
         *
         * TODO: no sync path writes the superblock back yet, so a volume's
         * on-disk free_blocks trails its bitmaps until fsck or a future unmount
         * reconciles it. */
        if (ctx->vol->super.free_blocks >= ctx->run_length) {
            ctx->vol->super.free_blocks -= ctx->run_length;
        } else {
            ctx->vol->super.free_blocks = 0u;
        }
        ctx->first_block =
            ctx->group * WFS_BLOCKS_PER_GROUP(ctx->vol->super.block_size) + ctx->run_start;
        ctx->length = ctx->run_length;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
