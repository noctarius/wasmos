/* wfs_alloc.h - block allocation over the group bitmaps (§12).
 *
 * The bitmaps are authoritative and the free counters are derived from them, so
 * allocation is: find a run of clear bits in a group, set them, write the bitmap
 * back, and only then adjust the counter. That ORDER is deliberate — the bitmap
 * is what the next mount believes, and a crash between the two leaves a stale
 * counter, which fsck rebuilds, rather than blocks that two objects both own.
 *
 * Locality policy, in the order §12 gives it: prefer the group holding the
 * parent, take a contiguous run where one exists, fall back to a shorter run,
 * fall back to another group.
 */
#ifndef FS_WFS_WFS_ALLOC_H
#define FS_WFS_WFS_ALLOC_H

#include "wfs_types.h"

/* Allocate up to ctx->want blocks, preferring ctx->prefer_group.
 *
 * On completion ctx->first_block and ctx->length name the run allocated, and
 * ctx->length may be SHORTER than requested — a caller that needs more comes
 * back for the remainder, which is what makes fragmented allocation expressible
 * without this task owning a loop over extents.
 *
 * Fails with WASMOS_ERR_FS_READ_ONLY when the volume does not permit writes,
 * WASMOS_ERR_FS_NO_SPACE when no group has a free block, and the block layer's
 * own code on an I/O failure.
 */
int32_t wfs_alloc_blocks_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_ALLOC_H */
