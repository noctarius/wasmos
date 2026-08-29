/* fat_block.c - single-buffer block-I/O layer.  See fat_block.h.  State is
 * caller-owned (fat_block_t); no globals.  The driver issues no PIO or DMA
 * itself (that is the block server's job).  Staged transfers are one 512-byte
 * sector each and blk->sector is a 1-sector cache tagged by loaded_lba; only
 * fat_block_read_direct moves more than a sector, and it lands in the client's
 * buffer without touching the cache. */
#include "fat_block.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h" /* wasmos_sys_ipc_report_discard */
#include "wasmos_driver_abi.h"

void fat_block_configure(fat_block_t* blk, int32_t block_endpoint, int32_t reply_endpoint,
                         uint32_t target) {
    blk->sector_bytes = FAT_SECTOR_SIZE;
    /* The FSInfo accounting starts unknown: a caller that did not zero the
     * struct would otherwise present a garbage free count as valid and write it
     * back to the volume on the first allocation. */
    blk->free_count = 0;
    blk->free_count_valid = 0;
    blk->block_endpoint = block_endpoint;
    blk->reply_endpoint = reply_endpoint;
    blk->target = target;
    blk->req_bid = -1;
    blk->buf_phys = -1;
    blk->next_req_id = 1;
    blk->cur_req_id = 0;
    blk->wait_lba = 0;
    blk->wait_resp_type = 0;
    blk->copy_into_sector = 0;
    blk->direct_read = 0;
    blk->direct_pending = 0;
    blk->direct_sectors = 0;
    blk->write_pending = 0;
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    blk->owner = 0;
}

int fat_block_setup(fat_block_t* blk) {
    blk->buf_phys = wasmos_block_buffer_phys();
    if (blk->buf_phys < 0) {
        return -1;
    }
    /* Acquire and lend the request buffer ONCE. Doing it per transfer would be
     * the expensive shape people assume a descriptor forces; it is not the shape
     * the descriptor asks for, and a re-grant would fail ALREADY_BORROWED
     * anyway. */
    blk->req_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(wasmos_block_request_t));
    if (blk->req_bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_borrow(blk->block_endpoint, blk->req_bid, WASMOS_BUFFER_GRANT_READ) <
        0) {
        (void)wasmos_xfer_buffer_release(blk->req_bid);
        blk->req_bid = -1;
        return -1;
    }
    return 0;
}

/* Stage the outstanding request and hand its buffer to the server. Returns 0, or
 * -1 when the request could not be written or sent. */
static int fat_block_submit(fat_block_t* blk, int32_t req_type, const wasmos_block_request_t* req) {
    if (blk->req_bid < 0 ||
        wasmos_xfer_buffer_write(blk->req_bid, req, (int32_t)sizeof(*req), 0) != 0) {
        return -1;
    }
    return wasmos_ipc_send(blk->block_endpoint,
                           blk->reply_endpoint,
                           req_type,
                           blk->cur_req_id,
                           blk->req_bid,
                           0,
                           (int32_t)sizeof(*req),
                           0);
}

uint8_t* fat_block_sector(fat_block_t* blk) {
    return blk->sector;
}

int fat_block_idle(const fat_block_t* blk) {
    return blk->cur_req_id == 0;
}

void fat_block_set_owner(fat_block_t* blk, fat_op_ctx_t* ctx) {
    blk->owner = ctx;
}

void fat_block_set_err(fat_block_t* blk, int32_t err) {
    if (blk->owner) {
        blk->owner->err = err;
    }
}

void fat_block_invalidate(fat_block_t* blk) {
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
}

void fat_block_set_free_count(fat_block_t* blk, uint32_t count) {
    if (!blk) {
        return;
    }
    blk->free_count = count;
    blk->free_count_valid = 1;
}

int fat_block_set_sector_bytes(fat_block_t* blk, uint32_t bytes) {
    if (!blk || bytes < FAT_SECTOR_SIZE || bytes > FAT_MAX_SECTOR_BYTES ||
        (bytes % FAT_SECTOR_SIZE) != 0) {
        return -1;
    }
    blk->sector_bytes = bytes;
    return 0;
}

/* Submit one block transfer of blk->sector_bytes (count = 1).  Returns 0, or -1
 * when the layer is unconfigured, when staging the write into the block buffer
 * failed, or when the request could not be sent. */
static int fat_block_start(fat_block_t* blk, uint32_t lba, int rw) {
    int32_t req_type;

    if (blk->block_endpoint < 0 || blk->reply_endpoint < 0 || blk->buf_phys < 0) {
        return -1;
    }
    if (rw == 1 /* write */ &&
        wasmos_block_buffer_write(
            blk->buf_phys, addr_cast(int32_t, blk->sector), (int32_t)blk->sector_bytes, 0) != 0) {
        return -1;
    }

    blk->cur_req_id = blk->next_req_id++;
    if (blk->next_req_id < 1) {
        blk->next_req_id = 1;
    }
    blk->wait_lba = lba;
    blk->wait_resp_type = rw ? BLOCK_IPC_WRITE_RESP : BLOCK_IPC_READ_RESP;
    blk->copy_into_sector = rw ? 0u : 1u;
    req_type = rw ? BLOCK_IPC_WRITE_REQ : BLOCK_IPC_READ_REQ;

    wasmos_block_request_t req = {0};
    req.version = BLOCK_REQUEST_VERSION;
    req.target = blk->target;
    req.lba = lba;
    req.sector_count = 1u;
    req.dst_kind = BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = (uint32_t)blk->buf_phys;
    if (fat_block_submit(blk, req_type, &req) != 0) {
        blk->cur_req_id = 0;
        return -1;
    }
    return 0;
}

uint32_t fat_block_direct_sectors(const fat_block_t* blk) {
    return blk->direct_sectors;
}

int32_t fat_block_server_endpoint(const fat_block_t* blk) {
    return blk->block_endpoint;
}

fat_r_t fat_block_read_direct(fat_block_t* blk, uint32_t lba, uint32_t count, int32_t buffer_id,
                              int32_t borrow_id, uint32_t dst_offset) {
    /* The coroutine macros re-invoke this on resume (the case label sits before
     * the call), so completion must be reported on the second entry — the same
     * yield-once contract fat_block_write uses. fat_need_sector gets away
     * without a flag only because its sector cache answers the repeat call. */
    if (blk->direct_pending) {
        blk->direct_pending = 0;
        return FAT_R_DONE;
    }
    /* FIXME: both FAT_R_ERR paths below leave owner->err at 0, so the reactor
     * reports the failure as a bare -1 instead of a packed WASMOS_ERR_FS_* code
     * (fat_send_response in fs_fat.c substitutes -1 when err is unset). */
    if (blk->block_endpoint < 0 || blk->reply_endpoint < 0 || buffer_id < 0 || count == 0 ||
        count > WASMOS_BLOCK_ZC_MAX_SECTORS) {
        return FAT_R_ERR;
    }
    blk->cur_req_id = blk->next_req_id++;
    if (blk->next_req_id < 1) {
        blk->next_req_id = 1;
    }
    blk->wait_lba = lba;
    blk->wait_resp_type = BLOCK_IPC_READ_RESP;
    /* Nothing lands in the staged sector, so do not pull it on completion and do
     * not retag the cache: whatever it held before is still what it holds. */
    blk->copy_into_sector = 0u;
    blk->direct_read = 1u;

    /* Same opcode as a staged read; only the destination differs. The borrow is
     * carried so the server can map the destination for device DMA, and a server
     * that only copies ignores it. */
    wasmos_block_request_t req = {0};
    req.version = BLOCK_REQUEST_VERSION;
    req.target = blk->target;
    req.lba = lba;
    req.sector_count = count;
    req.dst_kind = BLOCK_DST_XFER_BUFFER;
    req.dst_buffer_id = buffer_id;
    req.dst_borrow_id = borrow_id;
    req.dst_offset = dst_offset;
    if (fat_block_submit(blk, BLOCK_IPC_READ_REQ, &req) != 0) {
        blk->cur_req_id = 0;
        blk->direct_read = 0u;
        return FAT_R_ERR;
    }
    blk->direct_pending = 1u;
    return FAT_R_WAIT;
}

fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba) {
    if (blk->loaded_lba == lba) {
        return FAT_R_DONE; /* cache hit */
    }
    if (fat_block_start(blk, lba, 0) != 0) {
        fat_block_set_err(blk, WASMOS_ERR_FS_IO);
        return FAT_R_ERR;
    }
    return FAT_R_WAIT;
}

fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba) {
    if (blk->write_pending) {
        blk->write_pending = 0; /* resumed: the write completed */
        return FAT_R_DONE;
    }
    if (fat_block_start(blk, lba, 1) != 0) {
        fat_block_set_err(blk, WASMOS_ERR_FS_IO);
        return FAT_R_ERR;
    }
    blk->write_pending = 1;
    return FAT_R_WAIT;
}

void fat_block_release(fat_block_t* blk, fat_op_ctx_t* ctx) {
    if (blk->owner == ctx) {
        blk->owner = 0;
        /* An op torn down mid-flight (a failed completion) must not leave the
         * yield-once latch armed, or the next op's first direct read would
         * report completion without ever submitting one. */
        blk->direct_pending = 0u;
        blk->direct_read = 0u;
    }
}

fat_op_ctx_t* fat_block_complete(fat_block_t* blk, int* out_ok) {
    fat_op_ctx_t* owner = blk->owner;
    int32_t rtype, rreq, rstatus;

    if (out_ok) {
        *out_ok = 0;
    }
    if (wasmos_ipc_select_one(blk->reply_endpoint) < 0) {
        return 0; /* spurious wake */
    }
    rtype = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    rreq = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    rstatus = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);

    if (blk->cur_req_id == 0 || rreq != blk->cur_req_id) {
        /* Consumed and dropped.  On this path that is a block reply nobody will
         * ever see again, and the operation waiting for it stalls the whole FS
         * chain behind it -- so it is reported rather than silently returned. */
        wasmos_ipc_message_t dropped;
        wasmos_ipc_message_read_last(&dropped);
        wasmos_sys_ipc_report_discard(
            "fs-fat/block", blk->reply_endpoint, blk->cur_req_id, &dropped);
        return 0; /* stale/unmatched reply */
    }
    blk->cur_req_id = 0;

    /* A direct read never touched the staged sector, so its outcome says nothing
     * about the cache either way — leave loaded_lba exactly as it was. */
    if (blk->direct_read) {
        int32_t rsectors = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        uint8_t ok = (rtype == blk->wait_resp_type && rstatus == 0 && rsectors > 0) ? 1u : 0u;
        blk->direct_sectors = ok ? (uint32_t)rsectors : 0u;
        blk->direct_read = 0u;
        if (out_ok) {
            *out_ok = ok;
        }
        return owner;
    }

    if (rtype == BLOCK_IPC_ERROR || rstatus != 0 || rtype != blk->wait_resp_type) {
        blk->loaded_lba = FAT_BLOCK_NO_LBA;
        return owner; /* out_ok stays 0 */
    }
    if (blk->copy_into_sector &&
        wasmos_block_buffer_copy(
            blk->buf_phys, addr_cast(int32_t, blk->sector), (int32_t)blk->sector_bytes, 0) != 0) {
        blk->loaded_lba = FAT_BLOCK_NO_LBA;
        return owner;
    }
    blk->loaded_lba = blk->wait_lba; /* sector now holds this lba (read or write) */
    if (out_ok) {
        *out_ok = 1;
    }
    return owner;
}
