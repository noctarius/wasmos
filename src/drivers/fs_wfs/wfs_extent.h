/* wfs_extent.h - the extent-map walk.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §9.
 *
 * Maps a logical block of an object to the physical block holding it, and to the
 * length of the contiguous run starting there. This is what every read above it
 * goes through: a directory's records live in the directory's data blocks, so
 * even a name lookup comes here first.
 *
 * A task on the system coroutine runtime, like the rest (see wfs_types.h).
 */
#ifndef FS_WFS_WFS_EXTENT_H
#define FS_WFS_WFS_EXTENT_H

#include "wasmos/coroutine_wasm.h"
#include "wfs_block.h"
#include "wfs_types.h"

/* Resolve `ctx->logical` in `ctx->obj`'s map into `ctx->physical` / `ctx->run`.
 *
 * Set `ctx->vol`, `ctx->obj` and `ctx->logical` before starting the task.
 *
 * Completes with `ctx->found` 0 when no extent covers the logical block: that is
 * a hole, which reads as zeroes and is NOT a failure. Fails with a packed code
 * when the map itself is unusable — a node that does not verify, a capacity or
 * entry count the block cannot hold, a child outside the volume, or a descent
 * that does not shrink the depth.
 *
 * An object with an inline map completes without reading anything, so a small
 * file costs no metadata block beyond its record.
 */
int32_t wfs_extent_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_EXTENT_H */
