/* fs_manager_types.h - shared structs and constants for the FS manager service */
#ifndef WASMOS_FS_MANAGER_TYPES_H
#define WASMOS_FS_MANAGER_TYPES_H

#include <stdint.h>

/* Slots per client-state chunk, not a global ceiling: the chunk list grows by
 * one FS_CLIENT_CHUNK_CAP-slot chunk from the bump heap whenever every existing
 * slot is taken, so the real limit is the heap. */
#define FS_CLIENT_CHUNK_CAP 32 /* max concurrent per-context client state slots */
/* Hard cap on simultaneously registered backends; a class event for a ninth
 * provider finds no free slot and is dropped. */
#define FS_BACKEND_CAP 8 /* max registered FS backend instances */
/* Hard cap on open files per client context.  Exhausting it fails the open with
 * WASMOS_ERR_FS_NO_FD, after fs-manager has closed the backend fd again. */
#define FSMGR_CLIENT_FD_CAP 32 /* max forwarded open files per client context */

/* Whether the client stands at a VFS root that NO filesystem is mounted at, or in
 * a directory some backend owns. Once a root filesystem is mounted "/" routes to
 * it like any other path, so FS_MOUNT_ROOT is the degenerate case rather than the
 * normal one. */
typedef enum { FS_MOUNT_ROOT = 0, FS_MOUNT_BACKEND = 1 } fs_mount_t;

/* One open file, as seen by a client.  fs-manager hands the client the slot's
 * index in fs_client_state_t.fds[] as the fd, and keeps here the backend that
 * actually owns the file plus the fd that backend issued; every fd-carrying
 * request is rewritten through this mapping before being forwarded.  The
 * indirection is what stops one backend's fd numbers from colliding with
 * another's. Meaningful only while in_use is non-zero. */
typedef struct {
    uint8_t in_use;
    int32_t backend_endpoint;
    int32_t backend_fd;
} fsmgr_client_fd_t;

/* Longest mount path fs-manager will hold, NUL included. A mount is an absolute
 * canonical path ("/", "/boot", "/mnt/usb"), so this bounds mount DEPTH as well
 * as name length -- it was 16 bytes while a mount was a single top-level name,
 * which is not enough for a path. A backend reporting a longer one is refused
 * rather than truncated: a shortened mount path is a different mount. */
#define FSMGR_MOUNT_PATH_MAX 64

/* One registered FS backend (e.g. a FAT driver instance).
 * has_meta: non-zero once PCI metadata has been filled in; the PCI fields below
 * are meaningless while it is 0.
 * mount_path: the absolute canonical path this backend is mounted at, taken from
 * what the backend reports in FSMGR_IPC_BACKEND_INFO and normalized by
 * fsmgr_mount_path_from_reported. Nothing on this side supplies a default. */
typedef struct {
    uint8_t in_use;
    uint8_t kind;     /* FSMGR_BACKEND_BLOCK / FSMGR_BACKEND_PSEUDO / other */
    int32_t endpoint; /* IPC endpoint for this backend driver */
    /* Which filesystem this backend serves (FS_TYPE_*, abi/constants.yaml),
     * reported by the backend in FSMGR_IPC_BACKEND_INFO_RESP. `kind` cannot
     * answer this: it separates a block-backed backend from the initfs one, so
     * every block-backed backend shares one value whatever it mounts.
     * FS_TYPE_UNKNOWN when the backend reports no type. */
    uint32_t fs_type;
    uint8_t slot; /* slot index in the backend table */
    uint8_t has_meta;
    uint8_t unit; /* block device unit number */
    uint8_t bus;
    uint8_t device_fn; /* PCI device+function byte from devmgr */
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    char mount_path[FSMGR_MOUNT_PATH_MAX];
} fs_backend_t;

/* Longest working directory fs-manager will hold for a client, NUL included.
 * A chdir whose result does not fit is refused, so no client can end up with a
 * silently shortened cwd. */
#define FSMGR_CWD_MAX 128

/* Per-IPC-context state: the client's working directory, plus the mount that
 * directory lives on and the backend serving it.
 *
 * cwd is the authority and is always a canonical absolute VFS path ("/",
 * "/wfs", "/wfs/docs"); mount and backend_endpoint are derived from it on every
 * chdir and cached here so a request need not re-route the directory itself.
 * Relative paths from the client are joined onto cwd before routing, which is
 * what makes a name mean the same directory for a spawned child as for its
 * spawner -- a child's state is a copy of its parent's cwd, not a fresh one. */
typedef struct {
    uint8_t in_use;
    /* Owning context of the requesting endpoint, so every endpoint a process
     * uses shares one cwd; falls back to the raw source endpoint id when the
     * owner cannot be resolved. */
    int32_t context_id;
    fs_mount_t mount;
    int32_t backend_endpoint; /* -1 only when no filesystem is mounted at "/" */
    char cwd[FSMGR_CWD_MAX];
    fsmgr_client_fd_t fds[FSMGR_CLIENT_FD_CAP];
} fs_client_state_t;

#endif
