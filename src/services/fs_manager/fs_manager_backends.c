/* fs_manager_backends.c - backend-table decisions for the FS manager service */
#include "fs_manager_backends.h"

/* FS_TYPE_* and FSMGR_BACKEND_* both arrive through this header, which pulls
 * the generated abi/constants.yaml values. */
#include "wasmos_driver_abi.h"
/* wasmos_sys_strcasecmp: NULL-safe ASCII compare, the same helper fs_manager.c
 * matches mount names with. */
#include "wasmos/libsys_string.h"

const char* fsmgr_backend_fs_name(const fs_backend_t* backend) {
    if (!backend) {
        return "fs";
    }
    /* The initfs backend is identified by kind because it is not block-backed
     * and probes no superblock, so it reports no FS_TYPE_*. */
    if (backend->kind == FSMGR_BACKEND_INIT) {
        return "fs-init";
    }
    switch (backend->fs_type) {
    case FS_TYPE_FAT:
        return "fs-fat";
    case FS_TYPE_WFS:
        return "fs-wfs";
    default:
        return "fs";
    }
}

int32_t fsmgr_select_root_backend(const fs_backend_t* backends, uint32_t count) {
    if (!backends) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (backends[i].in_use &&
            wasmos_sys_strcasecmp(backends[i].mount_name, FSMGR_ROOT_MOUNT_NAME) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}
