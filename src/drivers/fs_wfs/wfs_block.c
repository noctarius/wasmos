/* wfs_block.c - staging and cache-tag bookkeeping for the WFS block client.
 *
 * Everything that touches IPC is in wfs_block_submit_read / _write, which this
 * file deliberately does not define; see the seam note in wfs_block.h.
 */
#include "wfs_block.h"

/* No libc. Every field is set explicitly rather than through memset, which
 * keeps this file's dependencies at stdint plus its own headers — that is what
 * lets a host test link the reactor without standing up the driver ABI. */
void wfs_block_configure(wfs_block_t* b, int32_t block_endpoint, int32_t reply_endpoint) {
    b->block_endpoint = block_endpoint;
    b->reply_endpoint = reply_endpoint;
    b->buf_phys = -1;
    b->next_req_id = 1;
    b->block_size = WFS_BLOCK_SIZE_MIN;
    b->cur_req_id = 0;
    b->wait_block = WFS_BLOCK_NONE;
    b->wait_is_write = 0;
    b->staged_block = WFS_BLOCK_NONE;
    b->io_failed = 0;
    b->err = WASMOS_ERR_NONE;
    b->owner = 0;
}

wasmos_error_code_t wfs_block_set_block_size(wfs_block_t* b, uint32_t block_size) {
    if (block_size != 4096u && block_size != 8192u && block_size != 16384u) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    /* The staged block was read at the old size, so its contents no longer
     * describe a whole block at the new one. */
    if (block_size != b->block_size) {
        b->block_size = block_size;
        wfs_block_invalidate(b);
    }
    return WASMOS_ERR_NONE;
}

void wfs_block_invalidate(wfs_block_t* b) {
    b->staged_block = WFS_BLOCK_NONE;
}

void wfs_block_set_err(wfs_block_t* b, wasmos_error_code_t code) {
    b->err = code;
}

wfs_r_t wfs_block_need(wfs_block_t* b, uint32_t block) {
    /* Checked before the cache: a step resuming after a failed completion must
     * learn that it failed, not silently reissue the request. */
    if (b->io_failed) {
        b->io_failed = 0;
        return WFS_R_ERR;
    }
    if (b->staged_block == block) {
        return WFS_R_DONE;
    }
    if (b->cur_req_id != 0) {
        /* The reactor runs one op at a time and a step yields on submitting, so
         * reaching here means a step issued a second request without yielding
         * for the first. */
        wfs_block_set_err(b, WASMOS_ERR_FS_BUSY);
        return WFS_R_ERR;
    }
    return wfs_block_submit_read(b, block);
}

wfs_r_t wfs_block_write(wfs_block_t* b, uint32_t block) {
    if (b->io_failed) {
        b->io_failed = 0;
        return WFS_R_ERR;
    }
    if (b->cur_req_id != 0) {
        wfs_block_set_err(b, WASMOS_ERR_FS_BUSY);
        return WFS_R_ERR;
    }
    return wfs_block_submit_write(b, block);
}

void wfs_block_complete(wfs_block_t* b, int ok) {
    if (!ok) {
        /* A failed transfer leaves the buffer holding whatever the device or
         * the server left; tagging it would make those bytes look cached. */
        wfs_block_invalidate(b);
        wfs_block_set_err(b, WASMOS_ERR_FS_IO);
        b->io_failed = 1;
    } else {
        /* A write leaves the buffer holding what was just written, so the tag
         * stays valid for the block written, exactly as a read's does. */
        b->staged_block = b->wait_block;
    }
    b->cur_req_id = 0;
    b->wait_is_write = 0;
}
