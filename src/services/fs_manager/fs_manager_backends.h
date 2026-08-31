/* fs_manager_backends.h - backend-table decisions for the FS manager service
 *
 * What a mount's serving filesystem is CALLED: a pure function of the backend's
 * filesystem type, so it lives here rather than in fs_manager.c, which cannot be
 * linked without IPC and transfer-buffer stubs.
 * tests/unit/test_fs_manager_backends.c links this translation unit on its own.
 */
#ifndef WASMOS_FS_MANAGER_BACKENDS_H
#define WASMOS_FS_MANAGER_BACKENDS_H

#include <stdint.h>

#include "fs_manager_types.h"

/* Name of the filesystem serving a backend, for `mount` output.
 *
 * Derived from the backend's reported FS_TYPE_* (abi/constants.yaml) ALONE, and
 * never from fs_backend_t.kind. kind separates a block-backed backend from the
 * initfs one and carries no filesystem identity, so every block-backed backend
 * reports the same value whatever it mounts; naming from it reports a WFS volume
 * as FAT.
 *
 * Every filesystem is one row of the lookup, pseudo-filesystems included: initfs
 * reports FS_TYPE_INITFS rather than being recognised by its kind, so a future
 * devfs or sysfs is a value in the enum and not a case here. A backend reporting
 * FS_TYPE_UNKNOWN is named generically rather than guessed at.
 *
 * Returns a static string; never NULL, including for a NULL backend. */
const char* fsmgr_backend_fs_name(const fs_backend_t* backend);

#endif
