/* wfs_read.c - copy bytes out of an object's data.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §16, §9.
 */
#include "wfs_read.h"

#include "wfs_extent.h"

void wfs_read_init(wfs_read_ctx_t* ctx, const wfs_volume_t* vol, const struct wfs_object* obj,
                   const uint8_t* inline_data, uint64_t offset, uint8_t* dst, uint32_t len) {
    ctx->pc = WFS_READ_PC_START;
    ctx->vol = vol;
    ctx->obj = obj;
    ctx->inline_data = inline_data;
    ctx->offset = offset;
    ctx->dst = dst;
    ctx->len = len;
    ctx->done = 0u;
    ctx->logical = 0u;
    ctx->physical = 0u;
    ctx->err = WASMOS_ERR_NONE;
    ctx->extent_started = 0u;
    ctx->extent.pc = WFS_EXTENT_PC_START;
}

int32_t wfs_read_task(void* user, uintptr_t* out_value) {
    wfs_read_ctx_t* ctx = (wfs_read_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t block_size;
    uint32_t in_block;
    uint32_t chunk;
    uint32_t i;
    const uint8_t* src;
    int32_t jr;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_READ_PC_START:
            if (!ctx->vol || !ctx->obj || (!ctx->dst && ctx->len != 0u)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            /* A directory's bytes are records. Handing them out as file content
             * would leak the on-disk layout to something that asked for a
             * file. */
            if (ctx->obj->type == WFS_TYPE_DIR) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_IS_DIR);
            }

            /* Clamp to the object's end. A read starting at or past it delivers
             * nothing, which is how a reader learns where the object ends. */
            if (ctx->offset >= ctx->obj->size) {
                ctx->len = 0u;
                return WASMOS_WASM_TASK_COMPLETE;
            }
            if (ctx->obj->size - ctx->offset < (uint64_t)ctx->len) {
                ctx->len = (uint32_t)(ctx->obj->size - ctx->offset);
            }
            if (ctx->len == 0u) {
                return WASMOS_WASM_TASK_COMPLETE;
            }

            /* Inline content lives in the object record, so no block is read.
             * The decoded object cannot supply those bytes — the decode read
             * them as block numbers — so the caller passes them in. */
            if (ctx->obj->flags & WFS_OBJ_INLINE_DATA) {
                if (!ctx->inline_data) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
                }
                if (ctx->offset + ctx->len > WFS_INLINE_DATA_MAX) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
                }
                for (i = 0; i < ctx->len; ++i) {
                    ctx->dst[i] = ctx->inline_data[ctx->offset + i];
                }
                ctx->done = ctx->len;
                return WASMOS_WASM_TASK_COMPLETE;
            }

            ctx->pc = WFS_READ_PC_MAP;
            continue;

        case WFS_READ_PC_MAP:
            if (ctx->done >= ctx->len) {
                return WASMOS_WASM_TASK_COMPLETE;
            }
            block_size = ctx->vol->super.block_size;
            ctx->logical = (ctx->offset + ctx->done) / block_size;

            ctx->extent.pc = WFS_EXTENT_PC_START;
            ctx->extent.vol = ctx->vol;
            ctx->extent.obj = ctx->obj;
            ctx->extent.logical = ctx->logical;
            ctx->extent.err = WASMOS_ERR_NONE;
            wfs_ops_task_reset(&ctx->extent_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->extent_task, wfs_extent_task, &ctx->extent)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->extent_started = 1u;
            ctx->pc = WFS_READ_PC_JOIN;
            continue;

        case WFS_READ_PC_JOIN:
            jr = 0;
            if (wasmos_wasm_coroutine_join(&ctx->extent_task, &jr) == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->extent_started = 0u;
            if (ctx->extent.err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->extent.err;
            }

            block_size = ctx->vol->super.block_size;
            in_block = (uint32_t)((ctx->offset + ctx->done) % block_size);
            chunk = block_size - in_block;
            if (chunk > ctx->len - ctx->done) {
                chunk = ctx->len - ctx->done;
            }

            if (!ctx->extent.found) {
                /* A hole reads as zeroes (§9). A sparsely written object has
                 * ranges nothing maps, and they are content, not an error. */
                for (i = 0; i < chunk; ++i) {
                    ctx->dst[ctx->done + i] = 0u;
                }
                ctx->done += chunk;
                ctx->pc = WFS_READ_PC_MAP;
                continue;
            }
            ctx->physical = ctx->extent.physical;
            ctx->pc = WFS_READ_PC_BLOCK;
            continue;

        case WFS_READ_PC_BLOCK:
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->physical), WFS_READ_PC_COPY);
            /* fall through when the block was already staged */

        case WFS_READ_PC_COPY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            block_size = ctx->vol->super.block_size;
            in_block = (uint32_t)((ctx->offset + ctx->done) % block_size);
            chunk = block_size - in_block;
            if (chunk > ctx->len - ctx->done) {
                chunk = ctx->len - ctx->done;
            }
            src = wfs_block_data(b) + in_block;
            for (i = 0; i < chunk; ++i) {
                ctx->dst[ctx->done + i] = src[i];
            }
            ctx->done += chunk;
            ctx->pc = WFS_READ_PC_MAP;
            continue;

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}
