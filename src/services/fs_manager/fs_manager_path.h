#ifndef WASMOS_FS_MANAGER_PATH_H
#define WASMOS_FS_MANAGER_PATH_H

#include <stdint.h>

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
