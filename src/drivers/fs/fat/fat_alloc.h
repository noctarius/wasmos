/* fat_alloc.h - FAT-table + cluster-chain access for the reactor.
 *
 * All I/O-bearing functions are coroutines (fat_co.h): they take their own
 * embedded context (with `int cont`) plus the block layer and the mount
 * geometry.  They yield per sector via fat_need_sector / fat_block_write.
 *
 * Chain mutation used by file growth (find free -> write EOC -> link last) is
 * composed at the file layer from fat_find_free_cluster + fat_fatent_write. */
#ifndef FS_FAT_FAT_ALLOC_H
#define FS_FAT_FAT_ALLOC_H

#include <stdint.h>
#include "fat_block.h"
#include "fat_geom.h"
#include "fat_types.h"

/* Pure geometry helpers.  The end-of-chain marker is 0x0FFF on FAT12, 0xFFFF on
 * FAT16 and 0x0FFFFFFF on FAT32 (28 significant bits); it is 0 for an unknown
 * volume type, which callers treat as unsupported rather than as a valid
 * marker.  fat_total_clusters returns 0 when the geometry is inconsistent. */
uint32_t fat_end_of_chain_marker(const fat_mount_t* mnt);
uint32_t fat_total_clusters(const fat_mount_t* mnt);

/* Read the FAT-table entry for e->cluster into e->value.  FAT_R_ERR on a
 * corrupt cluster index or I/O error. */
fat_r_t fat_fatent_read(fat_fatent_ctx_t* e, fat_block_t* blk, const fat_mount_t* mnt);

/* Store e->write_value into the FAT-table entry for e->cluster, in every FAT
 * copy (FAT12 read-merges the shared nibble byte).  FAT_R_ERR on fault. */
fat_r_t fat_fatent_write(fat_fatent_ctx_t* e, fat_block_t* blk, const fat_mount_t* mnt);

/* Resolve c->cluster's successor into c->next; c->next == 0 means end-of-chain
 * (a normal result, distinct from FAT_R_ERR which is an I/O/corruption fault). */
fat_r_t fat_chain_next(fat_chain_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt);

/* Walk w->cluster's chain to its end, setting w->last (last cluster) and w->hops
 * (clusters visited, >= 1). */
fat_r_t fat_chain_walk(fat_chainwalk_ctx_t* w, fat_block_t* blk, const fat_mount_t* mnt);

/* Find the first free cluster into f->result.  FAT_R_ERR with WASMOS_ERR_FS_NO_SPACE if
 * the volume is full. */
fat_r_t fat_find_free_cluster(fat_findfree_ctx_t* f, fat_block_t* blk, const fat_mount_t* mnt);

#endif /* FS_FAT_FAT_ALLOC_H */
