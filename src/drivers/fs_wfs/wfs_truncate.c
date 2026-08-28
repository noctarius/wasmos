/* wfs_truncate.c - set an object's size (§16). */
#include "wfs_truncate.h"
#include "wfs_endian.h"
#include "wfs_extent.h"
#include "wfs_extent_write.h"

#include <stddef.h>

#include "wfs_alloc.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_journal.h"
#include "wfs_ops.h"

void wfs_truncate_init(wfs_trunc_ctx_t* ctx, wfs_volume_t* vol, uint32_t object_id,
                       const struct wfs_object* obj, const uint8_t* inline_data, uint64_t new_size,
                       uint64_t now_ns) {
    uint32_t i;

    ctx->pc = WFS_TRUNC_PC_START;
    ctx->vol = vol;
    ctx->object_id = object_id;
    if (obj) {
        ctx->obj = *obj;
    }
    for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
        ctx->inline_data[i] = inline_data ? inline_data[i] : 0u;
    }
    ctx->new_size = new_size;
    ctx->now_ns = now_ns;
    ctx->free_count = 0u;
    ctx->free_index = 0u;
    ctx->tail_block = 0u;
    ctx->tail_from = 0u;
    ctx->tail_needed = 0u;
    ctx->record_block = 0u;
    ctx->free_started = 0u;
    ctx->trim_started = 0u;
    ctx->extent_started = 0u;
    ctx->promote_inline = 0u;
    ctx->promote_block = 0u;
    ctx->alloc_started = 0u;
    ctx->trim_keep = 0u;
    ctx->trim_root = 0u;
    ctx->err = WASMOS_ERR_NONE;
}

/* Note a run to release once the record no longer names it. */
static void note_free(wfs_trunc_ctx_t* ctx, uint32_t first, uint32_t length) {
    if (length == 0u || ctx->free_count >= WFS_INLINE_EXTENTS) {
        return;
    }
    ctx->free_first[ctx->free_count] = first;
    ctx->free_len[ctx->free_count] = length;
    ctx->free_count++;
}

/* Trim the extent array to the new size, collecting what stops being referenced.
 *
 * `keep` is the number of logical blocks the object retains. An extent wholly at
 * or past it is dropped and its blocks released; one that straddles it is
 * shortened and only its tail released. */
static void trim_extents(wfs_trunc_ctx_t* ctx, uint64_t keep) {
    uint32_t out = 0u;
    uint32_t i;

    for (i = 0; i < ctx->obj.extent_count && i < WFS_INLINE_EXTENTS; ++i) {
        struct wfs_extent e = ctx->obj.extents[i];

        if (e.logical_block >= keep) {
            note_free(ctx, (uint32_t)e.physical_block, e.length);
            continue;
        }
        if (e.logical_block + e.length > keep) {
            uint32_t survivors = (uint32_t)(keep - e.logical_block);

            note_free(ctx, (uint32_t)e.physical_block + survivors, e.length - survivors);
            e.length = survivors;
        }
        ctx->obj.extents[out] = e;
        out++;
    }
    for (i = out; i < WFS_INLINE_EXTENTS; ++i) {
        ctx->obj.extents[i].logical_block = 0u;
        ctx->obj.extents[i].physical_block = 0u;
        ctx->obj.extents[i].length = 0u;
        ctx->obj.extents[i].reserved = 0u;
    }
    ctx->obj.extent_count = out;
}

/* The physical block a retained logical block maps to, or 0 when nothing maps it.
 * Resolved from the in-memory array: an object with an extent TREE is refused
 * before this is reached, so no I/O is needed. */
static uint32_t physical_of(const wfs_trunc_ctx_t* ctx, uint64_t logical) {
    uint32_t i;

    for (i = 0; i < ctx->obj.extent_count && i < WFS_INLINE_EXTENTS; ++i) {
        const struct wfs_extent* e = &ctx->obj.extents[i];

        if (logical >= e->logical_block && logical < e->logical_block + e->length) {
            return (uint32_t)(e->physical_block + (logical - e->logical_block));
        }
    }
    return 0u;
}

int32_t wfs_truncate_task(void* user, uintptr_t* out_value) {
    wfs_trunc_ctx_t* ctx = (wfs_trunc_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t block_size;
    uint64_t keep;
    uint8_t* d;
    uint32_t i;
    int32_t joined;
    int jr;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_TRUNC_PC_START:
            if (!ctx->vol || !ctx->vol->mounted) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            if (ctx->obj.type == WFS_TYPE_DIR) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_IS_DIR);
            }
            if (ctx->vol->super.read_only) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
            }
            if (ctx->new_size == ctx->obj.size) {
                return WASMOS_WASM_TASK_COMPLETE;
            }
            /* An extent TREE is trimmed a run at a time by wfs_extent_trim_task,
             * which needs no bound on how many runs an object drops. */
            if (ctx->obj.extent_tree_block != 0u) {
                ctx->pc = WFS_TRUNC_PC_TREE_PREPARED;
                continue;
            }
            /* An inline object a grow takes past the record is PROMOTED, the same
             * way a write past it is (wfs_write.c): the bytes move into a first
             * data block and the flag clears. */
            if ((ctx->obj.flags & WFS_OBJ_INLINE_DATA) && ctx->new_size > WFS_INLINE_DATA_MAX) {
                ctx->promote_inline = 1u;
            }
            ctx->pc = WFS_TRUNC_PC_PREPARED;
            continue;

        case WFS_TRUNC_PC_TREE_PREPARED:
            if (ctx->new_size > ctx->obj.size) {
                /* A grow adds no extent: the range past the old end is a hole,
                 * which reads as zeroes (§9). Only the size moves. */
                ctx->obj.size = ctx->new_size;
                ctx->pc = WFS_TRUNC_PC_RECORD_READ;
                continue;
            }
            ctx->trim_root = (uint32_t)ctx->obj.extent_tree_block;
            /* Rounded UP, so a block the new end falls INSIDE stays allocated and
             * only its tail is cleared -- the same rule the inline route uses. */
            ctx->trim_keep =
                (ctx->new_size + ctx->vol->super.block_size - 1u) / ctx->vol->super.block_size;
            ctx->obj.size = ctx->new_size;
            ctx->pc = WFS_TRUNC_PC_TRIM_JOINED;
            continue;

        case WFS_TRUNC_PC_PREPARED:
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
                ctx->pc = WFS_TRUNC_PC_INLINE_ALLOC_JOINED;
                continue;
            }
            if (ctx->obj.flags & WFS_OBJ_INLINE_DATA) {
                /* Shrinking an inline object zeroes the bytes it removed. Leaving
                 * them would let a later grow read back content the truncation
                 * was supposed to have removed -- the record keeps its 144 bytes
                 * whatever the size says. */
                if (ctx->new_size < ctx->obj.size) {
                    for (i = (uint32_t)ctx->new_size; i < WFS_INLINE_DATA_MAX; ++i) {
                        ctx->inline_data[i] = 0u;
                    }
                }
                ctx->pc = WFS_TRUNC_PC_RECORD_READ;
                continue;
            }

            block_size = ctx->vol->super.block_size;
            if (ctx->new_size > ctx->obj.size) {
                /* GROWING allocates nothing. The new range is a hole, which reads
                 * as zeroes (§9), so the file is sparse until something writes
                 * into it -- and a hole costs no block. */
                ctx->pc = WFS_TRUNC_PC_RECORD_READ;
                continue;
            }
            /* Blocks retained: the new size rounded UP, so a partial final block
             * stays allocated and only its tail is cleared. */
            keep = (ctx->new_size + block_size - 1u) / block_size;
            trim_extents(ctx, keep);
            if ((ctx->new_size % block_size) != 0u) {
                ctx->tail_block = physical_of(ctx, ctx->new_size / block_size);
                ctx->tail_from = (uint32_t)(ctx->new_size % block_size);
                ctx->tail_needed = ctx->tail_block != 0u ? 1u : 0u;
            }
            ctx->pc = ctx->tail_needed ? WFS_TRUNC_PC_TAIL_READ : WFS_TRUNC_PC_RECORD_READ;
            continue;

        case WFS_TRUNC_PC_INLINE_ALLOC_JOINED:
            joined = 0;
            jr = wasmos_wasm_coroutine_join(&ctx->alloc_task, &joined);
            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->alloc_started = 0u;
            if (jr != 0 || joined != 0) {
                WFS_FAIL(ctx, joined != 0 ? (wasmos_error_code_t)joined : (wasmos_error_code_t)jr);
            }
            if (ctx->alloc.length == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NO_SPACE);
            }
            ctx->promote_block = ctx->alloc.first_block;
            /* The record's bytes at the offset they were at, and zeroes for the
             * rest: the range past the old end is content nothing has written.
             * The block is fresh, so it is not read first. */
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            block_size = ctx->vol->super.block_size;
            d = wfs_block_data(b);
            for (i = 0; i < block_size; ++i) {
                d[i] = 0u;
            }
            for (i = 0; i < WFS_INLINE_DATA_MAX && (uint64_t)i < ctx->obj.size; ++i) {
                d[i] = ctx->inline_data[i];
            }
            WFS_AWAIT(
                ctx, wfs_block_write_begin(b, ctx->promote_block), WFS_TRUNC_PC_INLINE_WRITTEN);
            continue;

        case WFS_TRUNC_PC_INLINE_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            /* The content is on disk, so the map may name it. The flag goes in the
             * same update: while it is set a reader takes these bytes as inline
             * content rather than as an extent (§7). */
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
            /* A promotion only happens on a GROW, and a grow allocates nothing
             * further: the range past the block just written is a hole. */
            ctx->pc = WFS_TRUNC_PC_RECORD_READ;
            continue;

        case WFS_TRUNC_PC_TAIL_READ:
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->tail_block), WFS_TRUNC_PC_TAIL_WRITTEN);
            /* fall through when the block was already staged */

        case WFS_TRUNC_PC_TAIL_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            if (ctx->tail_needed) {
                /* Zero from the new end to the end of its block, then write it
                 * back. The block stays allocated, so these bytes are what a grow
                 * would read; leaving them is how removed content comes back. */
                block_size = ctx->vol->super.block_size;
                d = wfs_block_data(b);
                for (i = ctx->tail_from; i < block_size; ++i) {
                    d[i] = 0u;
                }
                ctx->tail_needed = 0u;
                WFS_AWAIT(
                    ctx, wfs_block_write_begin(b, ctx->tail_block), WFS_TRUNC_PC_TAIL_WRITTEN);
                continue;
            }
            ctx->pc = WFS_TRUNC_PC_RECORD_READ;
            continue;

        case WFS_TRUNC_PC_RECORD_READ:
            if (ctx->object_id == WFS_OBJECT_INVALID ||
                ctx->object_id >= ctx->vol->super.total_objects) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NOT_FOUND);
            }
            ctx->record_block = ctx->vol->super.object_table_start +
                                ctx->object_id / wfs_objects_per_block(ctx->vol->super.block_size);
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->record_block), WFS_TRUNC_PC_RECORD_PATCH);
            /* fall through */

        case WFS_TRUNC_PC_RECORD_PATCH:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->obj.size = ctx->new_size;
            d = wfs_block_data(b) +
                (ctx->object_id % wfs_objects_per_block(ctx->vol->super.block_size)) *
                    WFS_OBJECT_SIZE;

            wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, size), ctx->obj.size);
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, extent_count), ctx->obj.extent_count);
            /* Written because a trim can CLEAR it: a leaf emptied by the trim is
             * released, and a record still naming it would send the next reader
             * to a freed block. */
            wfs_wr64(d,
                     (uint32_t)offsetof(struct wfs_object, extent_tree_block),
                     ctx->obj.extent_tree_block);
            wfs_wr16(d, (uint32_t)offsetof(struct wfs_object, flags), ctx->obj.flags);
            if (ctx->now_ns != 0u) {
                wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), ctx->now_ns);
                wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), ctx->now_ns);
            }
            if (ctx->obj.flags & WFS_OBJ_INLINE_DATA) {
                for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
                    d[(uint32_t)offsetof(struct wfs_object, extents) + i] = ctx->inline_data[i];
                }
            } else {
                for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
                    uint32_t e = (uint32_t)offsetof(struct wfs_object, extents) +
                                 i * (uint32_t)sizeof(struct wfs_extent);

                    wfs_wr64(d, e + 0u, ctx->obj.extents[i].logical_block);
                    wfs_wr64(d, e + 8u, ctx->obj.extents[i].physical_block);
                    wfs_wr32(d, e + 16u, ctx->obj.extents[i].length);
                    wfs_wr32(d, e + 20u, 0u);
                }
            }
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, checksum), 0u);
            wfs_wr32(d,
                     (uint32_t)offsetof(struct wfs_object, checksum),
                     wfs_checksum_struct(ctx->vol->super.uuid,
                                         ctx->object_id,
                                         d,
                                         WFS_OBJECT_SIZE,
                                         (uint32_t)offsetof(struct wfs_object, checksum)));
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->record_block), WFS_TRUNC_PC_RECORD_WRITTEN);
            /* fall through */

        case WFS_TRUNC_PC_RECORD_WRITTEN:
            ctx->err = wfs_txn_stage_take(ctx->vol);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->pc = WFS_TRUNC_PC_FREE_JOINED;
            continue;

        case WFS_TRUNC_PC_TRIM_JOINED:
            /* One run per step. The leaf is rewritten without a run before that
             * run is released, so every step leaves the tree readable and an
             * interruption leaks blocks rather than freeing a referenced one. */
            if (ctx->trim_started) {
                joined = 0;
                jr = wasmos_wasm_coroutine_join(&ctx->trim_task, &joined);
                if (jr == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                ctx->trim_started = 0u;
                if (jr != 0 || joined != 0) {
                    WFS_FAIL(ctx,
                             joined != 0 ? (wasmos_error_code_t)joined : (wasmos_error_code_t)jr);
                }
                /* The step's record count comes off the object's own total: for
                 * a tree, no single step can recompute it without reading every
                 * leaf. */
                if (ctx->trim.removed != 0u && ctx->obj.extent_count >= ctx->trim.removed) {
                    ctx->obj.extent_count -= ctx->trim.removed;
                }
                if (ctx->trim.freed_length != 0u || ctx->trim.freed_node != 0u) {
                    wfs_ops_task_reset(&ctx->free_task);
                    memset(&ctx->free_ctx, 0, sizeof(ctx->free_ctx));
                    ctx->free_ctx.vol = ctx->vol;
                    if (ctx->trim.freed_node != 0u) {
                        /* A leaf the trim emptied out of an interior tree. It is
                         * METADATA, so its number is revoked as it is freed
                         * (§18): the log may still hold an image of it, and the
                         * block may be a file's next. */
                        ctx->free_ctx.first_block = ctx->trim.freed_node;
                        ctx->free_ctx.length = 1u;
                        ctx->free_ctx.metadata = 1u;
                    } else {
                        ctx->free_ctx.first_block = ctx->trim.freed_first;
                        ctx->free_ctx.length = ctx->trim.freed_length;
                    }
                    if (!wasmos_async_start(wfs_ops_runtime(),
                                            &ctx->free_task,
                                            wfs_free_blocks_task,
                                            &ctx->free_ctx)) {
                        WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
                    }
                    ctx->free_started = 1u;
                    ctx->pc = WFS_TRUNC_PC_TRIM_FREE_JOINED;
                    continue;
                }
                /* Nothing reaches past the cut. An empty map is released with the
                 * root, so the object goes back to an inline map. */
                if (ctx->trim.remaining == 0u) {
                    ctx->obj.extent_count = 0u;
                }
                if (ctx->trim.remaining != 0u &&
                    (ctx->new_size % ctx->vol->super.block_size) != 0u) {
                    /* The new end falls inside a block that survived. Which
                     * PHYSICAL block that is takes a descent through the tree, so
                     * the read path resolves it -- the same walk a reader makes. */
                    memset(&ctx->extent, 0, sizeof(ctx->extent));
                    ctx->extent.pc = WFS_EXTENT_PC_START;
                    ctx->extent.vol = ctx->vol;
                    ctx->extent.obj = &ctx->obj;
                    ctx->extent.logical = ctx->new_size / ctx->vol->super.block_size;
                    ctx->extent.err = WASMOS_ERR_NONE;
                    wfs_ops_task_reset(&ctx->extent_task);
                    if (!wasmos_async_start(
                            wfs_ops_runtime(), &ctx->extent_task, wfs_extent_task, &ctx->extent)) {
                        WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
                    }
                    ctx->extent_started = 1u;
                    ctx->pc = WFS_TRUNC_PC_TAIL_LOOKUP_JOINED;
                    continue;
                }
                if (ctx->trim.remaining == 0u) {
                    ctx->obj.extent_tree_block = 0u;
                    wfs_ops_task_reset(&ctx->free_task);
                    memset(&ctx->free_ctx, 0, sizeof(ctx->free_ctx));
                    ctx->free_ctx.vol = ctx->vol;
                    ctx->free_ctx.first_block = ctx->trim_root;
                    ctx->free_ctx.length = 1u;
                    /* The leaf is a metadata block, so its number is revoked as
                     * it is freed: an image of it stays replayable until then,
                     * and the block may be a file's next (§18). */
                    ctx->free_ctx.metadata = 1u;
                    ctx->trim_root = 0u;
                    if (!wasmos_async_start(wfs_ops_runtime(),
                                            &ctx->free_task,
                                            wfs_free_blocks_task,
                                            &ctx->free_ctx)) {
                        WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
                    }
                    ctx->free_started = 1u;
                    ctx->pc = WFS_TRUNC_PC_TRIM_FREE_JOINED;
                    continue;
                }
                ctx->pc = WFS_TRUNC_PC_RECORD_READ;
                continue;
            }
            memset(&ctx->trim, 0, sizeof(ctx->trim));
            ctx->trim.pc = WFS_XTTRIM_PC_START;
            ctx->trim.vol = ctx->vol;
            ctx->trim.node_block = ctx->trim_root;
            ctx->trim.keep = ctx->trim_keep;
            wfs_ops_task_reset(&ctx->trim_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->trim_task, wfs_extent_trim_task, &ctx->trim)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->trim_started = 1u;
            continue;

        case WFS_TRUNC_PC_TAIL_LOOKUP_JOINED:
            joined = 0;
            jr = wasmos_wasm_coroutine_join(&ctx->extent_task, &joined);
            if (jr == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->extent_started = 0u;
            if (ctx->extent.err != WASMOS_ERR_NONE) {
                WFS_FAIL(ctx, ctx->extent.err);
            }
            /* Nothing mapping it means the new end falls in a HOLE, and a hole has
             * no bytes to clear. */
            if (ctx->extent.found) {
                ctx->tail_block = ctx->extent.physical;
                ctx->tail_from = (uint32_t)(ctx->new_size % ctx->vol->super.block_size);
                ctx->tail_needed = 1u;
                ctx->pc = WFS_TRUNC_PC_TAIL_READ;
                continue;
            }
            ctx->pc = WFS_TRUNC_PC_RECORD_READ;
            continue;

        case WFS_TRUNC_PC_TRIM_FREE_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->free_task, &joined) == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->free_started = 0u;
            if (jr != 0 || joined != 0) {
                WFS_FAIL(ctx, joined != 0 ? (wasmos_error_code_t)joined : (wasmos_error_code_t)jr);
            }
            /* The leaf's own release is the last step: the map is already cleared,
             * so there is nothing more to trim. */
            if (ctx->trim_root == 0u) {
                ctx->pc = WFS_TRUNC_PC_RECORD_READ;
                continue;
            }
            ctx->pc = WFS_TRUNC_PC_TRIM_JOINED;
            continue;

        case WFS_TRUNC_PC_FREE_JOINED:
            /* The blocks are released only NOW, after the record has stopped
             * naming them. The reverse order would let a crash leave the record
             * pointing at blocks the bitmap has freed, and the next allocation
             * would hand them to a second object. This way a crash leaves them
             * allocated and unreferenced, which fsck reclaims. */
            for (;;) {
                if (ctx->free_started) {
                    joined = 0;
                    jr = wasmos_wasm_coroutine_join(&ctx->free_task, &joined);
                    if (jr == WASMOS_WASM_AWAIT_PENDING) {
                        return WASMOS_WASM_TASK_YIELDED;
                    }
                    ctx->free_started = 0u;
                    if (jr != 0 || joined != 0) {
                        WFS_FAIL(ctx,
                                 joined != 0 ? (wasmos_error_code_t)joined
                                             : (wasmos_error_code_t)jr);
                    }
                    ctx->free_index++;
                }
                if (ctx->free_index >= ctx->free_count) {
                    return WASMOS_WASM_TASK_COMPLETE;
                }
                wfs_ops_task_reset(&ctx->free_task);
                memset(&ctx->free_ctx, 0, sizeof(ctx->free_ctx));
                ctx->free_ctx.vol = ctx->vol;
                ctx->free_ctx.first_block = ctx->free_first[ctx->free_index];
                ctx->free_ctx.length = ctx->free_len[ctx->free_index];
                if (!wasmos_async_start(
                        wfs_ops_runtime(), &ctx->free_task, wfs_free_blocks_task, &ctx->free_ctx)) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
                }
                ctx->free_started = 1u;
            }

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}

/* Run this operation as one transaction. The cheap refusals come first so a call
 * that was never going to write does not open one. */
wasmos_error_code_t wfs_truncate_run(wfs_trunc_ctx_t* ctx) {
    wasmos_error_code_t rc;
    int32_t status;

    if (!ctx || !ctx->vol || !ctx->vol->mounted) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (ctx->vol->super.read_only) {
        return WASMOS_ERR_FS_READ_ONLY;
    }
    rc = wfs_txn_open(ctx->vol);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    status = wfs_ops_run(wfs_truncate_task, ctx);
    if (status != 0) {
        wfs_txn_abort(ctx->vol);
        return (wasmos_error_code_t)status;
    }
    return wfs_txn_close(ctx->vol);
}
