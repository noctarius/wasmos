/* wfs_alloc.h - block allocation over the group bitmaps (§12).
 *
 * The bitmaps are authoritative and the free counters are derived from them, so
 * allocation is: find a run of clear bits in a group, set them, write the bitmap
 * back, and only then adjust the counter.
 *
 * Every task here is a TRANSACTION PARTICIPANT (wfs_journal.h): it stages its
 * blocks into the caller's open transaction rather than writing them, and is
 * refused outright when there is none. The operation that opens the transaction
 * is what makes the bitmap and the counter land together — the ORDER above is
 * kept because it still governs a crash before the commit, which discards the
 * transaction and leaves whichever of the two writes had reached the log.
 *
 * Freeing a run that held METADATA revokes each of its blocks (§18); see
 * wfs_free_ctx_t's `metadata`.
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

/* Release ctx->length blocks starting at ctx->first_block.
 *
 * The run may span groups and is freed one group at a time. Order within each
 * group mirrors allocation: the BITMAP first, then the derived counter, so a
 * crash leaves a counter fsck rebuilds rather than a bitmap that disagrees with
 * itself.
 *
 * A caller must have already stopped REFERENCING these blocks -- for a
 * truncation that means the object record is written first. Freeing bits the
 * record still names would let a later allocation hand the same blocks to a
 * second object.
 */
int32_t wfs_free_blocks_task(void* user, uintptr_t* out_value);

/* Claim an object record, initialised so that it is VALID the moment its bit is
 * set. ctx->object_id names it on completion.
 *
 * The record is written BEFORE the bitmap bit, which is the opposite of the block
 * case and deliberate. For a block the bitmap is the only thing that makes it
 * taken; for an object the record is what makes the id mean anything, and an id
 * whose bit is set while its record does not verify reads as a CORRUPT
 * filesystem. Writing the record first means a crash leaves at worst a valid
 * record in a slot the bitmap still calls free -- invisible, and overwritten by
 * the next allocation.
 *
 * Fails with WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes and
 * WASMOS_ERR_FS_NO_SPACE when no group has a free record.
 */
int32_t wfs_alloc_object_task(void* user, uintptr_t* out_value);

/* Release the object record ctx->object_id names.
 *
 * Only the bitmap bit and the derived counter change; the record's bytes are left
 * as they are. A caller must have already removed every reference to it, which
 * for an unlink means the directory entry is gone from disk first -- freeing a
 * record something still names would let a later create hand the same id out
 * twice.
 */
int32_t wfs_free_object_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_ALLOC_H */
