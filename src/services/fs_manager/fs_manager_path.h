/* fs_manager_path.h - path routing helper for the FS manager service */
#ifndef WASMOS_FS_MANAGER_PATH_H
#define WASMOS_FS_MANAGER_PATH_H

#include <stdint.h>

/* Match path against mount_names[] and write the tail (path after the mount
 * prefix) into out_path.  Returns 1 on match with *out_mount_index set;
 * returns 0 if no mount matches (caller should treat as not-found).
 * allow_relative: non-zero to accept paths that don't start with '/'. */
int32_t fsmgr_route_path_for_mounts(const char *path,
                                    int32_t path_len,
                                    const char *const *mount_names,
                                    int32_t mount_count,
                                    int32_t allow_relative,
                                    int32_t *out_mount_index,
                                    char *out_path,
                                    int32_t out_path_cap,
                                    int32_t *out_path_len);

#endif
