/* wfs_fd.c - open-file bookkeeping. See the header. */
#include "wfs_fd.h"

#define WFS_FD_BASE 3

void wfs_fd_table_init(wfs_fd_table_t* t) {
    uint32_t i;

    for (i = 0; i < WFS_MAX_OPEN; ++i) {
        t->files[i].in_use = 0u;
        t->files[i].owner = -1;
    }
}

int32_t wfs_fd_open(wfs_fd_table_t* t, int32_t owner, uint32_t object_id, uint64_t size,
                    uint16_t type, uint16_t flags, const uint8_t* inline_data) {
    uint32_t i;
    uint32_t k;

    for (i = 0; i < WFS_MAX_OPEN; ++i) {
        if (t->files[i].in_use) {
            continue;
        }
        t->files[i].in_use = 1u;
        t->files[i].owner = owner;
        t->files[i].object_id = object_id;
        t->files[i].size = size;
        t->files[i].offset = 0u;
        t->files[i].type = type;
        t->files[i].flags = flags;
        for (k = 0; k < sizeof(t->files[i].inline_data); ++k) {
            t->files[i].inline_data[k] = inline_data ? inline_data[k] : 0u;
        }
        return (int32_t)i + WFS_FD_BASE;
    }
    return WASMOS_ERR_FS_NO_FD;
}

wfs_open_file_t* wfs_fd_get(wfs_fd_table_t* t, int32_t owner, int32_t fd) {
    int32_t slot = fd - WFS_FD_BASE;

    if (slot < 0 || (uint32_t)slot >= WFS_MAX_OPEN) {
        return 0;
    }
    if (!t->files[slot].in_use || t->files[slot].owner != owner) {
        /* Another client's number resolves to nothing, not to their file. */
        return 0;
    }
    return &t->files[slot];
}

wasmos_error_code_t wfs_fd_close(wfs_fd_table_t* t, int32_t owner, int32_t fd) {
    wfs_open_file_t* f = wfs_fd_get(t, owner, fd);

    if (!f) {
        return WASMOS_ERR_FS_BAD_FD;
    }
    f->in_use = 0u;
    f->owner = -1;
    return WASMOS_ERR_NONE;
}

void wfs_fd_close_all(wfs_fd_table_t* t, int32_t owner) {
    uint32_t i;

    for (i = 0; i < WFS_MAX_OPEN; ++i) {
        if (t->files[i].in_use && t->files[i].owner == owner) {
            t->files[i].in_use = 0u;
            t->files[i].owner = -1;
        }
    }
}

int64_t wfs_fd_seek(wfs_open_file_t* f, int64_t delta, int32_t whence) {
    int64_t base;
    int64_t target;

    if (!f) {
        return WASMOS_ERR_FS_BAD_FD;
    }
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (int64_t)f->offset;
    } else if (whence == 2) {
        base = (int64_t)f->size;
    } else {
        return WASMOS_ERR_FS_BAD_ARGS;
    }

    target = base + delta;
    /* A negative result is refused rather than clamped: a caller that computed
     * one has a bug, and clamping would hide it behind a plausible read. Seeking
     * PAST the end is allowed — that is how a sparse write positions itself —
     * and the read path reports the short read. */
    if (target < 0) {
        return WASMOS_ERR_FS_RANGE;
    }
    f->offset = (uint64_t)target;
    return target;
}
