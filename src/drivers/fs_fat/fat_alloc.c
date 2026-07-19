/* fat_alloc.c - FAT-table + cluster-chain access (read/scan side).  See
 * fat_alloc.h.  All I/O-bearing functions are fat_co.h coroutines; loop cursors
 * live in the context, and locals are declared without initializers because the
 * resume switch jumps past their declarations. */
#include "fat_alloc.h"
#include "fat_co.h"
#include "wasmos_driver_abi.h"

uint16_t fat_end_of_chain_marker(const fat_mount_t* mnt) {
    if (mnt->fat_type == FAT_TYPE_12) {
        return 0x0FFFu;
    }
    if (mnt->fat_type == FAT_TYPE_16) {
        return 0xFFFFu;
    }
    return 0;
}

uint32_t fat_total_clusters(const fat_mount_t* mnt) {
    uint32_t first_data = fat_first_data_lba(mnt);
    uint32_t data_sectors;

    if (first_data < mnt->boot_lba || mnt->total_sectors <= first_data - mnt->boot_lba ||
        mnt->sectors_per_cluster == 0) {
        return 0;
    }
    data_sectors = mnt->total_sectors - (first_data - mnt->boot_lba);
    return data_sectors / mnt->sectors_per_cluster;
}

fat_r_t fat_fatent_read(fat_fatent_ctx_t* e, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t fat_offset;
    uint16_t v;

    FAT_CO_BEGIN(e);
    if (e->cluster < 2) {
        FAT_CO_FAIL(e, blk, FS_ERR_CORRUPT);
    }
    if (mnt->fat_type == FAT_TYPE_12) {
        fat_offset = e->cluster + (e->cluster / 2u);
    } else if (mnt->fat_type == FAT_TYPE_16) {
        fat_offset = (uint32_t)e->cluster * 2u;
    } else {
        FAT_CO_FAIL(e, blk, FS_ERR_CORRUPT);
    }
    e->fat_lba = fat_table_lba(mnt) + (fat_offset / mnt->bytes_per_sector);
    e->sector_offset = fat_offset % mnt->bytes_per_sector;

    FAT_CO_READ(e, blk, e->fat_lba);
    e->lo = fat_block_sector(blk)[e->sector_offset];
    if (e->sector_offset + 1u < mnt->bytes_per_sector) {
        e->hi = fat_block_sector(blk)[e->sector_offset + 1u];
    } else {
        FAT_CO_READ(e, blk, e->fat_lba + 1u); /* FAT12 entry spans a sector edge */
        e->hi = fat_block_sector(blk)[0];
    }

    v = (uint16_t)e->lo | ((uint16_t)e->hi << 8);
    if (mnt->fat_type == FAT_TYPE_12) {
        if (e->cluster & 1u) {
            v >>= 4;
        } else {
            v &= 0x0FFFu;
        }
    }
    e->value = v;
    FAT_CO_END(e);
}

fat_r_t fat_fatent_write(fat_fatent_ctx_t* e, fat_block_t* blk, const fat_mount_t* mnt) {
    uint16_t current;
    uint16_t merged;
    uint16_t val;

    FAT_CO_BEGIN(e);
    if (e->cluster < 2) {
        FAT_CO_FAIL(e, blk, FS_ERR_CORRUPT);
    }
    if (mnt->fat_type == FAT_TYPE_12) {
        e->fat_offset = e->cluster + (e->cluster / 2u);
    } else if (mnt->fat_type == FAT_TYPE_16) {
        e->fat_offset = (uint32_t)e->cluster * 2u;
    } else {
        FAT_CO_FAIL(e, blk, FS_ERR_CORRUPT);
    }

    if (mnt->fat_type == FAT_TYPE_12) {
        /* Read the current entry (copy 0) and merge the 12-bit value into the
         * byte shared with the neighbouring entry. */
        e->fat_lba = fat_table_lba(mnt) + (e->fat_offset / mnt->bytes_per_sector);
        e->sector_offset = e->fat_offset % mnt->bytes_per_sector;
        FAT_CO_READ(e, blk, e->fat_lba);
        e->lo = fat_block_sector(blk)[e->sector_offset];
        if (e->sector_offset + 1u < mnt->bytes_per_sector) {
            e->hi = fat_block_sector(blk)[e->sector_offset + 1u];
        } else {
            FAT_CO_READ(e, blk, e->fat_lba + 1u);
            e->hi = fat_block_sector(blk)[0];
        }
        current = (uint16_t)e->lo | ((uint16_t)e->hi << 8);
        val = e->write_value & 0x0FFFu;
        if (e->cluster & 1u) {
            merged = (uint16_t)((current & 0x000Fu) | (uint16_t)(val << 4));
        } else {
            merged = (uint16_t)((current & 0xF000u) | val);
        }
        e->lo = (uint8_t)(merged & 0xFFu);
        e->hi = (uint8_t)((merged >> 8) & 0xFFu);
    } else {
        e->lo = (uint8_t)(e->write_value & 0xFFu);
        e->hi = (uint8_t)((e->write_value >> 8) & 0xFFu);
    }

    /* Store lo/hi at fat_offset in every FAT copy (read-modify-write). */
    for (e->copy_idx = 0; e->copy_idx < mnt->fat_count; ++e->copy_idx) {
        e->fat_lba = mnt->boot_lba + mnt->reserved_sectors + e->copy_idx * mnt->fat_size +
                     (e->fat_offset / mnt->bytes_per_sector);
        e->sector_offset = e->fat_offset % mnt->bytes_per_sector;

        FAT_CO_READ(e, blk, e->fat_lba);
        fat_block_sector(blk)[e->sector_offset] = e->lo;
        if (e->sector_offset + 1u < mnt->bytes_per_sector) {
            fat_block_sector(blk)[e->sector_offset + 1u] = e->hi;
            FAT_CO_WRITE(e, blk, e->fat_lba);
        } else {
            /* The entry straddles a sector boundary. */
            FAT_CO_WRITE(e, blk, e->fat_lba);
            FAT_CO_READ(e, blk, e->fat_lba + 1u);
            fat_block_sector(blk)[0] = e->hi;
            FAT_CO_WRITE(e, blk, e->fat_lba + 1u);
        }
    }
    FAT_CO_END(e);
}

fat_r_t fat_chain_next(fat_chain_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt) {
    uint16_t v;

    FAT_CO_BEGIN(c);
    if (c->cluster < 2) {
        c->next = 0;
        FAT_CO_DONE(c);
    }
    c->ent.cont = 0;
    c->ent.cluster = c->cluster;
    FAT_CO_AWAIT(c, fat_fatent_read(&c->ent, blk, mnt));

    v = c->ent.value;
    if ((mnt->fat_type == FAT_TYPE_12 && v >= 0x0FF8u) ||
        (mnt->fat_type == FAT_TYPE_16 && v >= 0xFFF8u) || v < 2u) {
        c->next = 0; /* end-of-chain (not an error) */
    } else {
        c->next = v;
    }
    FAT_CO_END(c);
}

fat_r_t fat_chain_walk(fat_chainwalk_ctx_t* w, fat_block_t* blk, const fat_mount_t* mnt) {
    FAT_CO_BEGIN(w);
    w->last = w->cluster;
    w->hops = 0;
    for (;;) {
        w->hops++;
        w->step.cont = 0;
        w->step.cluster = w->last;
        FAT_CO_AWAIT(w, fat_chain_next(&w->step, blk, mnt));
        if (w->step.next == 0) {
            break;
        }
        w->last = w->step.next;
    }
    FAT_CO_END(w);
}

fat_r_t fat_find_free_cluster(fat_findfree_ctx_t* f, fat_block_t* blk, const fat_mount_t* mnt) {
    FAT_CO_BEGIN(f);
    f->total = fat_total_clusters(mnt);
    if (f->total == 0) {
        FAT_CO_FAIL(f, blk, FS_ERR_NO_SPACE);
    }
    for (f->cursor = 2; f->cursor < f->total + 2u; ++f->cursor) {
        f->ent.cont = 0;
        f->ent.cluster = (uint16_t)f->cursor;
        FAT_CO_AWAIT(f, fat_fatent_read(&f->ent, blk, mnt));
        if (f->ent.value == 0) {
            f->result = (uint16_t)f->cursor;
            FAT_CO_DONE(f);
        }
    }
    FAT_CO_FAIL(f, blk, FS_ERR_NO_SPACE);
    FAT_CO_END(f);
}
