/* fat_geom.h - FAT volume geometry: BPB/MBR parse, cluster<->LBA math, mount
 * bring-up.  Mount geometry is immutable after fat_geom_mount(); it lives in a
 * caller-owned fat_mount_t (no globals).  Also holds the driver's single
 * current-working-directory navigation state. */
#ifndef FS_FAT_FAT_GEOM_H
#define FS_FAT_FAT_GEOM_H

#include <stdint.h>
#include "fat_types.h"
#include "fat_block.h"

typedef struct {
    /* Immutable-after-mount geometry. */
    uint32_t boot_lba; /* LBA of the volume boot sector (0, or the partition) */
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint32_t fat_size; /* sectors per FAT */
    uint32_t total_sectors;
    fat_type_t fat_type;
    uint32_t root_dir_lba;     /* FAT12/16 fixed root region; 0 on FAT32 */
    uint32_t root_dir_sectors; /* FAT12/16 fixed root region; 0 on FAT32 */
    uint32_t root_cluster;     /* FAT32 root chain start (BPB_RootClus); 0 otherwise */
    uint32_t fsinfo_lba;       /* FAT32 FSInfo sector; 0 when absent or not FAT32 */

    /* Single current-working-directory navigation state (set by CHDIR). */
    int32_t cwd_source; /* endpoint that owns the cwd, or -1 */
    uint32_t cwd_cluster;
    uint8_t cwd_root;
    vfs_mount_t cwd_mount;
    uint32_t dir_lba;     /* current dir first LBA (when not root) */
    uint32_t dir_sectors; /* current dir span in sectors */

    /* Lazy mount bring-up state (fat_geom_mount_step is a coroutine on *mnt). */
    int cont;        /* coroutine resume point */
    uint8_t mounted; /* 1 once the BPB is parsed */
    uint8_t tried_mbr;
} fat_mount_t;

/* Initialize *mnt to the unmounted state (call once at driver init). */
void fat_mount_init(fat_mount_t* mnt);

/* 1 once the volume geometry has been parsed. */
int fat_mount_ready(const fat_mount_t* mnt);

/* Coroutine (context = *mnt) that brings the volume up: reads LBA 0, probes an
 * MBR partition table if it is not a BPB, parses the BPB.  Driven by the reactor
 * like any op (blk->owner must be the active op).  Returns FAT_R_WAIT (read
 * submitted; resume on completion), FAT_R_DONE (mounted), or FAT_R_ERR (failed;
 * the active op's err is set via blk).  No synchronous variant. */
fat_r_t fat_geom_mount_step(fat_mount_t* mnt, fat_block_t* blk);

uint32_t fat_first_data_lba(const fat_mount_t* mnt);
uint32_t fat_lba_for_cluster(const fat_mount_t* mnt, uint32_t cluster);
uint32_t fat_dir_entry_limit(const fat_mount_t* mnt, uint8_t root, uint32_t dir_sectors);

/* Describe the root directory as a scan origin, the one place that knows how a
 * volume addresses its root.  FAT12/16 have a fixed contiguous region
 * (*out_root = 1, *out_cluster = 0); on FAT32 the root is an ordinary cluster
 * chain starting at root_cluster, so *out_root = 0 and it scans through the
 * same path as any subdirectory.  Returns 0, or -1 when the mount describes no
 * usable root -- which callers report as "not found" rather than proceeding. */
int fat_root_origin(const fat_mount_t* mnt, uint8_t* out_root, uint32_t* out_cluster,
                    uint32_t* out_lba, uint32_t* out_sectors);

/* First LBA of the FAT table region and the byte offset math base, for fat_alloc
 * (FAT-entry addressing): sector = fat_table_lba(mnt) + fat_offset/bytes_per_sec,
 * within-sector byte = fat_offset % bytes_per_sec. */
uint32_t fat_table_lba(const fat_mount_t* mnt);

#endif /* FS_FAT_FAT_GEOM_H */
