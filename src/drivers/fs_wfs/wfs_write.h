/* wfs_write.h - copy bytes into an object's data (§16).
 *
 * The mirror of wfs_read.c: the same extent walk decides where a byte belongs,
 * and where nothing is mapped the write ALLOCATES instead of reading zeroes.
 *
 * Update order within one write: data blocks first, then the object record. The
 * record is what names the data, so a crash after a block write and before the
 * record leaves blocks that are allocated but unreferenced -- space fsck
 * reclaims -- whereas a record written first would name blocks holding whatever
 * they held before, which is another object's data or garbage presented as this
 * object's content.
 */
#ifndef FS_WFS_WFS_WRITE_H
#define FS_WFS_WFS_WRITE_H

#include "wfs_types.h"

/* Prepare a write of `len` bytes from `src` at byte `offset` of `object_id`.
 *
 * `obj` and `inline_data` are the record as wfs_object_task read it; they are
 * COPIED, because the write updates them and seals them back. `now_ns` sets
 * mtime and ctime, or leaves them alone when zero.
 */
void wfs_write_init(wfs_write_ctx_t* ctx, wfs_volume_t* vol, uint32_t object_id,
                    const struct wfs_object* obj, const uint8_t* inline_data, uint64_t offset,
                    const uint8_t* src, uint32_t len, uint64_t now_ns);

/* Write the bytes and seal the record. ctx->done is how many landed.
 *
 * Fails with WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes,
 * WASMOS_ERR_FS_IS_DIR on a directory (its bytes are records, not content),
 * WASMOS_ERR_FS_NO_SPACE when the volume cannot supply a block, and
 * WASMOS_ERR_FS_UNSUPPORTED for the two growth cases that are not implemented:
 * an inline object that would outgrow WFS_INLINE_DATA_MAX, and an object that
 * would need more than WFS_INLINE_EXTENTS extents.
 */
int32_t wfs_write_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_WRITE_H */
