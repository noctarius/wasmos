/* fat_block.h - block-I/O layer for the FAT reactor.
 *
 * fs_fat is a pure block CLIENT: it stages sectors in a dedicated buffer and
 * sends BLOCK_IPC_READ/WRITE requests.  Whether ata services them via PIO or DMA
 * is ata's decision and opaque here.  The buffer is used for METADATA (FAT-table
 * sectors, directory sectors, the boot sector) and the unaligned-edge bounce of
 * client I/O; bulk client data uses the zero-copy borrow passthrough (ata writes
 * the client buffer directly), not this buffer.
 *
 * The reactor serializes work to a single ACTIVE op, so at most one block
 * request is outstanding.  All of that singleton state — the outstanding
 * request, the staged sector and its cache tag, and the op to resume — lives in
 * the caller-owned fat_block_t (no module globals; per-op resume/cursor state
 * lives in fat_op_ctx_t).  The reactor sets blk->owner to the active op before
 * stepping it, so a completion knows whom to resume. */
#ifndef FS_FAT_FAT_BLOCK_H
#define FS_FAT_FAT_BLOCK_H

#include <stdint.h>
#include "fat_types.h"

#define FAT_BLOCK_NO_LBA 0xFFFFFFFFu /* loaded_lba sentinel: nothing staged */

typedef struct {
    int32_t block_endpoint;
    int32_t reply_endpoint;
    int32_t buf_phys; /* dedicated block buffer physical handle */
    int32_t next_req_id;

    /* Single outstanding request + staged sector (the buffer is a 1-sector
     * cache tagged by loaded_lba). */
    int32_t cur_req_id;       /* outstanding block request id, 0 if idle */
    uint32_t wait_lba;        /* lba of the outstanding request */
    int32_t wait_resp_type;   /* expected BLOCK_IPC_*_RESP */
    uint8_t copy_into_sector; /* pull phys->sector on completion (reads) */
    uint8_t write_pending;    /* a FAT_CO_WRITE is mid-flight (yield-once flag) */
    uint32_t loaded_lba;      /* lba currently staged, or FAT_BLOCK_NO_LBA */

    uint8_t sector[FAT_MAX_SECTOR_BYTES];

    fat_op_ctx_t* owner; /* active op to resume on completion */
} fat_block_t;

void fat_block_configure(fat_block_t* blk, int32_t block_endpoint, int32_t reply_endpoint);

/* Acquire the dedicated block-buffer physical handle.  Returns 0, or -1. */
int fat_block_setup(fat_block_t* blk);

uint8_t* fat_block_sector(fat_block_t* blk); /* the metadata/bounce buffer */
int fat_block_idle(const fat_block_t* blk);  /* 1 if no request is outstanding */

/* The reactor sets the op that owns the buffer for the current step (whom a
 * completion resumes) and records a failure code onto it (used by FAT_CO_FAIL). */
void fat_block_set_owner(fat_block_t* blk, fat_op_ctx_t* ctx);
void fat_block_set_err(fat_block_t* blk, int32_t err);

/* Op-facing, coroutine-driven I/O (return fat_r_t):
 *  fat_need_sector: ensure `lba` is staged in fat_block_sector(); FAT_R_DONE if
 *    already cached, FAT_R_WAIT if a read was submitted (resume on completion),
 *    FAT_R_ERR on a hard send failure (owner->err set to WASMOS_ERR_FS_IO).
 *  fat_block_write: push fat_block_sector() to `lba`; FAT_R_WAIT or FAT_R_ERR. */
fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba);
fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba);

/* Invalidate the sector cache (call when the buffer is repurposed). */
void fat_block_invalidate(fat_block_t* blk);

/* Release the buffer owned by ctx (op finished/aborted).  No-op otherwise. */
void fat_block_release(fat_block_t* blk, fat_op_ctx_t* ctx);

/* Drain one reply.  Returns the active op to resume, with *out_ok = 1 on success
 * or 0 on a block error.  Returns NULL for a spurious/unmatched reply. */
fat_op_ctx_t* fat_block_complete(fat_block_t* blk, int* out_ok);

#endif /* FS_FAT_FAT_BLOCK_H */
