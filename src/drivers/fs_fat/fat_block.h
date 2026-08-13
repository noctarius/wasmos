/* fat_block.h - block-I/O layer for the FAT reactor.
 *
 * fs_fat is a pure block CLIENT: it stages sectors in a dedicated buffer and
 * sends BLOCK_IPC_READ/WRITE requests.  Whether the block server services them
 * via PIO or DMA is its decision and opaque here.  The buffer holds METADATA
 * (FAT-table sectors, directory sectors, the boot sector) and bounces client
 * I/O.  Only the READ path can bypass it: whole sectors of a client read go
 * through the zero-copy borrow passthrough (fat_block_read_direct, the server
 * writes the client buffer itself).  Client writes and partial-sector reads
 * always stage here.
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
    uint8_t direct_read;      /* outstanding request landed in the client buffer */
    uint8_t direct_pending;   /* a FAT_CO_READ_DIRECT is mid-flight (yield-once flag) */
    uint32_t direct_sectors;  /* sectors the server reported for the last direct read */
    uint8_t write_pending;    /* a FAT_CO_WRITE is mid-flight (yield-once flag) */
    uint32_t loaded_lba;      /* lba currently staged, or FAT_BLOCK_NO_LBA */

    uint8_t sector[FAT_MAX_SECTOR_BYTES];

    fat_op_ctx_t* owner; /* active op to resume on completion */
} fat_block_t;

void fat_block_configure(fat_block_t* blk, int32_t block_endpoint, int32_t reply_endpoint);

/* Acquire the dedicated block-buffer physical handle.  Returns 0, or -1. */
int fat_block_setup(fat_block_t* blk);

uint8_t* fat_block_sector(fat_block_t* blk); /* the metadata/bounce buffer */
/* Endpoint of the block server, needed to reborrow a client buffer to it. */
int32_t fat_block_server_endpoint(const fat_block_t* blk);
int fat_block_idle(const fat_block_t* blk); /* 1 if no request is outstanding */

/* The reactor sets the op that owns the buffer for the current step (whom a
 * completion resumes) and records a failure code onto it (used by FAT_CO_FAIL). */
void fat_block_set_owner(fat_block_t* blk, fat_op_ctx_t* ctx);
void fat_block_set_err(fat_block_t* blk, int32_t err);

/* Op-facing, coroutine-driven I/O (return fat_r_t):
 *  fat_need_sector: ensure `lba` is staged in fat_block_sector(); FAT_R_DONE if
 *    already cached, FAT_R_WAIT if a read was submitted (resume on completion),
 *    FAT_R_ERR on a hard send failure (owner->err set to WASMOS_ERR_FS_IO).
 *  fat_block_write: push fat_block_sector() to `lba`; FAT_R_WAIT on submit,
 *    FAT_R_ERR on a hard send failure (owner->err set to WASMOS_ERR_FS_IO), and
 *    FAT_R_DONE on the resume call the coroutine macro makes after completion. */
fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba);
fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba);

/* Submit a zero-copy read of `count` whole sectors from `lba` straight into the
 * client's transfer buffer at `dst_offset`. Nothing is staged here, so the
 * sector cache is untouched and stays valid. The caller must already have
 * reborrowed the buffer to the block server, and passes that `borrow_id` so a
 * bus-master server can map the destination instead of copying into it.
 * FAT_R_WAIT on submit, FAT_R_DONE on the resume call after completion, and
 * FAT_R_ERR on bad arguments or a hard send failure — unlike fat_need_sector /
 * fat_block_write this leaves owner->err untouched, so the caller must set it. */
fat_r_t fat_block_read_direct(fat_block_t* blk, uint32_t lba, uint32_t count, int32_t buffer_id,
                              int32_t borrow_id, uint32_t dst_offset);

/* Sectors the server actually transferred for the direct read that just
 * completed. It may be fewer than asked for, so the caller advances by this
 * rather than by what it requested. */
uint32_t fat_block_direct_sectors(const fat_block_t* blk);

/* Invalidate the sector cache (call when the buffer is repurposed). */
void fat_block_invalidate(fat_block_t* blk);

/* Release the buffer owned by ctx (op finished/aborted).  No-op otherwise. */
void fat_block_release(fat_block_t* blk, fat_op_ctx_t* ctx);

/* Drain one reply.  Returns the active op to resume, with *out_ok = 1 on success
 * or 0 on a block error.  Returns NULL for a spurious/unmatched reply. */
fat_op_ctx_t* fat_block_complete(fat_block_t* blk, int* out_ok);

#endif /* FS_FAT_FAT_BLOCK_H */
