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
    /* Whether the block device this driver holds is a partition rather than a
     * whole disk, from the descriptor's `partition` field. A partition device
     * rebases every transfer onto its window, so its LBA 0 is the volume's boot
     * sector and there is no table on it to parse. Set by fat_mount_init and
     * never changed. */
    uint8_t is_partition;

    /* Immutable-after-mount geometry. */
    uint32_t boot_lba; /* absolute LBA of the volume boot sector */
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

    /* Lazy mount bring-up state (fat_geom_mount_step is a coroutine on *mnt). */
    int cont;        /* coroutine resume point */
    uint8_t mounted; /* 1 once the BPB is parsed */
} fat_mount_t;

/* Initialize *mnt to the unmounted state (call once at driver init). */
/* Initialise *mnt. `is_partition` is the descriptor's `partition` field reduced
 * to a flag: nonzero means the device is one partition of a disk, addressed from
 * its own LBA 0, and holds no partition table. */
void fat_mount_init(fat_mount_t* mnt, uint8_t is_partition);

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
