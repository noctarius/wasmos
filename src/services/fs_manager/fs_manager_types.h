/* fs_manager_types.h - shared structs and constants for the FS manager service */
#ifndef WASMOS_FS_MANAGER_TYPES_H
#define WASMOS_FS_MANAGER_TYPES_H

#include <stdint.h>

#define FS_CLIENT_CHUNK_CAP 32 /* max concurrent per-context client state slots */
#define FS_BACKEND_CAP 8       /* max registered FS backend instances */
#define FSMGR_CLIENT_FD_CAP 32 /* max forwarded open files per client context */

/* Whether a request is being handled at the VFS root or forwarded to a backend. */
typedef enum { FS_MOUNT_ROOT = 0, FS_MOUNT_BACKEND = 1 } fs_mount_t;

typedef struct {
    uint8_t in_use;
    int32_t backend_endpoint;
    int32_t backend_fd;
} fsmgr_client_fd_t;

/* One registered FS backend (e.g. a FAT driver instance).
 * has_meta: non-zero once PCI metadata has been filled in; the PCI fields below
 * are meaningless while it is 0.
 * mount_name: defaulted on registration from kind+slot (e.g. "boot", "user"),
 * then overwritten by the name the backend reports in FSMGR_IPC_BACKEND_INFO. */
typedef struct {
    uint8_t in_use;
    uint8_t kind;     /* FSMGR_BACKEND_BOOT / FSMGR_BACKEND_INIT / other */
    int32_t endpoint; /* IPC endpoint for this backend driver */
    uint8_t slot;     /* slot index in the backend table */
    uint8_t has_meta;
    uint8_t unit; /* block device unit number */
    uint8_t bus;
    uint8_t device_fn; /* PCI device+function byte from devmgr */
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    char mount_name[16];
} fs_backend_t;

/* Per-IPC-context state: tracks which mount point a client is currently in
 * and the forwarded backend endpoint.  mount_depth counts nested path
 * segments stripped so far during forwarded operations. */
typedef struct {
    uint8_t in_use;
    /* Owning context of the requesting endpoint, so every endpoint a process
     * uses shares one cwd; falls back to the raw source endpoint id when the
     * owner cannot be resolved. */
    int32_t context_id;
    fs_mount_t mount;
    int32_t backend_endpoint; /* -1 when request is at the VFS root */
    uint16_t mount_depth;
    fsmgr_client_fd_t fds[FSMGR_CLIENT_FD_CAP];
} fs_client_state_t;

#endif
