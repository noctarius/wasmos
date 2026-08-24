/* wfs_dir.c - walk a directory's records.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §10, §13.
 */
#include "wfs_dir.h"

#include "wfs_crc32c.h"
#include "wfs_extent.h"

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

void wfs_dir_lookup_init(wfs_dir_ctx_t* ctx, const wfs_volume_t* vol, const struct wfs_object* dir,
                         const char* name, uint32_t name_len) {
    uint32_t i;

    /* Field by field rather than memset: this file carries no libc dependency,
     * which is what lets the host suite link it without the driver ABI. */
    ctx->pc = WFS_DIR_PC_START;
    ctx->vol = vol;
    ctx->dir = dir;
    ctx->want = name;
    ctx->want_len = name_len;
    ctx->logical = 0u;
    ctx->offset = 0u;
    ctx->physical = 0u;
    ctx->err = WASMOS_ERR_NONE;
    ctx->extent_started = 0u;
    ctx->extent.pc = WFS_EXTENT_PC_START;
    ctx->extent.vol = vol;
    ctx->extent.obj = dir;
    ctx->found = 0u;
    ctx->object_id = 0u;
    ctx->type = 0u;
    ctx->name_length = 0u;
    for (i = 0; i <= WFS_NAME_MAX; ++i) {
        ctx->name[i] = '\0';
    }
}

/* Exact byte comparison. Names carry no encoding the driver interprets, so two
 * that differ in case are two entries (§10). */
static int name_equals(const uint8_t* rec, uint32_t name_length, const char* want,
                       uint32_t want_len) {
    uint32_t i;

    if (name_length != want_len) {
        return 0;
    }
    for (i = 0; i < want_len; ++i) {
        if ((char)rec[WFS_DIR_ENTRY_HEADER + i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/* Copy the record's name into the context. See the lifetime note on
 * wfs_dir_ctx_t: a pointer into the staged block dies at the next await. */
static void take_entry(wfs_dir_ctx_t* ctx, const uint8_t* rec, uint32_t object_id,
                       uint32_t name_length) {
    uint32_t i;

    ctx->found = 1u;
    ctx->object_id = object_id;
    ctx->type = rec[11];
    ctx->name_length = (uint8_t)name_length;
    for (i = 0; i < name_length; ++i) {
        ctx->name[i] = (char)rec[WFS_DIR_ENTRY_HEADER + i];
    }
    ctx->name[name_length] = '\0';
}

int32_t wfs_dir_task(void* user, uintptr_t* out_value) {
    wfs_dir_ctx_t* ctx = (wfs_dir_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t block_size;
    uint32_t usable;
    uint64_t blocks;
    const uint8_t* blk;
    const uint8_t* rec;
    uint32_t record_length;
    uint32_t name_length;
    uint32_t object_id;
    int32_t jr;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_DIR_PC_START:
            if (!ctx->vol || !ctx->dir) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            if (ctx->dir->type != WFS_TYPE_DIR) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NOT_DIR);
            }
            if (ctx->want_len > WFS_NAME_MAX) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NAME);
            }
            block_size = ctx->vol->super.block_size;
            /* A directory occupies whole blocks: its records are addressed
             * within a block and a partial one could not carry a tail. */
            if (ctx->dir->size == 0u || (ctx->dir->size % block_size) != 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            ctx->found = 0u;
            ctx->pc = WFS_DIR_PC_MAP;
            continue;

        case WFS_DIR_PC_MAP:
            block_size = ctx->vol->super.block_size;
            blocks = ctx->dir->size / block_size;
            if (ctx->logical >= blocks) {
                return WASMOS_WASM_TASK_COMPLETE; /* exhausted; found stays 0 */
            }
            ctx->extent.pc = WFS_EXTENT_PC_START;
            ctx->extent.vol = ctx->vol;
            ctx->extent.obj = ctx->dir;
            ctx->extent.logical = ctx->logical;
            ctx->extent.err = WASMOS_ERR_NONE;
            wfs_ops_task_reset(&ctx->extent_task);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->extent_task, wfs_extent_task, &ctx->extent)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->extent_started = 1u;
            ctx->pc = WFS_DIR_PC_JOIN;
            continue;

        case WFS_DIR_PC_JOIN:
            jr = 0;
            if (wasmos_wasm_coroutine_join(&ctx->extent_task, &jr) == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->extent_started = 0u;
            if (ctx->extent.err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->extent.err;
            }
            /* A directory is never sparse. A hole here means a block of entries
             * is missing, not that it is full of zeroes. */
            if (!ctx->extent.found) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            ctx->physical = ctx->extent.physical;
            ctx->pc = WFS_DIR_PC_READ;
            continue;

        case WFS_DIR_PC_READ:
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->physical), WFS_DIR_PC_SCAN);
            /* fall through when the block was already staged */

        case WFS_DIR_PC_SCAN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            block_size = ctx->vol->super.block_size;
            usable = wfs_dir_usable_bytes(block_size);
            blk = wfs_block_data(b);

            /* The tail carries the block's checksum, so it is verified before
             * any record in the block is believed (§10, §13). */
            if (rd32(blk, usable + (uint32_t)offsetof(struct wfs_dir_tail, checksum)) !=
                wfs_checksum_struct(ctx->vol->super.uuid,
                                    ctx->physical,
                                    blk,
                                    block_size,
                                    usable + (uint32_t)offsetof(struct wfs_dir_tail, checksum))) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
            }

            while (ctx->offset + WFS_DIR_ENTRY_HEADER <= usable) {
                rec = blk + ctx->offset;
                record_length = rd16(rec, 8u);
                name_length = rec[10];
                object_id = (uint32_t)rd64(rec, 0u);

                /* The stride is validated before it is used. A length below the
                 * minimum — 0 above all — would make this loop never advance,
                 * and one not a multiple of 8 would leave the next record's
                 * object_id misaligned (§10). */
                if (record_length < WFS_DIR_RECORD_MIN || (record_length & 7u) != 0u ||
                    ctx->offset + record_length > usable) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
                }
                if (name_length > record_length - WFS_DIR_ENTRY_HEADER) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
                }

                /* object_id 0 is free space: a removed entry, or the tail, which
                 * a scan that knows nothing about it skips under the same rule
                 * (§10). */
                if (object_id != WFS_OBJECT_INVALID && name_length != 0u) {
                    if (ctx->want_len == 0u) {
                        take_entry(ctx, rec, object_id, name_length);
                        ctx->offset += record_length;
                        return WASMOS_WASM_TASK_COMPLETE;
                    }
                    if (name_equals(rec, name_length, ctx->want, ctx->want_len)) {
                        take_entry(ctx, rec, object_id, name_length);
                        ctx->offset += record_length;
                        return WASMOS_WASM_TASK_COMPLETE;
                    }
                }
                ctx->offset += record_length;
            }

            ctx->offset = 0u;
            ctx->logical++;
            ctx->pc = WFS_DIR_PC_MAP;
            continue;

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}
