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

/* Turn the mount name a backend REPORTED in FSMGR_IPC_BACKEND_INFO_RESP into the
 * mount name fs-manager holds: a leading '/' is dropped and the result is
 * lower-cased, so "/Boot" and "boot" are one mount.
 *
 * Returns 1 with `out` holding a NUL-terminated name, or 0 -- leaving `out` empty
 * -- when the reported name yields none. Three things yield none, and the second
 * is the one that is easy to miss:
 *
 * - An empty reported name. A backend MUST name its mount; nothing on this side
 *   knows where a backend belongs, so there is no default to fall back to.
 * - A reported name of exactly "/". That names the VFS ROOT, which is a mount
 *   PATH and not a mount name: while routing matches a path's FIRST SEGMENT there
 *   is nothing in "/" for it to match. Registering it would seat a backend no
 *   path can reach, which still holds one of the FS_BACKEND_CAP slots and still
 *   prints a bare "/" entry into the root listing.
 * - A name that does not fit out_cap. Refused rather than truncated, because a
 *   shortened mount name names a different mount.
 *
 * A NULL argument or an out_cap below 2 is also a refusal. */
int32_t fsmgr_mount_name_from_reported(const char* reported, char* out, uint32_t out_cap);

#endif
