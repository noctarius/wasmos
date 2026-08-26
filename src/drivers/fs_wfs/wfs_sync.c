/* wfs_sync.c - recording a volume's mount state on disk (§4). */
#include "wfs_sync.h"

#include <stddef.h>

#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_ops.h"

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

int32_t wfs_mark_dirty_task(void* user, uintptr_t* out_value) {
    wfs_dirty_ctx_t* ctx = (wfs_dirty_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint8_t* sb;

    (void)out_value;

    switch (ctx->pc) {
    case WFS_DIRTY_PC_START:
        if (!ctx->vol || !ctx->vol->mounted) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
        }
        /* Once per mount. A caller may start this before every write and pay for
         * it only the first time, which is what keeps the flag off the per-write
         * cost without the caller tracking it. */
        if (ctx->vol->dirty_marked) {
            return WASMOS_WASM_TASK_COMPLETE;
        }
        /* Marking a read-only volume would make a mount that was never written
         * look like an interrupted write, costing the NEXT mount its writability
         * for nothing. */
        if (ctx->vol->super.read_only) {
            WFS_FAIL(ctx, WASMOS_ERR_FS_READ_ONLY);
        }
        /* Block 0 carries the primary superblock at a fixed byte offset (§4). */
        WFS_AWAIT(ctx, wfs_block_read_begin(b, 0u), WFS_DIRTY_PC_SUPER_READY);
        /* fall through when the block was already staged */

    case WFS_DIRTY_PC_SUPER_READY:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        sb = wfs_block_data(b) + WFS_SUPER_OFFSET;
        wr32(sb, (uint32_t)offsetof(struct wfs_superblock, state), (uint32_t)WFS_STATE_DIRTY);
        /* Reseal, because the checksum covers `state`. A patch without this
         * leaves a volume that does not validate at all, which the next mount
         * reads as a corrupt filesystem rather than a dirty one -- strictly worse
         * than not marking it. The primary is seeded with location 0 (§13). */
        wr32(sb, (uint32_t)offsetof(struct wfs_superblock, checksum), 0u);
        wr32(sb,
             (uint32_t)offsetof(struct wfs_superblock, checksum),
             wfs_checksum_struct(ctx->vol->super.uuid,
                                 0u,
                                 sb,
                                 WFS_SUPER_SIZE,
                                 (uint32_t)offsetof(struct wfs_superblock, checksum)));
        WFS_AWAIT(ctx, wfs_block_write_begin(b, 0u), WFS_DIRTY_PC_SUPER_WRITTEN);
        /* fall through */

    case WFS_DIRTY_PC_SUPER_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        /* Recorded only after the write landed: a failed write must leave the
         * caller starting this again rather than believing the volume is
         * marked. */
        ctx->vol->dirty_marked = 1u;
        ctx->vol->super.state = (uint32_t)WFS_STATE_DIRTY;
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}
