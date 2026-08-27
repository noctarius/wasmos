/* wfs_truncate.h - set an object's size, releasing or sparsening what changes
 * (§16).
 *
 * The crash-safety order is the REVERSE of a write's, for the same reason a
 * write's is what it is. A write puts data before the record, so a crash leaves
 * blocks allocated but unreferenced. A truncation puts the record before the
 * free, so a crash leaves the same thing. Both orders are chosen so that the
 * survivable outcome is a LEAK, which fsck reclaims -- never a block the record
 * still names but the bitmap has released, which a later allocation would hand to
 * a second object.
 */
#ifndef FS_WFS_WFS_TRUNCATE_H
#define FS_WFS_WFS_TRUNCATE_H

#include "wfs_types.h"

/* Prepare a truncation of `object_id` to `new_size`.
 *
 * `obj` and `inline_data` are the record as wfs_object_task read it; they are
 * copied. `now_ns` sets mtime and ctime, or leaves them alone when zero.
 */
void wfs_truncate_init(wfs_trunc_ctx_t* ctx, wfs_volume_t* vol, uint32_t object_id,
                       const struct wfs_object* obj, const uint8_t* inline_data, uint64_t new_size,
                       uint64_t now_ns);

/* Apply the truncation.
 *
 * GROWING allocates nothing: the new range is a hole, which reads as zeroes
 * (§9), so a grown file is sparse until something writes into it. SHRINKING frees
 * the blocks that fall wholly past the new end and zeroes the tail of the block
 * the new end falls inside -- without that, growing the file again would read
 * back bytes the truncation was supposed to have removed.
 *
 * An object whose map is an extent TREE is trimmed a run at a time
 * (wfs_extent_write.h), and a leaf left empty is released with the object put
 * back on an inline map. Where the new end falls INSIDE a surviving block, the
 * physical block behind it is resolved by descending the tree through
 * wfs_extent_task -- the same walk a reader makes -- so a tree can be truncated
 * to any size, not only a block boundary.
 *
 * An INLINE object that a grow takes past WFS_INLINE_DATA_MAX is promoted to an
 * extent map, as a write past it is: its bytes move into a first data block and
 * the flag clears.
 *
 * Fails with WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes,
 * WASMOS_ERR_FS_IS_DIR on a directory, WASMOS_ERR_FS_NO_SPACE when a promotion
 * cannot get its block, and WASMOS_ERR_FS_UNSUPPORTED for an interior extent
 * tree, which nothing writes.
 */
int32_t wfs_truncate_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_TRUNCATE_H */
