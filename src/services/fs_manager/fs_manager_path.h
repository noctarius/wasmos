/* fs_manager_path.h - path routing helper for the FS manager service */
#ifndef WASMOS_FS_MANAGER_PATH_H
#define WASMOS_FS_MANAGER_PATH_H

#include <stdint.h>

/* Match the first path segment against mount_names[] (case-insensitive, ASCII)
 * and write the tail after that segment into out_path, NUL-terminated; a path
 * that is exactly the mount name yields "/".  Returns 1 on match, with
 * *out_mount_index and *out_path_len set.  Returns 0 — leaving the outputs
 * untouched — when no mount matches, when the path is relative and
 * allow_relative is 0, or when the tail does not fit in out_path_cap; the caller
 * treats all three as not-routed. */
/* Resolve `arg` against the absolute VFS working directory `cwd`, writing the
 * canonical absolute result into out_path.
 *
 * The working directory is a full VFS path ("/", "/wfs", "/wfs/docs"): fs-manager
 * owns it, and every path a client names is made absolute here before it is
 * routed to a mount. That is what makes a relative name mean the same thing for
 * a spawned child as for its spawner, and what stops a name from reaching a
 * backend that never held the directory it was typed in.
 *
 * `arg` beginning with '/' replaces cwd outright; otherwise it extends it. "."
 * and "" keep the directory, ".." pops one segment and cannot escape the root,
 * and repeated or trailing slashes collapse. The result never has a trailing
 * slash except for the root itself.
 *
 * Returns 1 on success. Returns 0 — leaving out_path untouched — on a NULL
 * argument, an out_cap below 2, a cwd that is not absolute, or a result that
 * does not fit: the join REFUSES rather than truncating, because a truncated
 * path names a different file that the caller would then open unknowingly. */
int32_t fsmgr_cwd_join(const char* cwd, const char* arg, char* out_path, int32_t out_cap);

int32_t fsmgr_route_path_for_mounts(const char* path, int32_t path_len,
                                    const char* const* mount_names, int32_t mount_count,
                                    int32_t allow_relative, int32_t* out_mount_index,
                                    char* out_path, int32_t out_path_cap, int32_t* out_path_len);

#endif
