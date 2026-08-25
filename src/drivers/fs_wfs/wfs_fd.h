/* wfs_fd.h - the open-file table.
 *
 * An fd is scoped to the CLIENT that opened it: the same number from a different
 * source endpoint is a different file, or none. Without that a client could read
 * another's file by guessing a number, and fds are small integers.
 *
 * Pure bookkeeping — no I/O, no coroutines — so it is host-testable on its own.
 */
#ifndef FS_WFS_WFS_FD_H
#define FS_WFS_WFS_FD_H

#include <stdint.h>

#include "wfs_status.h"

#define WFS_MAX_OPEN 32u

typedef struct {
    uint8_t in_use;
    int32_t owner; /* the source endpoint that opened it */
    uint32_t object_id;
    uint64_t size;   /* latched at open; the read path clamps against it */
    uint64_t offset; /* the seek cursor */
    uint16_t type;
    uint16_t flags;           /* the object's flags, for the inline path */
    uint8_t inline_data[144]; /* WFS_INLINE_DATA_MAX; see wfs_read.h */
} wfs_open_file_t;

typedef struct {
    wfs_open_file_t files[WFS_MAX_OPEN];
} wfs_fd_table_t;

void wfs_fd_table_init(wfs_fd_table_t* t);

/* Claim a slot for `owner`. Returns the fd, or a packed code when the table is
 * full. Fds start at 3, leaving 0..2 to the standard streams a client already
 * has. */
int32_t wfs_fd_open(wfs_fd_table_t* t, int32_t owner, uint32_t object_id, uint64_t size,
                    uint16_t type, uint16_t flags, const uint8_t* inline_data);

/* The file `fd` names for `owner`, or NULL. A number belonging to another client
 * resolves to NULL rather than to their file. */
wfs_open_file_t* wfs_fd_get(wfs_fd_table_t* t, int32_t owner, int32_t fd);

/* Release `fd`. Returns WASMOS_ERR_NONE, or FS_BAD_FD when `owner` does not hold
 * it. */
wasmos_error_code_t wfs_fd_close(wfs_fd_table_t* t, int32_t owner, int32_t fd);

/* Release every fd `owner` holds. Called when a client goes away, so its slots
 * do not leak for the life of the driver. */
void wfs_fd_close_all(wfs_fd_table_t* t, int32_t owner);

/* Apply a seek. `whence` is 0=set, 1=cur, 2=end, matching the FS ABI. Returns
 * the new offset, or a packed code for an unknown whence or a result that would
 * be negative. */
int64_t wfs_fd_seek(wfs_open_file_t* f, int64_t delta, int32_t whence);

#endif /* FS_WFS_WFS_FD_H */
