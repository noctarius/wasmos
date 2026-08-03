/* fat_geom.c - FAT geometry + async (reactor-driven) mount bring-up.  See
 * fat_geom.h. */
#include "fat_geom.h"
#include "fat_block.h"
#include "fat_co.h"
#include "fat_util.h"
#include "wasmos_driver_abi.h"

uint32_t fat_first_data_lba(const fat_mount_t* mnt) {
    return mnt->root_dir_lba + mnt->root_dir_sectors;
}

uint32_t fat_lba_for_cluster(const fat_mount_t* mnt, uint16_t cluster) {
    if (cluster < 2) {
        return 0;
    }
    return fat_first_data_lba(mnt) + (uint32_t)(cluster - 2) * mnt->sectors_per_cluster;
}

uint32_t fat_dir_entry_limit(const fat_mount_t* mnt, uint8_t root, uint32_t dir_sectors) {
    if (root) {
        return mnt->root_entry_count;
    }
    return (dir_sectors * mnt->bytes_per_sector) / 32u;
}

uint32_t fat_table_lba(const fat_mount_t* mnt) {
    return mnt->boot_lba + mnt->reserved_sectors;
}

/* Parse an MBR partition table in `sector`; on success writes the first FAT
 * partition's start LBA to *out_lba.  Returns 0 or -1. */
static int fat_try_parse_mbr(const uint8_t* sector, uint32_t* out_lba) {
    uint16_t sig = (uint16_t)sector[510] | ((uint16_t)sector[511] << 8);
    if (sig != 0xAA55) {
        return -1;
    }
    const fat_mbr_entry_t* entries = ptr_cast(const fat_mbr_entry_t, (sector + 446));
    for (uint32_t i = 0; i < 4; ++i) {
        uint8_t type = entries[i].type;
        if (type == 0x00) {
            continue;
        }
        if (type == 0xEE) {
            fat_log("GPT detected (unsupported)\n");
            return -1;
        }
        if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C ||
            type == 0x0E) {
            *out_lba = entries[i].lba_start;
            return 0;
        }
    }
    return -1;
}

/* Parse the BPB in `sector` into *mnt (mnt->boot_lba must already be set to the
 * volume's boot-sector LBA).  Returns 0 or -1. */
static int fat_parse_boot(fat_mount_t* mnt, const uint8_t* sector) {
    const fat_bpb_t* bpb = ptr_cast(const fat_bpb_t, sector);
    uint16_t sig = (uint16_t)sector[510] | ((uint16_t)sector[511] << 8);
    uint32_t bytes_per_sector = bpb->bytes_per_sector;
    uint32_t total_sectors, fat_size, root_dir_sectors, data_sectors, cluster_count;

    if (sig != 0xAA55 || (bytes_per_sector != 512 && bytes_per_sector != 1024 &&
                          bytes_per_sector != 2048 && bytes_per_sector != 4096)) {
        fat_log("invalid bytes_per_sector\n");
        return -1;
    }

    total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    fat_size = bpb->fat_size_16;
    if (fat_size == 0) {
        fat_size = ((const uint32_t*)bpb->ext)[0];
    }
    root_dir_sectors = ((bpb->root_entry_count * 32u) + (bytes_per_sector - 1u)) / bytes_per_sector;
    data_sectors =
        total_sectors - (bpb->reserved_sectors + (bpb->fat_count * fat_size) + root_dir_sectors);
    cluster_count = data_sectors / bpb->sectors_per_cluster;

    mnt->bytes_per_sector = (uint16_t)bytes_per_sector;
    mnt->sectors_per_cluster = bpb->sectors_per_cluster;
    mnt->reserved_sectors = bpb->reserved_sectors;
    mnt->fat_count = bpb->fat_count;
    mnt->root_entry_count = bpb->root_entry_count;
    mnt->fat_size = fat_size;
    mnt->total_sectors = total_sectors;
    mnt->root_dir_sectors = root_dir_sectors;
    mnt->root_dir_lba = mnt->boot_lba + bpb->reserved_sectors + (bpb->fat_count * fat_size);

    if (bpb->root_entry_count == 0) {
        mnt->fat_type = FAT_TYPE_32;
        fat_log("FAT32 detected\n");
    } else if (cluster_count < 4085) {
        mnt->fat_type = FAT_TYPE_12;
        fat_log("FAT12 detected\n");
    } else if (cluster_count < 65525) {
        mnt->fat_type = FAT_TYPE_16;
        fat_log("FAT16 detected\n");
    } else {
        mnt->fat_type = FAT_TYPE_32;
        fat_log("FAT32 detected\n");
    }
    return 0;
}

void fat_mount_init(fat_mount_t* mnt) {
    mnt->boot_lba = 0;
    mnt->fat_type = FAT_TYPE_UNKNOWN;
    mnt->cwd_source = -1;
    mnt->cwd_cluster = 0;
    mnt->cwd_root = 1;
    mnt->cwd_mount = VFS_MOUNT_BOOT;
    mnt->dir_lba = 0;
    mnt->dir_sectors = 0;
    mnt->cont = 0;
    mnt->mounted = 0;
    mnt->tried_mbr = 0;
}

int fat_mount_ready(const fat_mount_t* mnt) {
    return mnt->mounted;
}

/* Coroutine (context = *mnt): read LBA 0, MBR-probe once if it is not a BPB,
 * parse the BPB.  Locals are declared without initializers because the resume
 * switch jumps past their declarations (they are assigned before use, and none
 * are carried across a yield — cross-yield state lives in *mnt). */
fat_r_t fat_geom_mount_step(fat_mount_t* mnt, fat_block_t* blk) {
    const uint8_t* sector;
    uint16_t sig;
    uint16_t bytes_per_sector;
    uint32_t part_lba;

    FAT_CO_BEGIN(mnt);
    if (mnt->mounted) {
        FAT_CO_DONE(mnt);
    }
    mnt->boot_lba = 0;
    mnt->tried_mbr = 0;
    FAT_CO_READ(mnt, blk, 0u); /* boot sector at LBA 0 */

    sector = fat_block_sector(blk);
    sig = (uint16_t)sector[510] | ((uint16_t)sector[511] << 8);
    bytes_per_sector = (uint16_t)sector[11] | ((uint16_t)sector[12] << 8);
    if (sig != 0xAA55 || bytes_per_sector == 0) {
        /* LBA 0 is an MBR, not a BPB: locate the first FAT partition. */
        if (fat_try_parse_mbr(sector, &part_lba) != 0) {
            fat_log("no FAT boot sector\n");
            FAT_CO_FAIL(mnt, blk, -(int32_t)WASMOS_ERR_FS_NOT_READY);
        }
        mnt->tried_mbr = 1;
        mnt->boot_lba = part_lba;
        FAT_CO_READ(mnt, blk, mnt->boot_lba); /* partition boot sector */
    }

    if (fat_parse_boot(mnt, fat_block_sector(blk)) != 0) {
        fat_log("boot parse failed\n");
        FAT_CO_FAIL(mnt, blk, -(int32_t)WASMOS_ERR_FS_CORRUPT);
    }
    mnt->mounted = 1;
    FAT_CO_END(mnt);
}
