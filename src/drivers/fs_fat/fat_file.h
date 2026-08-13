/* fat_file.h - POSIX fd-operation layer for the FAT reactor.
 *
 * Implements open/read/write/stat/unlink/mkdir/rmdir/seek/close on the fat_co.h
 * coroutine pattern. Each fd op is a resumable step the reactor dispatches on an
 * fat_op_ctx_t; the open-file table is an fat_open_pool_t the reactor owns and
 * passes in by pointer.
 *
 * Reads and writes address the CLIENT transfer buffer by id (op->arg2) through
 * the xfer-buffer read/write calls.  A read additionally takes the zero-copy
 * path for whole sectors: it reborrows that buffer to the block server, which
 * lands the sectors there directly (see fat_block_read_direct).  Partial sectors
 * and all writes stage through the block buffer.
 *
 * Error reporting: the op steps here fail with a granular WASMOS_ERR_FS_* via
 * FAT_CO_FAIL rather than a blanket -1. The one gap is a zero-copy submit
 * failure, which fat_block_read_direct reports without setting op->err (see the
 * FIXME there). The open-file table helpers below (alloc/access_mode/set_offset)
 * return -1 on bad arguments; those values stay inside the driver and never
 * become a reply. */
#ifndef FS_FAT_FAT_FILE_H
#define FS_FAT_FAT_FILE_H

#include <stdint.h>
#include "fat_block.h"
#include "fat_geom.h"
#include "fat_types.h"

/* The open-file table.  The reactor holds the single instance and passes it to
 * every fd op; a slot belongs to the endpoint that opened it.  fd = index + 3. */
typedef struct {
    fat_open_file_t files[FAT_MAX_OPEN_FILES];
} fat_open_pool_t;

/* Reset every slot to the free/idle state (call once at driver init). */
void fat_open_pool_init(fat_open_pool_t* pool);

/* --- Pure / simple open-file-table helpers (no I/O, no coroutine). --- */

/* The open file for `fd` owned by `source`, or NULL if the fd is out of range,
 * free, or owned by another endpoint.  fd = index + 3. */
fat_open_file_t* fat_open_file_for_fd(fat_open_pool_t* pool, int32_t source, int32_t fd);

/* Allocate a free slot to `source`, zeroing its cursors; *out_fd = index + 3.
 * 0 on success, -1 if the table is full. */
int fat_open_file_alloc(fat_open_pool_t* pool, int32_t source, int32_t* out_fd);

/* 0 = read access, 1 = write access, -1 on NULL (flags bit 0). */
int fat_open_file_access_mode(const fat_open_file_t* file);

/* Set file->offset to `offset` and recompute the current cluster/sector/lba,
 * WITHOUT following the chain: only valid for offset==0 or an offset that stays
 * within the first cluster (the caller uses fat_reposition_open_file for the
 * general case).  Returns -1 if a chain walk would be required. */
int fat_set_open_file_offset(const fat_mount_t* mnt, fat_open_file_t* file, uint32_t offset,
                             uint32_t limit);

/* --- Coroutine helpers (I/O bearing; contexts in fat_types.h). --- */

/* RMW the file's directory entry: patch the 4-byte size (bytes 28..31); on
 * FAT_R_DONE file->size == c->size.  Inputs on `c`: file, size. */
fat_r_t fat_store_open_file_size(fat_storesize_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt);

/* RMW the file's directory entry: patch the 2-byte first cluster (bytes 26..27);
 * on FAT_R_DONE file->first_cluster == c->cluster.  Inputs: file, cluster. */
fat_r_t fat_store_open_file_cluster(fat_storecluster_ctx_t* c, fat_block_t* blk,
                                    const fat_mount_t* mnt);

/* Reposition to an absolute offset, following the cluster chain as needed.
 * Inputs on `c`: file, offset, limit. */
fat_r_t fat_reposition_open_file(fat_reposition_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt);

/* Append one freshly-allocated cluster to the file's chain and bump its
 * capacity.  Inputs on `c`: file. */
fat_r_t fat_append_cluster_to_file(fat_append_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt);

/* Grow capacity to >= min_size (append clusters, then reposition to the saved
 * offset).  Inputs on `c`: file, min_size. */
fat_r_t fat_ensure_open_file_capacity(fat_ensurecap_ctx_t* c, fat_block_t* blk,
                                      const fat_mount_t* mnt);

/* --- The fd OP step coroutines the reactor dispatches (one per op). --- */

fat_r_t fat_op_open(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool);
fat_r_t fat_op_read(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool);
fat_r_t fat_op_write(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool);
fat_r_t fat_op_stat(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool);
fat_r_t fat_op_unlink(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                      fat_open_pool_t* pool);
fat_r_t fat_op_mkdir(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool);
fat_r_t fat_op_rmdir(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool);
fat_r_t fat_op_seek(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool);
fat_r_t fat_op_close(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool);

#endif /* FS_FAT_FAT_FILE_H */
