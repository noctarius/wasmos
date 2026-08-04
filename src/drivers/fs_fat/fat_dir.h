/* fat_dir.h - directory scan + path resolution for the FAT reactor.
 *
 * All I/O-bearing functions are coroutines (fat_co.h): they take their own
 * context (fat_dir_scan_ctx_t / fat_resolve_ctx_t / fat_resolve_parent_ctx_t,
 * defined in fat_types.h and embedded in fat_op_ctx_t) plus the block layer and
 * the immutable mount geometry, and yield per directory sector via FAT_CO_READ.
 * The pure path helpers carry no I/O and no global mutable state.
 *
 * Match semantics: a scan that runs off the end of a directory without a match
 * is NOT an error (found.valid stays 0, the step returns FAT_R_DONE); only a
 * real block-I/O fault propagates as FAT_R_ERR. */
#ifndef FS_FAT_FAT_DIR_H
#define FS_FAT_FAT_DIR_H

#include <stdint.h>
#include "fat_block.h"
#include "fat_geom.h"
#include "fat_types.h"

/* --- Pure path helpers (no I/O, no coroutine). --- */

/* Translate a client path into the FAT-namespace path (out[out_len]); sets
 * *out_is_init when the path routes to the init overlay.  0 on success, -1 on
 * bad args / path too long. */
int vfs_translate_path(const char* in, char* out, uint32_t out_len, uint8_t* out_is_init);

/* Copy the next '/'-delimited component of `path` from *pos into
 * component[component_len], advancing *pos.  1 = a component, 0 = end of path,
 * -1 = bad args / component too long. */
int fat_path_next_component(const char* path, uint32_t* pos, char* component,
                            uint32_t component_len);

/* 1 if `path` has a further non-empty component past `pos`. */
int fat_path_has_more(const char* path, uint32_t pos);

/* --- Directory scan + path resolution (coroutines; contexts in fat_types.h). --- */

/* Scan the directory described by s's inputs for s->target.  FAT_R_DONE (check
 * s->found.valid), FAT_R_WAIT, or FAT_R_ERR on I/O fault. */
fat_r_t fat_find_in_dir(fat_dir_scan_ctx_t* s, fat_block_t* blk, const fat_mount_t* mnt);

/* Resolve r->path to a directory entry in r->found (found.valid tells hit/miss). */
fat_r_t fat_resolve_path(fat_resolve_ctx_t* r, fat_block_t* blk, const fat_mount_t* mnt);

/* Resolve p->path to its PARENT directory + leaf name (see fat_resolve_parent_ctx_t
 * in fat_types.h for how the parent descriptor is packed into p->found). */
fat_r_t fat_resolve_parent_dir(fat_resolve_parent_ctx_t* p, fat_block_t* blk,
                               const fat_mount_t* mnt);

/* --- Directory mutation (coroutines; contexts in fat_types.h). --- */

/* Pure open-file-table check: 1 if `entry`'s (dir_lba,dir_sector,dir_index)
 * matches an in-use open file in `files[count]`, else 0.  See the fat_dir.c
 * TODO: the reactor's open-file table lives in fat_file (not yet ported), so
 * callers currently pass NULL/0 and this returns 0 (never blocks). */
int fat_entry_is_open(const fat_dir_entry_info_t* entry, const fat_open_file_t* files,
                      uint32_t count);

/* Free every cluster of first_cluster's chain (writes 0 into each FAT entry).
 * Set f->cluster = first_cluster (and f->cont = 0) before the first step. */
fat_r_t fat_free_cluster_chain(fat_freechain_ctx_t* f, fat_block_t* blk, const fat_mount_t* mnt);

/* Scan a directory's raw 8.3 entries for s->short_name; s->result = 1 present,
 * 0 absent.  Inputs: dir_lba, dir_sectors, entry_limit, short_name[11]. */
fat_r_t fat_short_name_exists_in_dir(fat_shortscan_ctx_t* s, fat_block_t* blk,
                                     const fat_mount_t* mnt);

/* Find f->needed consecutive free directory slots; f->out_entry = first slot's
 * flat index, f->result = 0 found / -1 none.  Inputs: dir_lba, dir_sectors,
 * entry_limit, needed. */
fat_r_t fat_find_free_dir_slots(fat_findslots_ctx_t* f, fat_block_t* blk, const fat_mount_t* mnt);

/* RMW w->entry[32] into the slot w->entry_index of directory w->dir_lba. */
fat_r_t fat_write_dir_entry(fat_writeent_ctx_t* w, fat_block_t* blk, const fat_mount_t* mnt);

/* Mark d->entry_index's short entry + its preceding LFN entries deleted (0xE5).
 * Inputs: dir_lba, entry_index. */
fat_r_t fat_delete_dir_entry_chain(fat_delchain_ctx_t* d, fat_block_t* blk, const fat_mount_t* mnt);

/* Core create.  Inputs on `c`: path, source, attr, cluster, size,
 * fail_if_exists.  On FAT_R_DONE, c->found holds the created (or, when
 * !fail_if_exists and it already existed, the pre-existing) entry. */
fat_r_t fat_create_path_entry(fat_create_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt);

/* Create an empty file at c->path (thin wrapper: drives fat_create_path_entry
 * with attr=0, cluster=0, size=0, fail_if_exists=0).  Set c->path/c->source;
 * c->found is the result.  fail_if_exists is forced to 0 here regardless of the
 * incoming field (matches the original fat_create_empty_file). */
fat_r_t fat_create_empty_file(fat_create_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt);

/* Create a directory at m->path: allocate a cluster, write EOC, init '.'/'..',
 * then create the 0x10 entry.  Inputs on `m`: path, source. */
fat_r_t fat_create_directory(fat_mkdir_ctx_t* m, fat_block_t* blk, const fat_mount_t* mnt);

/* Unlink a file (r->is_rmdir = 0) or remove an empty directory (r->is_rmdir = 1)
 * at r->path.  Inputs on `r`: path, source, is_rmdir. */
fat_r_t fat_remove_path(fat_remove_ctx_t* r, fat_block_t* blk, const fat_mount_t* mnt,
                        const fat_open_file_t* files, uint32_t file_count);

/* --- Directory navigation (READDIR / CHDIR).  Contexts in fat_types.h. --- */

/* Stream the entries of the CURRENT directory (root region when mnt->cwd_root,
 * else the cwd subdir at mnt->dir_lba/dir_sectors) to op->source over
 * fs_endpoint as FS_IPC_STREAM messages (4 bytes per message, with the
 * original's IPC_ERR_FULL retry + wasmos_console_write fallback), one line per
 * entry ("name" + "/" for a directory + "\n").  Uses op->readdir.  Sends no
 * response itself (resp_override stays 0); the reactor emits the final
 * FS_IPC_RESP on FAT_R_DONE.  FAT_R_ERR only on a block-I/O fault. */
fat_r_t fat_op_readdir(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                       int32_t fs_endpoint);

/* Change the cwd to op->dir_name (already unpacked from arg0..3).  Empty or
 * "/"-only resets the cwd to the mount root.  Otherwise walks the components
 * (relative to the cwd unless the name starts with '/'), descending each matched
 * subdirectory, and on success repoints mnt's cwd_* / dir_lba / dir_sectors and
 * cwd_source.  Uses op->chdir.  FAT_CO_FAIL(WASMOS_ERR_FS_NOT_FOUND / WASMOS_ERR_FS_NOT_DIR)
 * on a missing / non-directory component. */
fat_r_t fat_op_chdir(fat_op_ctx_t* op, fat_block_t* blk, fat_mount_t* mnt);

#endif /* FS_FAT_FAT_DIR_H */
