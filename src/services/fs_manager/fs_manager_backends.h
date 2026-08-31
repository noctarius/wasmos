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

/* Turn what a backend REPORTED in FSMGR_IPC_BACKEND_INFO_RESP into the absolute
 * canonical mount path fs-manager holds.
 *
 * A backend may report its mount with or without a leading slash ("boot" and
 * "/boot" are the same mount, and both spellings are live in the tree), so one is
 * ensured rather than required. The result is lower-cased, so "/Boot" and "boot"
 * are one mount, and any trailing slash is dropped so "/boot/" is not a second
 * mount -- except for the root, which IS a single slash.
 *
 * "/" is a legal mount path and names the root filesystem. Routing matches the
 * longest mount path prefixing a request, so the root is the mount of last resort
 * rather than a name with nothing to match.
 *
 * Returns 1 with `out` holding a NUL-terminated absolute path, or 0 -- leaving
 * `out` empty -- when the report yields no mount:
 *
 * - An empty report, or one that is only slashes other than the root's single
 *   one. A backend MUST name its mount; nothing on this side knows where a
 *   backend belongs, so there is no default to fall back to.
 * - A path that does not fit out_cap. Refused rather than truncated, because a
 *   shortened mount path is a different mount.
 * - A NULL argument, or an out_cap below 3 (the shortest path plus a NUL is "/"
 *   and one leading slash may have to be added).
 *
 * The report is NOT otherwise canonicalized: an interior "." or ".." makes a
 * mount path nothing can route to, and is refused for that reason. */
int32_t fsmgr_mount_path_from_reported(const char* reported, char* out, uint32_t out_cap);

#endif
