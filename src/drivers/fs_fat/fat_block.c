/* fat_block.c - single-buffer block-I/O layer.  See fat_block.h.  State is
 * caller-owned (fat_block_t); no globals.  fs_fat always stages through
 * blk->sector; it does no PIO or DMA (that is ata's job).  All transfers are one
 * 512-byte block sector; the buffer is a 1-sector cache tagged by loaded_lba. */
#include "fat_block.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

void fat_block_configure(fat_block_t* blk, int32_t block_endpoint, int32_t reply_endpoint) {
    blk->block_endpoint = block_endpoint;
    blk->reply_endpoint = reply_endpoint;
    blk->buf_phys = -1;
    blk->next_req_id = 1;
    blk->cur_req_id = 0;
    blk->wait_lba = 0;
    blk->wait_resp_type = 0;
    blk->copy_into_sector = 0;
    blk->write_pending = 0;
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    blk->owner = 0;
}

int fat_block_setup(fat_block_t* blk) {
    blk->buf_phys = wasmos_block_buffer_phys();
    return blk->buf_phys < 0 ? -1 : 0;
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

/* Submit one 512-byte block transfer.  Returns 0 or -1 (send failure). */
static int fat_block_start(fat_block_t* blk, uint32_t lba, int rw) {
    int32_t req_type;

    if (blk->block_endpoint < 0 || blk->reply_endpoint < 0 || blk->buf_phys < 0) {
        return -1;
    }
    if (rw == 1 /* write */ &&
        wasmos_block_buffer_write(blk->buf_phys, addr_cast(int32_t, blk->sector),
                                  (int32_t)FAT_SECTOR_SIZE, 0) != 0) {
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

    if (wasmos_ipc_send(blk->block_endpoint, blk->reply_endpoint, req_type, blk->cur_req_id,
                        blk->buf_phys, (int32_t)lba, 1, 0) != 0) {
        blk->cur_req_id = 0;
        return -1;
    }
    return 0;
}

fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba) {
    if (blk->loaded_lba == lba) {
        return FAT_R_DONE; /* cache hit */
    }
    if (fat_block_start(blk, lba, 0) != 0) {
        fat_block_set_err(blk, -(int32_t)WASMOS_ERR_FS_IO);
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
        fat_block_set_err(blk, -(int32_t)WASMOS_ERR_FS_IO);
        return FAT_R_ERR;
    }
    blk->write_pending = 1;
    return FAT_R_WAIT;
}

void fat_block_release(fat_block_t* blk, fat_op_ctx_t* ctx) {
    if (blk->owner == ctx) {
        blk->owner = 0;
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
        return 0; /* stale/unmatched reply */
    }
    blk->cur_req_id = 0;

    if (rtype == BLOCK_IPC_ERROR || rstatus != 0 || rtype != blk->wait_resp_type) {
        blk->loaded_lba = FAT_BLOCK_NO_LBA;
        return owner; /* out_ok stays 0 */
    }
    if (blk->copy_into_sector &&
        wasmos_block_buffer_copy(blk->buf_phys, addr_cast(int32_t, blk->sector),
                                 (int32_t)FAT_SECTOR_SIZE, 0) != 0) {
        blk->loaded_lba = FAT_BLOCK_NO_LBA;
        return owner;
    }
    blk->loaded_lba = blk->wait_lba; /* sector now holds this lba (read or write) */
    if (out_ok) {
        *out_ok = 1;
    }
    return owner;
}
