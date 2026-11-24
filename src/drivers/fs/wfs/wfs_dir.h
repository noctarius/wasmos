/* wfs_dir.h - the directory scan.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §10, §13.
 *
 * One task serves both a name lookup and a readdir walk, because they are the
 * same scan with a different stopping condition. A directory is stored as
 * regular file data, so its blocks are reached through the extent map: this task
 * runs wfs_extent_task as a child rather than addressing blocks itself.
 */
#ifndef FS_WFS_WFS_DIR_H
#define FS_WFS_WFS_DIR_H

#include "wasmos/coroutine_wasm.h"
#include "wfs_block.h"
#include "wfs_ops.h"
#include "wfs_types.h"

/* Scan `ctx->dir` from the cursor (`ctx->logical` / `ctx->offset`).
 *
 * With `ctx->want_len` non-zero, runs until a record's name equals
 * `ctx->want[0..want_len)` or the directory is exhausted. With `want_len` zero,
 * stops at the first record that names anything — which is how a readdir walks:
 * run it, consume `ctx->object_id` / `ctx->name`, run it again.
 *
 * Completes with `ctx->found` 0 when the directory is exhausted; that is not a
 * failure. On a hit, the cursor is left just past the record reported.
 *
 * Names are byte strings compared exactly: no case folding and no normalisation,
 * unlike FAT's short names. Two names that differ in case are two entries.
 *
 * Fails with a packed code for a directory whose size is not a whole number of
 * blocks, a block whose tail checksum does not verify, a record stride that is
 * not a legal multiple of 8 or would run past the tail, or a hole where a block
 * of entries should be — a directory is never sparse, so a hole means entries
 * are missing rather than that they are zero.
 */
int32_t wfs_dir_task(void* user, uintptr_t* out_value);

/* Prepare `ctx` to look `name` up in `dir`. `name_len` may be 0 to walk entries
 * instead. Resets the cursor to the start of the directory. */
void wfs_dir_lookup_init(wfs_dir_ctx_t* ctx, const wfs_volume_t* vol, const struct wfs_object* dir,
                         const char* name, uint32_t name_len);

#endif /* FS_WFS_WFS_DIR_H */
