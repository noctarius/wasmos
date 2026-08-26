/* wfs_truncate.c - set an object's size (§16). */
#include "wfs_truncate.h"

#include <stddef.h>

#include "wfs_alloc.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_ops.h"
#include "wfs_sync.h"

static void wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr64(uint8_t* p, uint32_t off, uint64_t v) {
    wr32(p, off, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p, off + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

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
    ctx->dirty_started = 0u;
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
            /* An extent TREE cannot be trimmed: wfs_extent.c reads leaf and
             * interior nodes but nothing writes them, so a trim would have to
             * rewrite a structure this driver cannot produce. */
            if (ctx->obj.extent_tree_block != 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
            }
            /* An inline object cannot outgrow the record, for the same reason a
             * write into one cannot: promotion needs the inline bytes read before
             * an extent is written over them. */
            if ((ctx->obj.flags & WFS_OBJ_INLINE_DATA) && ctx->new_size > WFS_INLINE_DATA_MAX) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
            }
            if (ctx->new_size == ctx->obj.size) {
                return WASMOS_WASM_TASK_COMPLETE;
            }
            wfs_ops_task_reset(&ctx->dirty_task);
            ctx->dirty.pc = WFS_DIRTY_PC_START;
            ctx->dirty.vol = ctx->vol;
            ctx->dirty.err = WASMOS_ERR_NONE;
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->dirty_task, wfs_mark_dirty_task, &ctx->dirty)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->dirty_started = 1u;
            ctx->pc = WFS_TRUNC_PC_DIRTY_JOINED;
            continue;

        case WFS_TRUNC_PC_DIRTY_JOINED:
            joined = 0;
            if (wasmos_wasm_coroutine_join(&ctx->dirty_task, &joined) ==
                WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->dirty_started = 0u;
            if (joined != 0) {
                WFS_FAIL(ctx, (wasmos_error_code_t)joined);
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

            wr64(d, (uint32_t)offsetof(struct wfs_object, size), ctx->obj.size);
            wr32(d, (uint32_t)offsetof(struct wfs_object, extent_count), ctx->obj.extent_count);
            wr16(d, (uint32_t)offsetof(struct wfs_object, flags), ctx->obj.flags);
            if (ctx->now_ns != 0u) {
                wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), ctx->now_ns);
                wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), ctx->now_ns);
            }
            if (ctx->obj.flags & WFS_OBJ_INLINE_DATA) {
                for (i = 0; i < WFS_INLINE_DATA_MAX; ++i) {
                    d[(uint32_t)offsetof(struct wfs_object, extents) + i] = ctx->inline_data[i];
                }
            } else {
                for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
                    uint32_t e = (uint32_t)offsetof(struct wfs_object, extents) +
                                 i * (uint32_t)sizeof(struct wfs_extent);

                    wr64(d, e + 0u, ctx->obj.extents[i].logical_block);
                    wr64(d, e + 8u, ctx->obj.extents[i].physical_block);
                    wr32(d, e + 16u, ctx->obj.extents[i].length);
                    wr32(d, e + 20u, 0u);
                }
            }
            wr32(d, (uint32_t)offsetof(struct wfs_object, checksum), 0u);
            wr32(d,
                 (uint32_t)offsetof(struct wfs_object, checksum),
                 wfs_checksum_struct(ctx->vol->super.uuid,
                                     ctx->object_id,
                                     d,
                                     WFS_OBJECT_SIZE,
                                     (uint32_t)offsetof(struct wfs_object, checksum)));
            WFS_AWAIT(
                ctx, wfs_block_write_begin(b, ctx->record_block), WFS_TRUNC_PC_RECORD_WRITTEN);
            /* fall through */

        case WFS_TRUNC_PC_RECORD_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->pc = WFS_TRUNC_PC_FREE_JOINED;
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
                    if (wasmos_wasm_coroutine_join(&ctx->free_task, &joined) ==
                        WASMOS_WASM_AWAIT_PENDING) {
                        return WASMOS_WASM_TASK_YIELDED;
                    }
                    ctx->free_started = 0u;
                    if (joined != 0) {
                        WFS_FAIL(ctx, (wasmos_error_code_t)joined);
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
