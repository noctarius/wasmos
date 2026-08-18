/* fat_file.c - POSIX fd-operation layer for the FAT reactor.  See fat_file.h.
 *
 * Every op is a fat_co.h coroutine on the caller's fat_op_ctx_t: loop cursors
 * live in the context, and C locals carry no initializers because the resume
 * switch jumps past their declarations.  Anything that walks the cluster chain
 * is a coroutine because it yields per FAT sector; the offset/geometry math that
 * needs no I/O stays a pure helper. */
#include "fat_file.h"
#include "fat_co.h"
#include "fat_alloc.h"
#include "fat_dir.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"

/* ------------------------------------------------------------------------- */
/* Open-file table (pure / simple helpers).                                  */
/* ------------------------------------------------------------------------- */

void fat_open_pool_init(fat_open_pool_t* pool) {
    uint32_t i;

    if (!pool) {
        return;
    }
    for (i = 0; i < FAT_MAX_OPEN_FILES; ++i) {
        pool->files[i].in_use = 0;
        pool->files[i].owner = -1;
        pool->files[i].flags = 0;
        pool->files[i].first_cluster = 0;
        pool->files[i].current_cluster = 0;
        pool->files[i].current_sector = 0;
        pool->files[i].file_lba = 0;
        pool->files[i].size = 0;
        pool->files[i].capacity = 0;
        pool->files[i].offset = 0;
        pool->files[i].dir_lba = 0;
        pool->files[i].dir_sector = 0;
        pool->files[i].dir_index = 0;
    }
}

fat_open_file_t* fat_open_file_for_fd(fat_open_pool_t* pool, int32_t source, int32_t fd) {
    int32_t index = fd - 3;

    if (!pool || index < 0 || (uint32_t)index >= FAT_MAX_OPEN_FILES) {
        return 0;
    }
    if (!pool->files[index].in_use || pool->files[index].owner != source) {
        return 0;
    }
    return &pool->files[index];
}

int fat_open_file_alloc(fat_open_pool_t* pool, int32_t source, int32_t* out_fd) {
    uint32_t i;

    if (!pool || !out_fd) {
        return -1;
    }
    for (i = 0; i < FAT_MAX_OPEN_FILES; ++i) {
        if (!pool->files[i].in_use) {
            pool->files[i].in_use = 1;
            pool->files[i].owner = source;
            pool->files[i].flags = 0;
            pool->files[i].first_cluster = 0;
            pool->files[i].current_cluster = 0;
            pool->files[i].current_sector = 0;
            pool->files[i].file_lba = 0;
            pool->files[i].size = 0;
            pool->files[i].capacity = 0;
            pool->files[i].offset = 0;
            pool->files[i].dir_lba = 0;
            pool->files[i].dir_sector = 0;
            pool->files[i].dir_index = 0;
            *out_fd = (int32_t)i + 3;
            return 0;
        }
    }
    return -1;
}

int fat_open_file_access_mode(const fat_open_file_t* file) {
    if (!file) {
        return -1;
    }
    return file->flags & 1;
}

/* Set the offset WITHOUT following the chain.  Handles offset==0 and offsets
 * that stay within the first cluster; returns -1 (chain walk required) when the
 * target lies in a later cluster (fat_reposition_open_file handles that). */
int fat_set_open_file_offset(const fat_mount_t* mnt, fat_open_file_t* file, uint32_t offset,
                             uint32_t limit) {
    uint32_t cluster_bytes;
    uint32_t cluster_skip;
    uint32_t cluster_offset;
    uint32_t at_limit_boundary = 0;

    if (!mnt || !file || offset > limit) {
        return -1;
    }

    file->offset = offset;
    if (offset == 0) {
        file->current_cluster = file->first_cluster;
        file->current_sector = 0;
        file->file_lba =
            file->first_cluster >= 2 ? fat_lba_for_cluster(mnt, file->first_cluster) : 0;
        return 0;
    }
    if (file->first_cluster < 2 || mnt->sectors_per_cluster == 0 || mnt->bytes_per_sector == 0) {
        return -1;
    }

    cluster_bytes = (uint32_t)mnt->sectors_per_cluster * mnt->bytes_per_sector;
    cluster_skip = offset / cluster_bytes;
    cluster_offset = offset % cluster_bytes;
    at_limit_boundary = offset > 0 && offset == limit && cluster_offset == 0;
    if (at_limit_boundary) {
        cluster_skip--;
    }
    if (cluster_skip > 0) {
        /* Would need to walk the chain: not handled by the pure helper. */
        return -1;
    }
    file->current_cluster = file->first_cluster;
    file->current_sector = cluster_offset / mnt->bytes_per_sector;
    if (at_limit_boundary) {
        file->current_sector = mnt->sectors_per_cluster - 1u;
    }
    file->file_lba = fat_lba_for_cluster(mnt, file->current_cluster);
    return file->file_lba == 0 ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Coroutine helpers (I/O bearing).                                          */
/* ------------------------------------------------------------------------- */

fat_r_t fat_store_open_file_size(fat_storesize_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt) {
    uint8_t* ent;

    FAT_CO_BEGIN(c);
    if (!c->file || c->file->dir_lba == 0 || c->file->dir_index >= (mnt->bytes_per_sector / 32u)) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_CORRUPT);
    }
    FAT_CO_READ(c, blk, c->file->dir_lba + c->file->dir_sector);
    ent = fat_block_sector(blk) + c->file->dir_index * 32u;
    ent[28] = (uint8_t)(c->size & 0xFFu);
    ent[29] = (uint8_t)((c->size >> 8) & 0xFFu);
    ent[30] = (uint8_t)((c->size >> 16) & 0xFFu);
    ent[31] = (uint8_t)((c->size >> 24) & 0xFFu);
    FAT_CO_WRITE(c, blk, c->file->dir_lba + c->file->dir_sector);
    c->file->size = c->size;
    FAT_CO_END(c);
}

fat_r_t fat_store_open_file_cluster(fat_storecluster_ctx_t* c, fat_block_t* blk,
                                    const fat_mount_t* mnt) {
    uint8_t* ent;

    FAT_CO_BEGIN(c);
    if (!c->file || c->file->dir_lba == 0 || c->file->dir_index >= (mnt->bytes_per_sector / 32u)) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_CORRUPT);
    }
    FAT_CO_READ(c, blk, c->file->dir_lba + c->file->dir_sector);
    ent = fat_block_sector(blk) + c->file->dir_index * 32u;
    fat_dirent_set_cluster(ent, c->cluster);
    FAT_CO_WRITE(c, blk, c->file->dir_lba + c->file->dir_sector);
    c->file->first_cluster = c->cluster;
    FAT_CO_END(c);
}

fat_r_t fat_reposition_open_file(fat_reposition_ctx_t* c, fat_block_t* blk,
                                 const fat_mount_t* mnt) {
    uint32_t cluster_bytes;
    uint32_t cluster_offset;
    uint32_t at_limit_boundary;

    FAT_CO_BEGIN(c);
    if (!c->file || c->offset > c->limit) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_RANGE);
    }
    c->file->offset = c->offset;
    if (c->offset == 0) {
        c->file->current_cluster = c->file->first_cluster;
        c->file->current_sector = 0;
        c->file->file_lba =
            c->file->first_cluster >= 2 ? fat_lba_for_cluster(mnt, c->file->first_cluster) : 0;
        FAT_CO_DONE(c);
    }
    if (c->file->first_cluster < 2 || mnt->sectors_per_cluster == 0 || mnt->bytes_per_sector == 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_RANGE);
    }

    cluster_bytes = (uint32_t)mnt->sectors_per_cluster * mnt->bytes_per_sector;
    c->cluster_skip = c->offset / cluster_bytes;
    cluster_offset = c->offset % cluster_bytes;
    at_limit_boundary = c->offset > 0 && c->offset == c->limit && cluster_offset == 0;
    if (at_limit_boundary) {
        c->cluster_skip--;
    }
    c->file->current_cluster = c->file->first_cluster;
    c->file->current_sector = cluster_offset / mnt->bytes_per_sector;
    if (at_limit_boundary) {
        c->file->current_sector = mnt->sectors_per_cluster - 1u;
    }

    while (c->cluster_skip > 0) {
        c->step.cont = 0;
        c->step.cluster = c->file->current_cluster;
        FAT_CO_AWAIT(c, fat_chain_next(&c->step, blk, mnt));
        if (c->step.next == 0) {
            FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_RANGE);
        }
        c->file->current_cluster = c->step.next;
        c->cluster_skip--;
    }

    c->file->file_lba = fat_lba_for_cluster(mnt, c->file->current_cluster);
    if (c->file->file_lba == 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_RANGE);
    }
    FAT_CO_END(c);
}

fat_r_t fat_append_cluster_to_file(fat_append_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t cluster_bytes = (uint32_t)mnt->sectors_per_cluster * mnt->bytes_per_sector;

    FAT_CO_BEGIN(c);
    if (!c->file) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    c->end_marker = fat_end_of_chain_marker(mnt);
    if (c->end_marker == 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_CORRUPT);
    }

    c->findfree.cont = 0;
    FAT_CO_AWAIT(c, fat_find_free_cluster(&c->findfree, blk, mnt));
    c->new_cluster = c->findfree.result;

    c->fatent.cont = 0;
    c->fatent.cluster = c->new_cluster;
    c->fatent.write_value = c->end_marker;
    FAT_CO_AWAIT(c, fat_fatent_write(&c->fatent, blk, mnt));

    if (c->file->first_cluster < 2) {
        c->store.cont = 0;
        c->store.file = c->file;
        c->store.cluster = c->new_cluster;
        FAT_CO_AWAIT(c, fat_store_open_file_cluster(&c->store, blk, mnt));
    } else {
        c->walk.cont = 0;
        c->walk.cluster = c->file->first_cluster;
        FAT_CO_AWAIT(c, fat_chain_walk(&c->walk, blk, mnt));
        c->last_cluster = c->walk.last;
        c->fatent.cont = 0;
        c->fatent.cluster = c->last_cluster;
        c->fatent.write_value = c->new_cluster;
        FAT_CO_AWAIT(c, fat_fatent_write(&c->fatent, blk, mnt));
    }

    /* Refuse rather than wrap: a wrapped capacity reads as "smaller than the
     * offset", which sends fat_ensure_open_file_capacity back round to append
     * again -- an allocation loop that consumes the volume. */
    if (c->file->capacity > 0xFFFFFFFFu - cluster_bytes) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_RANGE);
    }
    c->file->capacity += cluster_bytes;
    FAT_CO_END(c);
}

fat_r_t fat_ensure_open_file_capacity(fat_ensurecap_ctx_t* c, fat_block_t* blk,
                                      const fat_mount_t* mnt) {
    FAT_CO_BEGIN(c);
    if (!c->file) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    if (c->min_size == 0 || c->file->capacity >= c->min_size) {
        FAT_CO_DONE(c);
    }
    c->saved_offset = c->file->offset;
    while (c->file->capacity < c->min_size) {
        c->prev_capacity = c->file->capacity;
        c->append.cont = 0;
        c->append.file = c->file;
        FAT_CO_AWAIT(c, fat_append_cluster_to_file(&c->append, blk, mnt));
        /* Every append must advance the capacity. If one does not, looping
         * would allocate clusters forever without ever satisfying min_size. */
        if (c->file->capacity <= c->prev_capacity) {
            FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_RANGE);
        }
    }
    c->repos.cont = 0;
    c->repos.file = c->file;
    c->repos.offset = c->saved_offset;
    c->repos.limit = c->file->capacity;
    FAT_CO_AWAIT(c, fat_reposition_open_file(&c->repos, blk, mnt));
    FAT_CO_END(c);
}

/* ------------------------------------------------------------------------- */
/* fd OP step coroutines.                                                     */
/* ------------------------------------------------------------------------- */

/* Shared path-read prologue used by the path-taking ops: validate arg1/len,
 * read the client path into op->path, translate into op->fat_path.  Returns an
 * WASMOS_ERR_FS_* (<0) on failure, 0 on success.  Pure (no yields), so it is a plain
 * helper the op invokes on its first step. */
static int fat_read_and_translate_path(fat_op_ctx_t* op, uint8_t* out_is_init) {
    uint32_t path_len = (uint32_t)op->arg0;

    if (op->arg1 != 0 || path_len == 0 || path_len >= sizeof(op->path)) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (path_len + 1u > (uint32_t)wasmos_xfer_buffer_size()) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    if (wasmos_sys_buffer_read(op->arg2, op->path, (int32_t)path_len, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    op->path[path_len] = '\0';
    if (vfs_translate_path(op->path, op->fat_path, sizeof(op->fat_path), out_is_init) != 0 ||
        *out_is_init) {
        return WASMOS_ERR_FS_TRANSLATE;
    }
    return 0;
}

fat_r_t fat_op_open(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool) {
    uint32_t path_len;
    int32_t access_mode;
    uint8_t path_is_init;
    fat_open_file_t* file;

    FAT_CO_BEGIN(op);
    path_len = (uint32_t)op->arg0;
    access_mode = op->arg1 & 1;
    path_is_init = 0;

    if ((op->arg1 & ~((int32_t)FAT_OPEN_APPEND | (int32_t)FAT_OPEN_CREAT | (int32_t)FAT_OPEN_TRUNC |
                      1)) != 0 ||
        path_len == 0 || path_len >= sizeof(op->path)) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    if ((access_mode != 0 && access_mode != 1) ||
        ((op->arg1 & (FAT_OPEN_APPEND | FAT_OPEN_TRUNC)) != 0 && access_mode != 1)) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    if (path_len + 1u > (uint32_t)wasmos_xfer_buffer_size()) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_PATH_TOO_LONG);
    }
    if (wasmos_sys_buffer_read(op->arg2, op->path, (int32_t)path_len, 0) != 0) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BUFFER);
    }
    op->path[path_len] = '\0';
    if (vfs_translate_path(op->path, op->fat_path, sizeof(op->fat_path), &path_is_init) != 0 ||
        path_is_init) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_TRANSLATE);
    }

    op->resolve.cont = 0;
    op->resolve.path = op->fat_path;
    op->resolve.source = op->source;
    FAT_CO_AWAIT(op, fat_resolve_path(&op->resolve, blk, mnt));
    op->open_entry = op->resolve.found;

    if (!op->open_entry.valid) {
        if ((op->arg1 & FAT_OPEN_CREAT) == 0) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NOT_FOUND);
        }
        op->create.cont = 0;
        op->create.path = op->fat_path;
        op->create.source = op->source;
        FAT_CO_AWAIT(op, fat_create_empty_file(&op->create, blk, mnt));
        if (!op->create.found.valid) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_IO);
        }
        op->open_entry = op->create.found;
    }
    if (op->open_entry.attr & 0x10) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_IS_DIR);
    }
    if (op->open_entry.size > 0 && op->open_entry.cluster < 2) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_CORRUPT);
    }

    if (fat_open_file_alloc(pool, op->source, &op->fd) != 0) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NO_FD);
    }
    file = fat_open_file_for_fd(pool, op->source, op->fd);
    if (!file) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NO_FD);
    }
    file->flags = op->arg1;
    file->first_cluster = op->open_entry.cluster;
    file->current_cluster = op->open_entry.cluster;
    file->current_sector = 0;
    file->file_lba = fat_lba_for_cluster(mnt, op->open_entry.cluster);
    file->size = op->open_entry.size;
    file->offset = 0;
    file->dir_lba = op->open_entry.dir_lba;
    file->dir_sector = op->open_entry.dir_sector;
    file->dir_index = op->open_entry.dir_index;

    /* Capacity = allocated chain length * cluster bytes, which is what the write
     * path grows; it is independent of the entry's size field. */
    if (op->open_entry.cluster >= 2 && mnt->sectors_per_cluster != 0 &&
        mnt->bytes_per_sector != 0) {
        op->capwalk.cont = 0;
        op->capwalk.cluster = op->open_entry.cluster;
        FAT_CO_AWAIT(op, fat_chain_walk(&op->capwalk, blk, mnt));
        /* `file` is a C local (not preserved across the yield above); re-derive
         * it from the fd stored in the op ctx. */
        file = fat_open_file_for_fd(pool, op->source, op->fd);
        if (!file) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NO_FD);
        }
        file->capacity =
            op->capwalk.hops * (uint32_t)mnt->sectors_per_cluster * mnt->bytes_per_sector;
    } else {
        file->capacity = 0;
    }

    if ((op->arg1 & FAT_OPEN_TRUNC) != 0) {
        file = fat_open_file_for_fd(pool, op->source, op->fd);
        if (!file) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NO_FD);
        }
        op->storesize.cont = 0;
        op->storesize.file = file;
        op->storesize.size = 0;
        FAT_CO_AWAIT(op, fat_store_open_file_size(&op->storesize, blk, mnt));
        file = fat_open_file_for_fd(pool, op->source, op->fd);
        if (!file) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NO_FD);
        }
        if (fat_set_open_file_offset(mnt, file, 0, file->capacity) != 0) {
            file->in_use = 0;
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_IO);
        }
    } else if ((op->arg1 & FAT_OPEN_APPEND) != 0) {
        file = fat_open_file_for_fd(pool, op->source, op->fd);
        if (!file) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NO_FD);
        }
        op->repos.cont = 0;
        op->repos.file = file;
        op->repos.offset = file->size;
        op->repos.limit = file->capacity;
        FAT_CO_AWAIT(op, fat_reposition_open_file(&op->repos, blk, mnt));
    }

    op->resp_override = 1;
    op->resp_arg0 = op->fd;
    FAT_CO_END(op);
}

/* Lazily reborrow the client's transfer buffer to the block server so it may
 * write there itself. Taken on first use rather than at op start, so a read made
 * entirely of partial sectors never creates a grant at all; dropped in
 * fat_op_free. Returns 1 when the direct path is usable.
 *
 * Failure is never an error — it just means this read stages through the block
 * buffer. op->arg3 is the borrow fs-manager reborrowed to this backend; a client
 * that talks to the backend directly has none to re-lend, which is the common
 * case during boot. */
static int fat_read_direct_arm(fat_op_ctx_t* op, fat_block_t* blk) {
    int32_t borrow;
    if (op->zc_borrow > 0) {
        return 1;
    }
    if (op->zc_borrow < 0 || op->arg3 <= 0) {
        return 0; /* already tried and failed, or nothing to re-lend */
    }
    borrow = wasmos_xfer_buffer_reborrow(
        fat_block_server_endpoint(blk), op->arg3, WASMOS_BUFFER_GRANT_WRITE);
    if (borrow <= 0) {
        op->zc_borrow = -1; /* remember, so every sector does not retry */
        return 0;
    }
    op->zc_borrow = borrow;
    return 1;
}

/* Whole sectors one direct request may cover. Three bounds: what is left of the
 * request, the tail of the current cluster (the next cluster can be anywhere on
 * disk, so a run may not cross the boundary), and what one block request carries.
 * Batching is what makes the direct path worth its per-op grant — the IPC round
 * trip dominates a single 512-byte transfer. */
static uint32_t fat_read_direct_run(const fat_op_ctx_t* op, const fat_open_file_t* file,
                                    const fat_mount_t* mnt) {
    uint32_t run = (op->requested - op->done) / mnt->bytes_per_sector;
    uint32_t to_cluster_end = mnt->sectors_per_cluster - file->current_sector;
    if (run > to_cluster_end) {
        run = to_cluster_end;
    }
    if (run > WASMOS_BLOCK_ZC_MAX_SECTORS) {
        run = WASMOS_BLOCK_ZC_MAX_SECTORS;
    }
    return run;
}

fat_r_t fat_op_read(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool) {
    /* `file` points at a stable pool slot, but the C local is re-derived every
     * step (it is not preserved across a coroutine yield); resolve it before the
     * resume switch so it is valid on every entry. */
    fat_open_file_t* file = fat_open_file_for_fd(pool, op->source, op->arg0);
    uint32_t max_buffer;
    uint32_t remaining;

    FAT_CO_BEGIN(op);
    max_buffer = (uint32_t)wasmos_xfer_buffer_size();

    if (!file || fat_open_file_access_mode(file) != 0 || op->arg1 < 0) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_ACCESS);
    }

    remaining = file->offset < file->size ? file->size - file->offset : 0;
    op->requested = (uint32_t)op->arg1;
    if (op->requested > max_buffer) {
        op->requested = max_buffer;
    }
    if (op->requested > remaining) {
        op->requested = remaining;
    }
    op->done = 0;

    while (op->done < op->requested) {
        op->io_sector_offset = file->offset % mnt->bytes_per_sector;
        op->io_chunk = mnt->bytes_per_sector - op->io_sector_offset;
        if (op->io_chunk > op->requested - op->done) {
            op->io_chunk = op->requested - op->done;
        }
        if (file->current_cluster < 2 || file->file_lba == 0) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_CORRUPT);
        }
        /* A whole sector can be landed straight in the client's buffer. A
         * partial one cannot: the block server transfers 512 bytes at a time, so
         * writing it at the client's offset would clobber the bytes on either
         * side of the range actually asked for. Those edges keep bouncing
         * through the staged sector. */
        op->io_run_sectors = 1u;
        if (op->io_sector_offset == 0u && op->io_chunk == mnt->bytes_per_sector &&
            fat_read_direct_arm(op, blk)) {
            op->io_run_sectors = fat_read_direct_run(op, file, mnt);
            if (op->io_run_sectors == 0u) {
                op->io_run_sectors = 1u;
            }
            FAT_CO_READ_DIRECT(op,
                               blk,
                               file->file_lba + file->current_sector,
                               op->io_run_sectors,
                               op->arg2,
                               op->zc_borrow,
                               op->done);
            /* The server may have transferred fewer than asked; advance by what
             * it reported and let the loop issue the remainder. */
            op->io_run_sectors = fat_block_direct_sectors(blk);
            if (op->io_run_sectors == 0u) {
                FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_IO);
            }
            op->io_chunk = op->io_run_sectors * mnt->bytes_per_sector;
        } else {
            FAT_CO_READ(op, blk, file->file_lba + file->current_sector);
            if (wasmos_xfer_buffer_write(op->arg2,
                                         fat_block_sector(blk) + op->io_sector_offset,
                                         (int32_t)op->io_chunk,
                                         (int32_t)op->done) != 0) {
                FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BUFFER);
            }
        }
        file->offset += op->io_chunk;
        op->done += op->io_chunk;

        if (file->offset >= file->size ||
            op->io_sector_offset + op->io_chunk < mnt->bytes_per_sector) {
            continue;
        }
        file->current_sector += op->io_run_sectors;
        if (file->current_sector < mnt->sectors_per_cluster) {
            continue;
        }
        op->chain.cont = 0;
        op->chain.cluster = file->current_cluster;
        FAT_CO_AWAIT(op, fat_chain_next(&op->chain, blk, mnt));
        if (op->chain.next == 0) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_CORRUPT);
        }
        file->current_cluster = op->chain.next;
        file->current_sector = 0;
        file->file_lba = fat_lba_for_cluster(mnt, op->chain.next);
    }

    op->resp_override = 1;
    op->resp_arg0 = (int32_t)op->done;
    FAT_CO_END(op);
}

fat_r_t fat_op_write(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool) {
    /* Re-derived every step (see fat_op_read): a stable slot, an unstable local. */
    fat_open_file_t* file = fat_open_file_for_fd(pool, op->source, op->arg0);
    uint32_t max_buffer;
    uint32_t next_end;
    uint32_t i;
    uint8_t stage[FAT_MAX_SECTOR_BYTES];

    FAT_CO_BEGIN(op);
    max_buffer = (uint32_t)wasmos_xfer_buffer_size();

    if (!file || fat_open_file_access_mode(file) != 1 || op->arg1 < 0) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_ACCESS);
    }
    if ((file->flags & FAT_OPEN_APPEND) != 0) {
        op->repos.cont = 0;
        op->repos.file = file;
        op->repos.offset = file->size;
        op->repos.limit = file->capacity;
        FAT_CO_AWAIT(op, fat_reposition_open_file(&op->repos, blk, mnt));
    }
    op->requested = (uint32_t)op->arg1;
    if (op->requested > max_buffer) {
        op->requested = max_buffer;
    }
    if (op->requested == 0) {
        op->resp_override = 1;
        op->resp_arg0 = 0;
        FAT_CO_DONE(op);
    }
    op->target_end = file->offset + op->requested;
    if (op->target_end < file->offset) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_RANGE);
    }
    op->ensurecap.cont = 0;
    op->ensurecap.file = file;
    op->ensurecap.min_size = op->target_end;
    FAT_CO_AWAIT(op, fat_ensure_open_file_capacity(&op->ensurecap, blk, mnt));

    op->done = 0;
    while (op->done < op->requested) {
        op->io_sector_offset = file->offset % mnt->bytes_per_sector;
        op->io_chunk = mnt->bytes_per_sector - op->io_sector_offset;
        if (op->io_chunk > op->requested - op->done) {
            op->io_chunk = op->requested - op->done;
        }
        next_end = file->offset + op->io_chunk;
        op->old_size = file->size;
        if (file->current_cluster < 2 || file->file_lba == 0) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_CORRUPT);
        }
        if (next_end > file->size) {
            /* Grow the visible file size before writing past the current EOF. */
            op->storesize.cont = 0;
            op->storesize.file = file;
            op->storesize.size = next_end;
            FAT_CO_AWAIT(op, fat_store_open_file_size(&op->storesize, blk, mnt));
        }
        if (op->io_sector_offset != 0 || op->io_chunk != mnt->bytes_per_sector) {
            if (op->old_size <= file->offset && op->io_sector_offset == 0) {
                for (i = 0; i < mnt->bytes_per_sector; ++i) {
                    fat_block_sector(blk)[i] = 0;
                }
                fat_block_invalidate(blk);
            } else {
                FAT_CO_READ(op, blk, file->file_lba + file->current_sector);
            }
        }
        /* Stage the client chunk, then merge it into the sector buffer.  The
         * xfer read + copy happen within this step (no yield between), so the
         * stack buffer is safe. */
        if (wasmos_xfer_buffer_read(op->arg2, stage, (int32_t)op->io_chunk, (int32_t)op->done) !=
            0) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BUFFER);
        }
        for (i = 0; i < op->io_chunk; ++i) {
            fat_block_sector(blk)[op->io_sector_offset + i] = stage[i];
        }
        FAT_CO_WRITE(op, blk, file->file_lba + file->current_sector);
        file->offset += op->io_chunk;
        op->done += op->io_chunk;

        if (file->offset >= file->capacity ||
            op->io_sector_offset + op->io_chunk < mnt->bytes_per_sector) {
            continue;
        }
        file->current_sector++;
        if (file->current_sector < mnt->sectors_per_cluster) {
            continue;
        }
        op->chain.cont = 0;
        op->chain.cluster = file->current_cluster;
        FAT_CO_AWAIT(op, fat_chain_next(&op->chain, blk, mnt));
        if (op->chain.next == 0) {
            FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_CORRUPT);
        }
        file->current_cluster = op->chain.next;
        file->current_sector = 0;
        file->file_lba = fat_lba_for_cluster(mnt, op->chain.next);
    }

    op->resp_override = 1;
    op->resp_arg0 = (int32_t)op->done;
    FAT_CO_END(op);
}

fat_r_t fat_op_stat(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool) {
    uint8_t path_is_init;
    int rc;

    (void)pool;
    FAT_CO_BEGIN(op);
    path_is_init = 0;
    rc = fat_read_and_translate_path(op, &path_is_init);
    if (rc != 0) {
        FAT_CO_FAIL(op, blk, rc);
    }
    op->resolve.cont = 0;
    op->resolve.path = op->fat_path;
    op->resolve.source = op->source;
    FAT_CO_AWAIT(op, fat_resolve_path(&op->resolve, blk, mnt));
    if (!op->resolve.found.valid) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    op->resp_override = 1;
    op->resp_arg0 = (int32_t)op->resolve.found.size;
    op->resp_arg1 = (op->resolve.found.attr & 0x10) ? 0x4000 : 0x8000;
    FAT_CO_END(op);
}

fat_r_t fat_op_unlink(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                      fat_open_pool_t* pool) {
    uint8_t path_is_init;
    int rc;

    (void)pool;
    FAT_CO_BEGIN(op);
    path_is_init = 0;
    rc = fat_read_and_translate_path(op, &path_is_init);
    if (rc != 0) {
        FAT_CO_FAIL(op, blk, rc);
    }
    op->remove.cont = 0;
    op->remove.path = op->fat_path;
    op->remove.source = op->source;
    op->remove.is_rmdir = 0;
    FAT_CO_AWAIT(op, fat_remove_path(&op->remove, blk, mnt, pool->files, FAT_MAX_OPEN_FILES));
    op->resp_override = 1;
    op->resp_arg0 = 0;
    FAT_CO_END(op);
}

/* RENAME carries BOTH paths in one client buffer: the source at offset 0 and
 * the destination immediately after its NUL, with arg0/arg1 their lengths.
 * One buffer keeps the request a single message; the destination's offset is
 * derived rather than sent, so the two cannot disagree. */
fat_r_t fat_op_rename(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                      fat_open_pool_t* pool) {
    uint32_t old_len;
    uint32_t new_len;
    uint8_t path_is_init;

    FAT_CO_BEGIN(op);
    old_len = (uint32_t)op->arg0;
    new_len = (uint32_t)op->arg1;
    path_is_init = 0;

    if (old_len == 0 || new_len == 0 || old_len >= sizeof(op->path) ||
        new_len >= sizeof(op->rename_path)) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    if (old_len + new_len + 2u > (uint32_t)wasmos_xfer_buffer_size()) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_PATH_TOO_LONG);
    }
    if (wasmos_sys_buffer_read(op->arg2, op->path, (int32_t)old_len, 0) != 0 ||
        wasmos_sys_buffer_read(
            op->arg2, op->rename_path, (int32_t)new_len, (int32_t)(old_len + 1u)) != 0) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BUFFER);
    }
    op->path[old_len] = '\0';
    op->rename_path[new_len] = '\0';

    if (vfs_translate_path(op->path, op->fat_path, sizeof(op->fat_path), &path_is_init) != 0 ||
        path_is_init) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_TRANSLATE);
    }
    if (vfs_translate_path(
            op->rename_path, op->fat_rename_path, sizeof(op->fat_rename_path), &path_is_init) !=
            0 ||
        path_is_init) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_TRANSLATE);
    }

    op->rename.cont = 0;
    op->rename.old_path = op->fat_path;
    op->rename.new_path = op->fat_rename_path;
    op->rename.source = op->source;
    /* POSIX rename() replaces an existing destination, and this is the boundary
     * callers reach through libc.  The driver-level entry point defaults to
     * refusing, so an internal caller that wants the strict behaviour keeps it. */
    op->rename.replace = 1;
    FAT_CO_AWAIT(op, fat_rename_path(&op->rename, blk, mnt, pool->files, FAT_MAX_OPEN_FILES));
    op->resp_override = 1;
    op->resp_arg0 = 0;
    FAT_CO_END(op);
}

fat_r_t fat_op_mkdir(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool) {
    uint8_t path_is_init;
    int rc;

    (void)pool;
    FAT_CO_BEGIN(op);
    path_is_init = 0;
    rc = fat_read_and_translate_path(op, &path_is_init);
    if (rc != 0) {
        FAT_CO_FAIL(op, blk, rc);
    }
    op->mkdir.cont = 0;
    op->mkdir.path = op->fat_path;
    op->mkdir.source = op->source;
    FAT_CO_AWAIT(op, fat_create_directory(&op->mkdir, blk, mnt));
    op->resp_override = 1;
    op->resp_arg0 = 0;
    FAT_CO_END(op);
}

fat_r_t fat_op_rmdir(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool) {
    uint8_t path_is_init;
    int rc;

    (void)pool;
    FAT_CO_BEGIN(op);
    path_is_init = 0;
    rc = fat_read_and_translate_path(op, &path_is_init);
    if (rc != 0) {
        FAT_CO_FAIL(op, blk, rc);
    }
    op->remove.cont = 0;
    op->remove.path = op->fat_path;
    op->remove.source = op->source;
    op->remove.is_rmdir = 1;
    FAT_CO_AWAIT(op, fat_remove_path(&op->remove, blk, mnt, pool->files, FAT_MAX_OPEN_FILES));
    op->resp_override = 1;
    op->resp_arg0 = 0;
    FAT_CO_END(op);
}

fat_r_t fat_op_seek(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                    fat_open_pool_t* pool) {
    fat_open_file_t* file;
    int32_t base;
    int64_t target;

    FAT_CO_BEGIN(op);
    file = fat_open_file_for_fd(pool, op->source, op->arg0);
    if (!file) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    if (op->arg2 == 0) {
        base = 0;
    } else if (op->arg2 == 1) {
        base = (int32_t)file->offset;
    } else if (op->arg2 == 2) {
        base = (int32_t)file->size;
    } else {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    target = (int64_t)base + (int64_t)op->arg1;
    if (target < 0 || (uint64_t)target > file->size) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_RANGE);
    }
    op->repos.cont = 0;
    op->repos.file = file;
    op->repos.offset = (uint32_t)target;
    op->repos.limit = file->size;
    /* Stage the response before the yield: `target` is a C local that would not
     * survive the FAT_CO_AWAIT below. */
    op->resp_override = 1;
    op->resp_arg0 = (int32_t)target;
    FAT_CO_AWAIT(op, fat_reposition_open_file(&op->repos, blk, mnt));
    FAT_CO_END(op);
}

fat_r_t fat_op_close(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                     fat_open_pool_t* pool) {
    fat_open_file_t* file;

    (void)mnt;
    FAT_CO_BEGIN(op);
    file = fat_open_file_for_fd(pool, op->source, op->arg0);
    if (!file) {
        FAT_CO_FAIL(op, blk, WASMOS_ERR_FS_BAD_ARGS);
    }
    file->in_use = 0;
    file->owner = -1;
    file->flags = 0;
    file->first_cluster = 0;
    file->current_cluster = 0;
    file->current_sector = 0;
    file->file_lba = 0;
    file->size = 0;
    file->capacity = 0;
    file->offset = 0;
    file->dir_lba = 0;
    file->dir_sector = 0;
    file->dir_index = 0;
    FAT_CO_END(op);
}
