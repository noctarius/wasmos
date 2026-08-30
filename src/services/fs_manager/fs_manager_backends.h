/* fs_manager_backends.h - backend-table decisions for the FS manager service
 *
 * The two questions fs-manager answers from its registration table alone: what
 * a mount's serving filesystem is CALLED, and which backend is the ROOT
 * filesystem. Both are pure functions of the table, so they live here rather
 * than in fs_manager.c, which cannot be linked without IPC and transfer-buffer
 * stubs. tests/unit/test_fs_manager_backends.c links this translation unit on
 * its own.
 */
#ifndef WASMOS_FS_MANAGER_BACKENDS_H
#define WASMOS_FS_MANAGER_BACKENDS_H

#include <stdint.h>

#include "fs_manager_types.h"

/* Mount name that identifies the root filesystem: the volume holding the system
 * tree, which is where an absolute path naming no mount is served. */
#define FSMGR_ROOT_MOUNT_NAME "boot"

/* Name of the filesystem serving a backend, for `mount` output.
 *
 * Derived from the backend's reported FS_TYPE_* (abi/constants.yaml), never
 * from fs_backend_t.kind. kind separates a block-backed backend from the initfs
 * one and carries no filesystem identity, so every block-backed backend reports
 * the same value whatever it mounts; naming from it reports a WFS volume as
 * FAT. A backend reporting FS_TYPE_UNKNOWN is named generically rather than
 * guessed at.
 *
 * Returns a static string; never NULL, including for a NULL backend. */
const char* fsmgr_backend_fs_name(const fs_backend_t* backend);

/* Index of the backend serving paths that name no mount, or -1 when none is
 * registered yet.
 *
 * The VFS root lists the mounts, but absolute paths like "/system/utils/ip" and
 * "/apps/calculator" name directories on the boot volume rather than mounts, so
 * they are served by this backend.
 *
 * Selection is by MOUNT NAME (FSMGR_ROOT_MOUNT_NAME, matched case-insensitively),
 * which is the property that makes a backend the root one. Selecting the first
 * block-backed backend in slot order instead makes the root filesystem a
 * function of registration order: one block-backed backend registers per mounted
 * volume, so any volume that registered first would serve every unrouted
 * absolute path.
 *
 * Returning -1 before the boot volume registers is meaningful and distinct from
 * index 0: a caller must not route to a backend that is merely first.
 *
 * `backends` is an array of `count` entries; entries with in_use == 0 are
 * skipped. */
int32_t fsmgr_select_root_backend(const fs_backend_t* backends, uint32_t count);

#endif
