/* wfs_path.h - resolve a path to an object.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15.
 *
 * Walks components from the root, reading a directory's record and scanning it
 * for each one. Both of those are tasks, so this one starts and joins them —
 * the runtime's own composition rather than a private convention.
 */
#ifndef FS_WFS_WFS_PATH_H
#define FS_WFS_WFS_PATH_H

#include "wasmos/coroutine_wasm.h"
#include "wfs_ops.h"
#include "wfs_types.h"

/* Resolve `path` (`len` bytes, absolute) into `ctx->object` / `ctx->object_id`.
 *
 * Completes with `ctx->found` 0 when a component does not exist, which is not a
 * failure: an open that misses is how a create learns it may proceed. On success
 * `ctx->object.out` holds the record and `ctx->object.inline_data` its inline
 * bytes, so a caller can read the object without fetching it again.
 *
 * "/" resolves to the root. Empty components are skipped, so "//etc///x" and
 * "/etc/x" name the same object and a trailing slash is harmless. A component
 * that is "." or ".." resolves through the records the directory carries, so
 * ".." from the root stays at the root.
 *
 * Refuses a path that is not absolute (FS_NOT_ABSOLUTE), one longer than
 * WFS_PATH_MAX (FS_PATH_TOO_LONG), and a component under something that is not
 * a directory (FS_NOT_DIR).
 */
int32_t wfs_path_task(void* user, uintptr_t* out_value);

/* Prepare `ctx` to resolve an ABSOLUTE `path`. Returns a packed code for a path
 * this walk will not accept, so a caller can reject without starting a task. */
wasmos_error_code_t wfs_path_init(wfs_path_ctx_t* ctx, const wfs_volume_t* vol, const char* path,
                                  uint32_t len);

/* Prepare `ctx` to resolve `path` from `start_object_id`.
 *
 * A path beginning with '/' is absolute and starts at the root regardless of
 * `start_object_id`; anything else is relative and starts there. This is what a
 * client's working directory needs: `cat hello.txt` inside a mount sends the
 * backend a bare name, and resolving it from the root would look for it in the
 * wrong directory — or find a different file of the same name.
 *
 * An empty path resolves to `start_object_id` itself. */
wasmos_error_code_t wfs_path_init_from(wfs_path_ctx_t* ctx, const wfs_volume_t* vol,
                                       uint32_t start_object_id, const char* path, uint32_t len);

#endif /* FS_WFS_WFS_PATH_H */
