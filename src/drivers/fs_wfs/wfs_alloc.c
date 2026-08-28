/* wfs_alloc.c - block allocation over the group bitmaps (§12). */
#include "wfs_alloc.h"

#include <stddef.h>

#include "wfs_bitmap.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_mount.h"
#include "wfs_ops.h"
#include "wfs_sync.h"

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
        /* The volume says DIRTY on disk before any metadata write lands. That
         * flag is what makes a crash mid-allocation mount read-only instead of
         * serving bitmaps and counters that disagree -- the whole of WFS's crash
         * safety until the journal exists (phase 3). Costs one write per mount. */
        wfs_ops_task_reset(&ctx->dirty_task);
        memset(&ctx->dirty, 0, sizeof(ctx->dirty));
        ctx->dirty.vol = ctx->vol;
        if (!wasmos_async_start(
                wfs_ops_runtime(), &ctx->dirty_task, wfs_mark_dirty_task, &ctx->dirty)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->dirty_started = 1u;
        ctx->pc = WFS_ALLOC_PC_DIRTY_JOINED;
        /* fall through */

    case WFS_ALLOC_PC_DIRTY_JOINED:
        if (ctx->dirty_started) {
            int dj = wasmos_wasm_coroutine_join(&ctx->dirty_task, &joined);

            if (dj == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->dirty_started = 0u;
            if (dj != 0 || joined != 0) {
                WFS_FAIL(ctx, joined ? (wasmos_error_code_t)joined : WASMOS_ERR_FS_IO);
            }
        }
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

        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, free_blocks),
                 wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks)) -
                     ctx->run_length);
        /* Reseal: free_blocks is inside the checksummed record, and a descriptor
         * whose checksum no longer matches fails the next mount. */
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum), 0u);
        wfs_wr32(d,
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

int32_t wfs_free_blocks_task(void* user, uintptr_t* out_value) {
    wfs_free_ctx_t* ctx = (wfs_free_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    int32_t joined = 0;
    uint32_t per_group;
    uint32_t per_block;
    uint32_t offset;
    uint32_t base;
    uint8_t* d;
    uint32_t i;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_FREE_PC_START:
        if (!ctx->vol || !ctx->vol->mounted) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        if (ctx->length == 0u) {
            return WASMOS_WASM_TASK_COMPLETE;
        }
        if (ctx->first_block + ctx->length > ctx->vol->super.total_blocks ||
            ctx->first_block + ctx->length < ctx->first_block) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_RANGE);
        }
        ctx->cursor = ctx->first_block;
        ctx->pc = WFS_FREE_PC_DESC_JOINED;
        /* fall through */

    case WFS_FREE_PC_DESC_JOINED:
        for (;;) {
            if (ctx->desc_started) {
                int jr = wasmos_wasm_coroutine_join(&ctx->desc_task, &joined);

                if (jr == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                ctx->desc_started = 0u;
                if (jr != 0 || joined != 0) {
                    WFS_FAIL(ctx, joined ? (wasmos_error_code_t)joined : WASMOS_ERR_FS_CORRUPT);
                }
                ctx->bitmap_block = (uint32_t)ctx->desc.out.block_bitmap;
                per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
                ctx->desc_block = ctx->vol->super.group_table_start + ctx->group / per_block;
                WFS_AWAIT(
                    ctx, wfs_block_read_begin(b, ctx->bitmap_block), WFS_FREE_PC_BITMAP_READY);
                return wfs_free_blocks_task(user, out_value);
            }
            if (ctx->cursor >= ctx->first_block + ctx->length) {
                return WASMOS_WASM_TASK_COMPLETE;
            }
            /* How much of the remaining run falls in the group the cursor is in.
             * A run crossing a group boundary is two bitmaps and two counters, so
             * it is handled as two passes rather than one wrong one. */
            per_group = WFS_BLOCKS_PER_GROUP(ctx->vol->super.block_size);
            ctx->group = ctx->cursor / per_group;
            base = ctx->group * per_group;
            ctx->run_in_group = base + per_group - ctx->cursor;
            if (ctx->run_in_group > ctx->first_block + ctx->length - ctx->cursor) {
                ctx->run_in_group = ctx->first_block + ctx->length - ctx->cursor;
            }
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

    case WFS_FREE_PC_BITMAP_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_group = WFS_BLOCKS_PER_GROUP(ctx->vol->super.block_size);
        base = ctx->group * per_group;
        for (i = 0; i < ctx->run_in_group; ++i) {
            wfs_bitmap_clear(wfs_block_data(b), ctx->cursor - base + i);
        }
        /* Bitmap first, same as allocation: it is what the next mount believes,
         * and a crash before the counter leaves a number fsck rebuilds. */
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->bitmap_block), WFS_FREE_PC_BITMAP_WRITTEN);
        /* fall through */

    case WFS_FREE_PC_BITMAP_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->desc_block), WFS_FREE_PC_DESC_READY);
        /* fall through */

    case WFS_FREE_PC_DESC_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
        offset = (ctx->group % per_block) * WFS_GROUP_DESC_SIZE;
        d = wfs_block_data(b) + offset;
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, free_blocks),
                 wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks)) +
                     ctx->run_in_group);
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum), 0u);
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, checksum),
                 wfs_checksum_struct(ctx->vol->super.uuid,
                                     ctx->group,
                                     d,
                                     WFS_GROUP_DESC_SIZE,
                                     (uint32_t)offsetof(struct wfs_group_desc, checksum)));
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->desc_block), WFS_FREE_PC_DESC_WRITTEN);
        /* fall through */

    case WFS_FREE_PC_DESC_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->vol->super.free_blocks += ctx->run_in_group;
        ctx->cursor += ctx->run_in_group;
        ctx->pc = WFS_FREE_PC_DESC_JOINED;
        return wfs_free_blocks_task(user, out_value);

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

/* Object records per group. Not a superblock field: it follows from two that are,
 * and deriving it keeps the two from disagreeing. */
static uint32_t objects_per_group(const wfs_volume_t* vol) {
    if (vol->super.group_count == 0u) {
        return 0u;
    }
    return (uint32_t)(vol->super.total_objects / vol->super.group_count);
}

int32_t wfs_alloc_object_task(void* user, uintptr_t* out_value) {
    wfs_objalloc_ctx_t* ctx = (wfs_objalloc_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    int32_t joined = 0;
    uint32_t per_group;
    uint32_t per_block;
    uint32_t offset;
    uint8_t* d;
    uint32_t i;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_OBJALLOC_PC_START:
        if (!ctx->vol || !ctx->vol->mounted || ctx->vol->super.group_count == 0u) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        ctx->group = ctx->prefer_group < ctx->vol->super.group_count ? ctx->prefer_group : 0u;
        ctx->tried = 0u;
        wfs_ops_task_reset(&ctx->dirty_task);
        memset(&ctx->dirty, 0, sizeof(ctx->dirty));
        ctx->dirty.vol = ctx->vol;
        if (!wasmos_async_start(
                wfs_ops_runtime(), &ctx->dirty_task, wfs_mark_dirty_task, &ctx->dirty)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->dirty_started = 1u;
        ctx->pc = WFS_OBJALLOC_PC_DIRTY_JOINED;
        /* fall through */

    case WFS_OBJALLOC_PC_DIRTY_JOINED:
        if (ctx->dirty_started) {
            int dj = wasmos_wasm_coroutine_join(&ctx->dirty_task, &joined);

            if (dj == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->dirty_started = 0u;
            if (dj != 0 || joined != 0) {
                WFS_FAIL(ctx, joined ? (wasmos_error_code_t)joined : WASMOS_ERR_FS_IO);
            }
        }
        ctx->pc = WFS_OBJALLOC_PC_DESC_JOINED;
        /* fall through */

    case WFS_OBJALLOC_PC_DESC_JOINED:
        for (;;) {
            if (ctx->desc_started) {
                int jr = wasmos_wasm_coroutine_join(&ctx->desc_task, &joined);

                if (jr == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                ctx->desc_started = 0u;
                if (jr != 0 || joined != 0) {
                    WFS_FAIL(ctx, joined ? (wasmos_error_code_t)joined : WASMOS_ERR_FS_CORRUPT);
                }
                ctx->bitmap_block = (uint32_t)ctx->desc.out.object_bitmap;
                per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
                ctx->desc_block = ctx->vol->super.group_table_start + ctx->group / per_block;
                WFS_AWAIT(
                    ctx, wfs_block_read_begin(b, ctx->bitmap_block), WFS_OBJALLOC_PC_BITMAP_READY);
                return wfs_alloc_object_task(user, out_value);
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

    case WFS_OBJALLOC_PC_BITMAP_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_group = objects_per_group(ctx->vol);
        if (wfs_bitmap_find_run(wfs_block_data(b), per_group, 1u, &ctx->slot) == 0u) {
            ctx->group = (ctx->group + 1u) % ctx->vol->super.group_count;
            ctx->pc = WFS_OBJALLOC_PC_DESC_JOINED;
            return wfs_alloc_object_task(user, out_value);
        }
        ctx->object_id = ctx->group * per_group + ctx->slot;
        /* Ids 0..15 are reserved (§25); mkfs marks them allocated, so reaching
         * one here means the bitmap disagrees with the format. */
        if (ctx->object_id < WFS_OBJECT_FIRST) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        per_block = wfs_objects_per_block(ctx->vol->super.block_size);
        ctx->record_block = ctx->vol->super.object_table_start + ctx->object_id / per_block;
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->record_block), WFS_OBJALLOC_PC_RECORD_READY);
        /* fall through */

    case WFS_OBJALLOC_PC_RECORD_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_block = wfs_objects_per_block(ctx->vol->super.block_size);
        d = wfs_block_data(b) + (ctx->object_id % per_block) * WFS_OBJECT_SIZE;
        for (i = 0; i < WFS_OBJECT_SIZE; ++i) {
            d[i] = 0u;
        }
        wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, object_id), ctx->object_id);
        wfs_wr16(d, (uint32_t)offsetof(struct wfs_object, type), ctx->type);
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, mode), ctx->mode);
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, link_count), ctx->link_count);
        wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, atime), ctx->now_ns);
        wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), ctx->now_ns);
        wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), ctx->now_ns);
        wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, btime), ctx->now_ns);
        /* A new file starts INLINE with nothing in it: size 0 and no extent, which
         * is what lets a first small write stay in the record. */
        if (ctx->type == WFS_TYPE_FILE) {
            wfs_wr16(
                d, (uint32_t)offsetof(struct wfs_object, flags), (uint16_t)WFS_OBJ_INLINE_DATA);
        }
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_object, checksum),
                 wfs_checksum_struct(ctx->vol->super.uuid,
                                     ctx->object_id,
                                     d,
                                     WFS_OBJECT_SIZE,
                                     (uint32_t)offsetof(struct wfs_object, checksum)));
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->record_block), WFS_OBJALLOC_PC_RECORD_WRITTEN);
        /* fall through */

    case WFS_OBJALLOC_PC_RECORD_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        /* The bit goes LAST of the two, so an allocated id always has a record
         * that verifies. See the note in wfs_alloc.h. */
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->bitmap_block), WFS_OBJALLOC_PC_BITMAP_WRITTEN);
        /* fall through */

    case WFS_OBJALLOC_PC_BITMAP_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        if (!wfs_bitmap_test(wfs_block_data(b), ctx->slot)) {
            wfs_bitmap_set(wfs_block_data(b), ctx->slot);
            WFS_AWAIT(
                ctx, wfs_block_write_begin(b, ctx->bitmap_block), WFS_OBJALLOC_PC_BITMAP_WRITTEN);
            return wfs_alloc_object_task(user, out_value);
        }
        ctx->pc = WFS_OBJALLOC_PC_DESC_READY;
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->desc_block), WFS_OBJALLOC_PC_DESC_READY);
        /* fall through */

    case WFS_OBJALLOC_PC_DESC_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
        offset = (ctx->group % per_block) * WFS_GROUP_DESC_SIZE;
        d = wfs_block_data(b) + offset;
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, free_objects),
                 wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_objects)) - 1u);
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum), 0u);
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, checksum),
                 wfs_checksum_struct(ctx->vol->super.uuid,
                                     ctx->group,
                                     d,
                                     WFS_GROUP_DESC_SIZE,
                                     (uint32_t)offsetof(struct wfs_group_desc, checksum)));
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->desc_block), WFS_OBJALLOC_PC_DESC_WRITTEN);
        /* fall through */

    case WFS_OBJALLOC_PC_DESC_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        if (ctx->vol->super.free_objects > 0u) {
            ctx->vol->super.free_objects--;
        }
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

int32_t wfs_free_object_task(void* user, uintptr_t* out_value) {
    wfs_objfree_ctx_t* ctx = (wfs_objfree_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    int32_t joined = 0;
    uint32_t per_group;
    uint32_t per_block;
    uint32_t offset;
    uint8_t* d;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_OBJFREE_PC_START:
        if (!ctx->vol || !ctx->vol->mounted || ctx->vol->super.group_count == 0u) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        if (ctx->object_id < WFS_OBJECT_FIRST || ctx->object_id >= ctx->vol->super.total_objects) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_NOT_FOUND);
        }
        per_group = objects_per_group(ctx->vol);
        if (per_group == 0u) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
        ctx->group = ctx->object_id / per_group;
        ctx->slot = ctx->object_id % per_group;
        wfs_ops_task_reset(&ctx->desc_task);
        memset(&ctx->desc, 0, sizeof(ctx->desc));
        ctx->desc.vol = ctx->vol;
        ctx->desc.group = ctx->group;
        if (!wasmos_async_start(wfs_ops_runtime(), &ctx->desc_task, wfs_group_task, &ctx->desc)) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
        }
        ctx->desc_started = 1u;
        ctx->pc = WFS_OBJFREE_PC_DESC_JOINED;
        /* fall through */

    case WFS_OBJFREE_PC_DESC_JOINED:
        if (ctx->desc_started) {
            int jr = wasmos_wasm_coroutine_join(&ctx->desc_task, &joined);

            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->desc_started = 0u;
            if (jr != 0 || joined != 0) {
                WFS_FAIL(ctx, joined ? (wasmos_error_code_t)joined : WASMOS_ERR_FS_CORRUPT);
            }
            ctx->bitmap_block = (uint32_t)ctx->desc.out.object_bitmap;
            per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
            ctx->desc_block = ctx->vol->super.group_table_start + ctx->group / per_block;
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->bitmap_block), WFS_OBJFREE_PC_BITMAP_READY);
        /* fall through */

    case WFS_OBJFREE_PC_BITMAP_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        wfs_bitmap_clear(wfs_block_data(b), ctx->slot);
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->bitmap_block), WFS_OBJFREE_PC_BITMAP_WRITTEN);
        /* fall through */

    case WFS_OBJFREE_PC_BITMAP_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->desc_block), WFS_OBJFREE_PC_DESC_READY);
        /* fall through */

    case WFS_OBJFREE_PC_DESC_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        per_block = wfs_group_descs_per_block(ctx->vol->super.block_size);
        offset = (ctx->group % per_block) * WFS_GROUP_DESC_SIZE;
        d = wfs_block_data(b) + offset;
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, free_objects),
                 wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, free_objects)) + 1u);
        wfs_wr32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum), 0u);
        wfs_wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, checksum),
                 wfs_checksum_struct(ctx->vol->super.uuid,
                                     ctx->group,
                                     d,
                                     WFS_GROUP_DESC_SIZE,
                                     (uint32_t)offsetof(struct wfs_group_desc, checksum)));
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->desc_block), WFS_OBJFREE_PC_DESC_WRITTEN);
        /* fall through */

    case WFS_OBJFREE_PC_DESC_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->vol->super.free_objects++;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
