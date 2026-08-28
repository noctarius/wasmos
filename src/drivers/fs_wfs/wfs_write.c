/* wfs_write.c - copy bytes into an object's data (§16). */
#include "wfs_write.h"

#include <stddef.h>

#include "wfs_alloc.h"
#include "wfs_bitmap.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_extent.h"
#include "wfs_extent_write.h"
#include "wfs_ops.h"
#include "wfs_sync.h"

void wfs_write_init(wfs_write_ctx_t* ctx, wfs_volume_t* vol, uint32_t object_id,
                    const struct wfs_object* obj, const uint8_t* inline_data, uint64_t offset,
                    const uint8_t* src, uint32_t len, uint64_t now_ns) {
    uint32_t i;

    ctx->pc = WFS_WRITE_PC_START;
    ctx->vol = vol;
    ctx->object_id = object_id;
    if (obj) {
        ctx->obj = *obj;
    }
    for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
        ctx->inline_data[i] = inline_data ? inline_data[i] : 0u;
    }
    ctx->offset = offset;
    ctx->src = src;
    ctx->len = len;
    ctx->now_ns = now_ns;
    ctx->done = 0u;
    ctx->logical = 0u;
    ctx->physical = 0u;
    ctx->fresh = 0u;
    ctx->record_block = 0u;
    ctx->extent_started = 0u;
    ctx->alloc_started = 0u;
    ctx->dirty_started = 0u;
    ctx->promote_inline = 0u;
    ctx->promote_block = 0u;
    ctx->tree_pending = 0u;
    ctx->pending_logical = 0u;
    ctx->pending_physical = 0u;
    ctx->pending_length = 0u;
    ctx->xtadd_started = 0u;
    ctx->err = WASMOS_ERR_NONE;
}

/* Record the run just allocated in the object's extent map.
 *
 * Extends the last extent when the new run continues it both logically and
 * physically, which is what keeps an appended file to ONE extent instead of one
 * per allocation. Otherwise appends a new extent.
 *
 * Returns WASMOS_ERR_FS_UNSUPPORTED when the inline array is full, WITHOUT
 * mutating it, which is what lets the caller use this as the test for whether an
 * extent still fits in the record. Beyond that the map becomes an extent tree
 * (wfs_extent_write.h) and this is not called again: an object with a tree has a
 * zeroed inline array, and every further extent goes into the leaf. */
static wasmos_error_code_t record_extent(wfs_write_ctx_t* ctx, uint64_t logical, uint32_t physical,
                                         uint32_t length) {
    struct wfs_extent* last;

    if (ctx->obj.extent_count > 0u && ctx->obj.extent_count <= WFS_INLINE_EXTENTS) {
        last = &ctx->obj.extents[ctx->obj.extent_count - 1u];
        if (last->logical_block + last->length == logical &&
            last->physical_block + last->length == (uint64_t)physical) {
            last->length += length;
            return WASMOS_ERR_NONE;
        }
    }
    if (ctx->obj.extent_count >= WFS_INLINE_EXTENTS) {
        return WASMOS_ERR_FS_UNSUPPORTED;
    }
    ctx->obj.extents[ctx->obj.extent_count].logical_block = logical;
    ctx->obj.extents[ctx->obj.extent_count].physical_block = (uint64_t)physical;
    ctx->obj.extents[ctx->obj.extent_count].length = length;
    ctx->obj.extents[ctx->obj.extent_count].reserved = 0u;
    ctx->obj.extent_count++;
    return WASMOS_ERR_NONE;
}

/* Bytes of the current chunk: from the cursor to the end of its block, capped by
 * what is left to write. Recomputed rather than carried, because it is derived
 * from values that already survive the awaits. */
static uint32_t chunk_len(const wfs_write_ctx_t* ctx) {
    uint32_t block_size = ctx->vol->super.block_size;
    uint32_t in_block = (uint32_t)((ctx->offset + ctx->done) % block_size);
    uint32_t chunk = block_size - in_block;

    if (chunk > ctx->len - ctx->done) {
        chunk = ctx->len - ctx->done;
    }
    return chunk;
}

/* Start the extent-add sub-task for the held extent. `leaf_block` is the block
 * allocated for a promotion, or 0 when the object already has a tree. */
static void start_xtadd(wfs_write_ctx_t* ctx, uint32_t leaf_block) {
    memset(&ctx->xtadd, 0, sizeof(ctx->xtadd));
    ctx->xtadd.pc = WFS_XTADD_PC_START;
    ctx->xtadd.vol = ctx->vol;
    ctx->xtadd.obj = &ctx->obj;
    ctx->xtadd.logical = ctx->pending_logical;
    ctx->xtadd.physical = ctx->pending_physical;
    ctx->xtadd.length = ctx->pending_length;
    ctx->xtadd.leaf_block = leaf_block;
    wfs_ops_task_reset(&ctx->xtadd_task);
    if (!wasmos_async_start(
            wfs_ops_runtime(), &ctx->xtadd_task, wfs_extent_add_task, &ctx->xtadd)) {
        ctx->err = WASMOS_ERR_FS_BUSY;
        return;
    }
    ctx->xtadd_started = 1u;
    ctx->err = WASMOS_ERR_NONE;
}

int32_t wfs_write_task(void* user, uintptr_t* out_value) {
    wfs_write_ctx_t* ctx = (wfs_write_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t block_size;
    uint32_t in_block;
    uint32_t chunk;
    uint32_t i;
    uint8_t* d;
    int32_t joined;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_WRITE_PC_START:
            if (!ctx->vol || !ctx->vol->mounted || (!ctx->src && ctx->len != 0u)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            /* A directory's bytes are records. Writing them as file content would
             * let a caller corrupt the namespace through a file interface. */
            if (ctx->obj.type == WFS_TYPE_DIR) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_IS_DIR);
            }
            if (ctx->vol->super.read_only) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
            }
            if (ctx->len == 0u) {
                return WASMOS_WASM_TASK_COMPLETE;
            }
            if (ctx->offset + (uint64_t)ctx->len < ctx->offset) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_RANGE);
            }
            /* An inline object outgrowing the record is PROMOTED: its bytes live
             * where extents would be (§7), so they are copied into a first data
             * block and the flag cleared before the write proper begins. Decided
             * here, before anything is written, because the record's bytes have
             * to be read before an extent is written over them -- and they are
             * already in hand, as ctx->inline_data. */
            if ((ctx->obj.flags & WFS_OBJ_INLINE_DATA) &&
                ctx->offset + (uint64_t)ctx->len > WFS_INLINE_DATA_MAX) {
                ctx->promote_inline = 1u;
            }
            /* The volume says DIRTY on disk before any of this write lands, for
             * the same reason an allocation does: a crash mid-write must leave a
             * volume the next mount refuses to write. */
            wfs_ops_task_reset(&ctx->dirty_task);
            ctx->dirty.pc = WFS_DIRTY_PC_START;
            ctx->dirty.vol = ctx->vol;
            ctx->dirty.err = WASMOS_ERR_NONE;
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->dirty_task, wfs_mark_dirty_task, &ctx->dirty)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->dirty_started = 1u;
            ctx->pc = WFS_WRITE_PC_DIRTY_JOINED;
            continue;

        case WFS_WRITE_PC_DIRTY_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->dirty_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->dirty_started = 0u;
            if (joined != 0) {
                WFS_FAIL(ctx, (wasmos_error_code_t)joined);
            }
            if (ctx->promote_inline) {
                /* One block for the content the record held. */
                memset(&ctx->alloc, 0, sizeof(ctx->alloc));
                ctx->alloc.vol = ctx->vol;
                ctx->alloc.want = 1u;
                ctx->alloc.prefer_group = 0u;
                wfs_ops_task_reset(&ctx->alloc_task);
                if (!wasmos_async_start(
                        wfs_ops_runtime(), &ctx->alloc_task, wfs_alloc_blocks_task, &ctx->alloc)) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
                }
                ctx->alloc_started = 1u;
                ctx->pc = WFS_WRITE_PC_INLINE_ALLOC_JOINED;
                continue;
            }
            /* Inline content lives in the record, so no block is touched at all;
             * the bytes are patched and the record is sealed. */
            if (ctx->obj.flags & WFS_OBJ_INLINE_DATA) {
                for (i = 0; i < ctx->len; ++i) {
                    ctx->inline_data[ctx->offset + i] = ctx->src[i];
                }
                ctx->done = ctx->len;
                ctx->pc = WFS_WRITE_PC_RECORD_READ;
                continue;
            }
            ctx->pc = WFS_WRITE_PC_MAP;
            continue;

        case WFS_WRITE_PC_INLINE_ALLOC_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->alloc_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->alloc_started = 0u;
            if (joined != 0) {
                WFS_FAIL(ctx, (wasmos_error_code_t)joined);
            }
            if (ctx->alloc.length == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NO_SPACE);
            }
            ctx->promote_block = ctx->alloc.first_block;
            /* The record's bytes, and zeroes for the rest of the block: the
             * range past the old end is content nothing has written. The block
             * is fresh, so it is not read first. */
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            d = wfs_block_data(b);
            block_size = ctx->vol->super.block_size;
            for (i = 0; i < block_size; ++i) {
                d[i] = 0u;
            }
            for (i = 0; i < WFS_INLINE_DATA_MAX && (uint64_t)i < ctx->obj.size; ++i) {
                d[i] = ctx->inline_data[i];
            }
            WFS_AWAIT(
                ctx, wfs_block_write_begin(b, ctx->promote_block), WFS_WRITE_PC_INLINE_WRITTEN);
            continue;

        case WFS_WRITE_PC_INLINE_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            /* The content is on disk, so the map may name it. The flag goes with
             * the same update: while it is set a reader takes these bytes as
             * inline content rather than as an extent (§7). */
            ctx->obj.flags = (uint16_t)(ctx->obj.flags & ~(uint16_t)WFS_OBJ_INLINE_DATA);
            ctx->obj.extents[0].logical_block = 0u;
            ctx->obj.extents[0].physical_block = (uint64_t)ctx->promote_block;
            ctx->obj.extents[0].length = 1u;
            ctx->obj.extents[0].reserved = 0u;
            ctx->obj.extent_count = 1u;
            for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
                ctx->inline_data[i] = 0u;
            }
            ctx->promote_inline = 0u;
            ctx->pc = WFS_WRITE_PC_MAP;
            continue;

        case WFS_WRITE_PC_MAP:
            if (ctx->done >= ctx->len) {
                ctx->pc = WFS_WRITE_PC_RECORD_READ;
                continue;
            }
            block_size = ctx->vol->super.block_size;
            ctx->logical = (ctx->offset + ctx->done) / block_size;
            ctx->extent.pc = WFS_EXTENT_PC_START;
            ctx->extent.vol = ctx->vol;
            ctx->extent.obj = &ctx->obj;
            ctx->extent.logical = ctx->logical;
            ctx->extent.err = WASMOS_ERR_NONE;
            wfs_ops_task_reset(&ctx->extent_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->extent_task, wfs_extent_task, &ctx->extent)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->extent_started = 1u;
            ctx->pc = WFS_WRITE_PC_MAP_JOINED;
            continue;

        case WFS_WRITE_PC_MAP_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->extent_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->extent_started = 0u;
            if (ctx->extent.err != WASMOS_ERR_NONE) {
                WFS_FAIL(ctx, ctx->extent.err);
            }
            if (ctx->extent.found) {
                ctx->physical = ctx->extent.physical;
                ctx->fresh = 0u;
                ctx->pc = WFS_WRITE_PC_BLOCK_READ;
                continue;
            }
            /* Nothing maps this block, so it must be allocated -- an append past
             * the end, or a hole being filled. One block at a time: a longer run
             * would have to be recorded before it is written, and a crash between
             * would leave the record naming blocks holding old content. */
            memset(&ctx->alloc, 0, sizeof(ctx->alloc));
            ctx->alloc.vol = ctx->vol;
            ctx->alloc.want = 1u;
            /* Locality: the group the object's last extent already sits in (§12).
             * Group 0 when the object has no extents yet. */
            ctx->alloc.prefer_group =
                ctx->obj.extent_count > 0u
                    ? (uint32_t)(ctx->obj.extents[ctx->obj.extent_count - 1u].physical_block /
                                 WFS_BLOCKS_PER_GROUP(ctx->vol->super.block_size))
                    : 0u;
            wfs_ops_task_reset(&ctx->alloc_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->alloc_task, wfs_alloc_blocks_task, &ctx->alloc)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->alloc_started = 1u;
            ctx->pc = WFS_WRITE_PC_ALLOC_JOINED;
            continue;

        case WFS_WRITE_PC_ALLOC_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->alloc_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->alloc_started = 0u;
            if (joined != 0) {
                WFS_FAIL(ctx, (wasmos_error_code_t)joined);
            }
            if (ctx->alloc.length == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NO_SPACE);
            }
            ctx->physical = ctx->alloc.first_block;
            ctx->fresh = 1u;
            /* Recorded inline while the record still has room. Once it does not,
             * the extent WAITS until its data block is on disk: a leaf is
             * reachable from the object record as soon as it is written, so
             * publishing the extent first would name a block still holding
             * whatever it held before. record_extent does not mutate the map when
             * it refuses, so using it as the test is safe. */
            if (ctx->obj.extent_tree_block == 0u &&
                record_extent(ctx, ctx->logical, ctx->alloc.first_block, 1u) == WASMOS_ERR_NONE) {
                ctx->tree_pending = 0u;
            } else {
                ctx->tree_pending = 1u;
                ctx->pending_logical = ctx->logical;
                ctx->pending_physical = ctx->alloc.first_block;
                ctx->pending_length = 1u;
            }
            ctx->pc = WFS_WRITE_PC_BLOCK_READ;
            continue;

        case WFS_WRITE_PC_BLOCK_READ:
            /* A freshly allocated block holds whatever it held before, so it is
             * NOT read: the untouched bytes are zeroed instead, which is what the
             * format promises for a range nothing has written.
             *
             * TODO: a full-block overwrite of an existing block also needs no
             * read; reading it costs one request per block on a large sequential
             * write. */
            if (ctx->fresh) {
                ctx->pc = WFS_WRITE_PC_BLOCK_PATCH;
                continue;
            }
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->physical), WFS_WRITE_PC_BLOCK_PATCH);
            /* fall through when the block was already staged */

        case WFS_WRITE_PC_BLOCK_PATCH:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            block_size = ctx->vol->super.block_size;
            in_block = (uint32_t)((ctx->offset + ctx->done) % block_size);
            chunk = chunk_len(ctx);
            d = wfs_block_data(b);
            if (ctx->fresh) {
                for (i = 0; i < block_size; ++i) {
                    d[i] = 0u;
                }
            }
            for (i = 0; i < chunk; ++i) {
                d[in_block + i] = ctx->src[ctx->done + i];
            }
            WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->physical), WFS_WRITE_PC_BLOCK_WRITTEN);
            /* fall through */

        case WFS_WRITE_PC_BLOCK_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            /* The data is on disk, so a held extent may now be published. The
             * bytes are NOT counted until it is: an extent that never reached the
             * map names content no reader can find, and reporting it as written
             * would tell the caller bytes landed that cannot be read back. */
            if (ctx->tree_pending) {
                if (ctx->obj.extent_tree_block == 0u) {
                    /* Promotion needs one block for the leaf. Allocated here
                     * rather than inside the add task, so the allocator stays a
                     * sibling sub-task instead of nesting a level deeper. */
                    memset(&ctx->alloc, 0, sizeof(ctx->alloc));
                    ctx->alloc.vol = ctx->vol;
                    ctx->alloc.want = 1u;
                    ctx->alloc.prefer_group =
                        ctx->pending_physical / WFS_BLOCKS_PER_GROUP(ctx->vol->super.block_size);
                    wfs_ops_task_reset(&ctx->alloc_task);
                    if (!wasmos_async_start(wfs_ops_runtime(),
                                            &ctx->alloc_task,
                                            wfs_alloc_blocks_task,
                                            &ctx->alloc)) {
                        WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
                    }
                    ctx->alloc_started = 1u;
                    ctx->pc = WFS_WRITE_PC_LEAF_ALLOC_JOINED;
                    continue;
                }
                start_xtadd(ctx, 0u);
                if (ctx->err != WASMOS_ERR_NONE) {
                    return (int32_t)ctx->err;
                }
                ctx->pc = WFS_WRITE_PC_XTADD_JOINED;
                continue;
            }
            ctx->done += chunk_len(ctx);
            ctx->fresh = 0u;
            ctx->pc = WFS_WRITE_PC_MAP;
            continue;

        case WFS_WRITE_PC_LEAF_ALLOC_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->alloc_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->alloc_started = 0u;
            if (joined != 0) {
                WFS_FAIL(ctx, (wasmos_error_code_t)joined);
            }
            if (ctx->alloc.length == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NO_SPACE);
            }
            start_xtadd(ctx, ctx->alloc.first_block);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->pc = WFS_WRITE_PC_XTADD_JOINED;
            continue;

        case WFS_WRITE_PC_XTADD_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->xtadd_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->xtadd_started = 0u;
            if (joined != 0) {
                WFS_FAIL(ctx, (wasmos_error_code_t)joined);
            }
            ctx->tree_pending = 0u;
            ctx->done += chunk_len(ctx);
            ctx->fresh = 0u;
            ctx->pc = WFS_WRITE_PC_MAP;
            continue;

        case WFS_WRITE_PC_RECORD_READ:
            /* The record LAST. It is what names the data, so a crash before this
             * leaves blocks allocated but unreferenced -- space fsck reclaims --
             * whereas a record written first would name blocks still holding what
             * they held before. */
            if (ctx->object_id == WFS_OBJECT_INVALID ||
                ctx->object_id >= ctx->vol->super.total_objects) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NOT_FOUND);
            }
            ctx->record_block = ctx->vol->super.object_table_start +
                                ctx->object_id / wfs_objects_per_block(ctx->vol->super.block_size);
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->record_block), WFS_WRITE_PC_RECORD_PATCH);
            /* fall through */

        case WFS_WRITE_PC_RECORD_PATCH:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            if (ctx->offset + (uint64_t)ctx->len > ctx->obj.size) {
                ctx->obj.size = ctx->offset + (uint64_t)ctx->len;
            }
            d = wfs_block_data(b) +
                (ctx->object_id % wfs_objects_per_block(ctx->vol->super.block_size)) *
                    WFS_OBJECT_SIZE;

            wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, size), ctx->obj.size);
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, extent_count), ctx->obj.extent_count);
            wfs_wr64(d,
                     (uint32_t)offsetof(struct wfs_object, extent_tree_block),
                     ctx->obj.extent_tree_block);
            wfs_wr16(d, (uint32_t)offsetof(struct wfs_object, flags), ctx->obj.flags);
            if (ctx->now_ns != 0u) {
                wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), ctx->now_ns);
                wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), ctx->now_ns);
            }
            if (ctx->obj.flags & WFS_OBJ_INLINE_DATA) {
                /* Inline content occupies the extents array's bytes (§7), so it is
                 * written over that region rather than beside it. */
                for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
                    d[(uint32_t)offsetof(struct wfs_object, extents) + i] = ctx->inline_data[i];
                }
            } else {
                for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
                    uint32_t e = (uint32_t)offsetof(struct wfs_object, extents) +
                                 i * (uint32_t)sizeof(struct wfs_extent);

                    if (i < ctx->obj.extent_count) {
                        wfs_wr64(d, e + 0u, ctx->obj.extents[i].logical_block);
                        wfs_wr64(d, e + 8u, ctx->obj.extents[i].physical_block);
                        wfs_wr32(d, e + 16u, ctx->obj.extents[i].length);
                        wfs_wr32(d, e + 20u, 0u);
                    } else {
                        wfs_wr64(d, e + 0u, 0u);
                        wfs_wr64(d, e + 8u, 0u);
                        wfs_wr32(d, e + 16u, 0u);
                        wfs_wr32(d, e + 20u, 0u);
                    }
                }
            }
            /* Reseal: an object record is checksummed under its object_id (§13),
             * and the reader validates it. */
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, checksum), 0u);
            wfs_wr32(d,
                     (uint32_t)offsetof(struct wfs_object, checksum),
                     wfs_checksum_struct(ctx->vol->super.uuid,
                                         ctx->object_id,
                                         d,
                                         WFS_OBJECT_SIZE,
                                         (uint32_t)offsetof(struct wfs_object, checksum)));
            WFS_AWAIT(
                ctx, wfs_block_write_begin(b, ctx->record_block), WFS_WRITE_PC_RECORD_WRITTEN);
            /* fall through */

        case WFS_WRITE_PC_RECORD_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            return WASMOS_WASM_TASK_COMPLETE;

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}
