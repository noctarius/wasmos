/* wfs_read.h - read bytes out of an object.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §16, §9.
 *
 * The last piece of the read path: a name resolves to an object (wfs_dir),
 * the object's map resolves a logical block (wfs_extent), and this copies the
 * bytes out. A task on the system coroutine runtime like the rest.
 */
#ifndef FS_WFS_WFS_READ_H
#define FS_WFS_WFS_READ_H

#include "wasmos/coroutine_wasm.h"
#include "wfs_block.h"
#include "wfs_ops.h"
#include "wfs_types.h"

/* Read up to `ctx->len` bytes at `ctx->offset` from `ctx->obj` into `ctx->dst`.
 *
 * Completes with `ctx->done` set to the bytes actually delivered, which is SHORT
 * of `len` when the request runs past the end of the object and 0 when it starts
 * there. A short read is not a failure: it is how a reader learns where the
 * object ends.
 *
 * A hole reads as zeroes (§9). A sparsely written object has logical ranges no
 * extent maps, and they are not an error — which is why this fills rather than
 * failing, and why a directory, which is never sparse, is refused here instead.
 *
 * Set `ctx->inline_data` when the object carries WFS_OBJ_INLINE_DATA; the
 * content lives in the object record and no block is read at all.
 *
 * Refuses a directory with FS_IS_DIR: a directory's bytes are records, and
 * handing them to a client as file content would leak the on-disk layout to
 * something that asked for a file.
 */
int32_t wfs_read_task(void* user, uintptr_t* out_value);

/* Prepare `ctx` for a read. `inline_data` may be NULL for an object without the
 * inline flag. */
void wfs_read_init(wfs_read_ctx_t* ctx, const wfs_volume_t* vol, const struct wfs_object* obj,
                   const uint8_t* inline_data, uint64_t offset, uint8_t* dst, uint32_t len);

#endif /* FS_WFS_WFS_READ_H */
