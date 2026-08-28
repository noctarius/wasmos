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
 * WASMOS_ERR_FS_UNSUPPORTED only when an object outgrows a single extent-tree
 * leaf, which is wfs_extent_leaf_capacity() extents; splitting a leaf and adding
 * an interior level are not implemented.
 *
 * Growth itself is handled: an inline object outgrowing WFS_INLINE_DATA_MAX is
 * promoted to an extent map, and an object outgrowing WFS_INLINE_EXTENTS extents
 * is promoted to a tree (wfs_extent_write.h).
 */
int32_t wfs_write_task(void* user, uintptr_t* out_value);

/* Run wfs_write_task as one journal transaction (§14), which is how the driver
 * reaches it: the record it seals is metadata and belongs in the log, and a
 * write that reached the device but not the record would otherwise leave blocks
 * allocated to an object that does not name them.
 *
 * The DATA the write copies is not journaled (§17), so a crash between the data
 * and the commit leaves the metadata consistent and the newly allocated blocks
 * holding whatever they held before.
 *
 * `ctx->done` is meaningful whether this succeeds or fails: a partial write
 * reports what landed, and a client resending from zero would duplicate bytes
 * already on disk.
 */
wasmos_error_code_t wfs_write_run(wfs_write_ctx_t* ctx);

#endif /* FS_WFS_WFS_WRITE_H */
