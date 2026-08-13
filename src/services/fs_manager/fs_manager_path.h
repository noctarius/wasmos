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
int32_t fsmgr_route_path_for_mounts(const char* path, int32_t path_len,
                                    const char* const* mount_names, int32_t mount_count,
                                    int32_t allow_relative, int32_t* out_mount_index,
                                    char* out_path, int32_t out_path_cap, int32_t* out_path_len);

#endif
