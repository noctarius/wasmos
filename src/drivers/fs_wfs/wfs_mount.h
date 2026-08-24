/* wfs_mount.h - the mount, group-descriptor and object-record coroutines.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15, §11, §7.
 *
 * Each of these is a resumable step (see wfs_co.h): it returns WFS_R_WAIT
 * having submitted a block request, and the reactor re-invokes it when that
 * request completes. None of them blocks, and none holds a pointer into the
 * staged block across a yield.
 */
#ifndef FS_WFS_WFS_MOUNT_H
#define FS_WFS_WFS_MOUNT_H

#include "wfs_block.h"
#include "wfs_types.h"

/* Read and verify group descriptor `ctx->group` into `ctx->out`.
 *
 * The descriptor is checksummed under its GROUP INDEX, not under the block that
 * holds it: it is one record among many in a shared block, so the index that
 * addresses it is what binds it to its slot (§13). A descriptor moved to
 * another slot therefore fails here.
 */
wfs_r_t wfs_group_step(wfs_group_ctx_t* ctx, wfs_block_t* b, const wfs_volume_t* vol);

/* Read and verify object record `ctx->object_id` into `ctx->out`.
 *
 * Objects are fixed-size and the table is contiguous, so the record's location
 * is a division rather than a walk. The record is checksummed under its
 * object_id (§13).
 */
wfs_r_t wfs_object_step(wfs_object_ctx_t* ctx, wfs_block_t* b, const wfs_volume_t* vol);

/* Mount `vol` from the volume behind `b`.
 *
 * Reads block 0, parses the superblock at its fixed byte offset, adopts the
 * block size, then walks the group descriptor table to verify every descriptor
 * before the volume is declared mounted. A descriptor that fails to verify is a
 * volume whose allocator would be reading a bitmap from a block number nothing
 * vouches for, so it fails the mount rather than waiting to be found in use.
 */
wfs_r_t wfs_mount_step(wfs_mount_ctx_t* ctx, wfs_block_t* b, wfs_volume_t* vol);

#endif /* FS_WFS_WFS_MOUNT_H */
