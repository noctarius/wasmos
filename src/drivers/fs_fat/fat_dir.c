/* fat_dir.c - directory scan, path resolution, mutation and navigation.  See
 * fat_dir.h.
 *
 * Every I/O-bearing function is a fat_co.h coroutine: loop cursors live in the
 * context, and C locals carry no initializers because the resume switch jumps
 * past their declarations.  Volume geometry is read from the immutable
 * fat_mount_t; nothing here holds file-scope mutable state. */
#include "fat_dir.h"
#include "fat_co.h"
#include "fat_alloc.h"
#include "fat_name.h"
#include "fat_util.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

/* --- Pure path helpers (no I/O). --- */

int vfs_translate_path(const char* in, char* out, uint32_t out_len, uint8_t* out_is_init) {
    uint32_t i;

    if (!in || !out || out_len < 2 || !out_is_init) {
        return -1;
    }
    *out_is_init = 0;
    /* Straight copy, leading '/' included: whether a path is absolute is decided
     * downstream by the resolvers, not by this translation. */
    i = 0;
    while (in[i] && i + 1 < out_len) {
        out[i] = in[i];
        i++;
    }
    out[i] = '\0';
    return in[i] == '\0' ? 0 : -1;
}

int fat_path_next_component(const char* path, uint32_t* pos, char* component,
                            uint32_t component_len) {
    uint32_t out = 0;

    if (!path || !pos || !component || component_len < 2) {
        return -1;
    }

    while (path[*pos] == '/') {
        (*pos)++;
    }
    if (path[*pos] == '\0') {
        component[0] = '\0';
        return 0;
    }

    while (path[*pos] && path[*pos] != '/') {
        if (out + 1 >= component_len) {
            return -1;
        }
        component[out++] = path[*pos];
        (*pos)++;
    }
    component[out] = '\0';
    return 1;
}

int fat_path_has_more(const char* path, uint32_t pos) {
    while (path[pos] == '/') {
        pos++;
    }
    return path[pos] != '\0';
}

/* --- On-disk start-cluster field. ---
 *
 * A directory entry splits its first-cluster number: the low half at bytes
 * 26..27 and the high half at bytes 20..21.  The high half is zero on FAT12/16
 * (where it is a reserved field), so reading both is correct on every volume
 * type and writing both keeps a FAT12/16 entry byte-identical to what the older
 * two-byte store produced. */
uint32_t fat_dirent_cluster(const uint8_t* ent) {
    uint32_t lo = (uint32_t)ent[26] | ((uint32_t)ent[27] << 8);
    uint32_t hi = (uint32_t)ent[20] | ((uint32_t)ent[21] << 8);

    return (hi << 16) | lo;
}

void fat_dirent_set_cluster(uint8_t* ent, uint32_t cluster) {
    ent[26] = (uint8_t)(cluster & 0xFFu);
    ent[27] = (uint8_t)((cluster >> 8) & 0xFFu);
    ent[20] = (uint8_t)((cluster >> 16) & 0xFFu);
    ent[21] = (uint8_t)((cluster >> 24) & 0xFFu);
}

/* --- Directory scan. --- */

fat_r_t fat_find_in_dir(fat_dir_scan_ctx_t* s, fat_block_t* blk, const fat_mount_t* mnt) {
    uint8_t* ent;
    char entry_name[FAT_LFN_MAX + 1u];
    uint32_t entries_per_sector;

    FAT_CO_BEGIN(s);

    s->found.valid = 0;
    if (!s->target) {
        FAT_CO_DONE(s); /* miss: caller checks found.valid */
    }

    s->entries_left = s->entry_limit;
    s->hops = 0;
    s->first_cluster = s->cur_cluster;
    fat_lfn_reset(&s->lfn);

    /* Outer loop advances the directory: over the sectors of the current run,
     * then (non-root) hops to the next cluster of the chain. */
    for (;;) {
        for (s->cur_sector = 0; s->cur_sector < s->dir_sectors && s->entries_left > 0;
             ++s->cur_sector) {
            entries_per_sector = mnt->bytes_per_sector / 32u;
            s->entries_total =
                s->entries_left < entries_per_sector ? s->entries_left : entries_per_sector;

            FAT_CO_READ(s, blk, s->dir_lba + s->cur_sector);

            for (s->scan_index = 0; s->scan_index < s->entries_total; ++s->scan_index) {
                ent = fat_block_sector(blk) + s->scan_index * 32u;

                if (ent[0] == 0x00) {
                    fat_lfn_reset(&s->lfn);
                    FAT_CO_DONE(s); /* end-of-directory: definite miss */
                }
                if (ent[0] == 0xE5) {
                    fat_lfn_reset(&s->lfn);
                    continue;
                }
                if ((ent[11] & 0x0F) == 0x0F) {
                    fat_lfn_collect(&s->lfn, ent);
                    continue;
                }
                if (ent[11] & 0x08) {
                    fat_lfn_reset(&s->lfn);
                    continue;
                }

                fat_entry_name_from_dirent(&s->lfn, ent, entry_name, sizeof(entry_name));
                if (!fat_name_eq(entry_name, s->target)) {
                    fat_lfn_reset(&s->lfn);
                    continue;
                }

                s->found.valid = 1;
                s->found.attr = ent[11];
                s->found.cluster = fat_dirent_cluster(ent);
                s->found.size = (uint32_t)ent[28] | ((uint32_t)ent[29] << 8) |
                                ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
                s->found.dir_lba = s->dir_lba;
                s->found.dir_sector = s->cur_sector;
                s->found.dir_index = s->scan_index;
                /* `hops` is the number of clusters advanced so far, so it IS
                 * the current cluster's ordinal within the chain. */
                s->found.dir_entry_index =
                    s->hops * (s->cur_root
                                   ? 0u
                                   : mnt->sectors_per_cluster * (mnt->bytes_per_sector / 32u)) +
                    s->cur_sector * (mnt->bytes_per_sector / 32u) + s->scan_index;
                s->found.dir_first_cluster = s->cur_root ? 0u : s->first_cluster;
                s->found.dir_root = s->cur_root;
                fat_lfn_reset(&s->lfn);
                FAT_CO_DONE(s);
            }

            s->entries_left -= s->entries_total;
        }

        /* Root region is a single contiguous run: no chain to follow. */
        if (s->cur_root) {
            break;
        }
        /* Non-root: follow the cluster chain to the next cluster and rescan.
         * `entry_limit` budgets one cluster run, so it is refilled per cluster
         * below rather than spent across the chain; `hops` is what terminates
         * the walk, because a corrupt volume can present a cyclic chain and no
         * honest chain exceeds the volume's cluster count. */
        if (s->hops >= fat_total_clusters(mnt)) {
            FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_CORRUPT);
        }
        s->hops++;
        s->chain.cont = 0;
        s->chain.cluster = s->cur_cluster;
        FAT_CO_AWAIT(s, fat_chain_next(&s->chain, blk, mnt));
        if (s->chain.next == 0) {
            break; /* end-of-chain */
        }
        s->cur_cluster = s->chain.next;
        s->dir_lba = fat_lba_for_cluster(mnt, s->cur_cluster);
        s->dir_sectors = mnt->sectors_per_cluster;
        if (s->dir_lba == 0 || s->dir_sectors == 0) {
            break;
        }
        s->entries_left = s->entry_limit;
    }

    fat_lfn_reset(&s->lfn);
    FAT_CO_END(s); /* miss (found.valid == 0) */
}

/* --- Path resolution. --- */

fat_r_t fat_resolve_path(fat_resolve_ctx_t* r, fat_block_t* blk, const fat_mount_t* mnt) {
    FAT_CO_BEGIN(r);

    r->found.valid = 0;
    if (!r->path ||
        fat_root_origin(mnt, &r->cur_root, &r->cur_cluster, &r->cur_lba, &r->cur_sectors) != 0) {
        FAT_CO_DONE(r);
    }
    r->pos = 0;

    /* Relative path from the owning endpoint's cwd (when cwd is a subdir). */
    if (r->path[0] != '\0' && r->path[0] != '/' && r->source == mnt->cwd_source && !mnt->cwd_root &&
        mnt->dir_lba != 0) {
        r->cur_root = 0;
        r->cur_cluster = mnt->cwd_cluster;
        r->cur_lba = mnt->dir_lba;
        r->cur_sectors = mnt->dir_sectors;
    }

    for (;;) {
        r->comp_rc = fat_path_next_component(r->path, &r->pos, r->component, sizeof(r->component));
        if (r->comp_rc <= 0) {
            FAT_CO_DONE(r); /* malformed / empty path */
        }
        if (r->component[0] == '.' && r->component[1] == '\0') {
            if (!fat_path_has_more(r->path, r->pos)) {
                FAT_CO_DONE(r);
            }
            continue;
        }
        if (r->component[0] == '.' && r->component[1] == '.' && r->component[2] == '\0') {
            /* TODO: '..' jumps to the root region instead of the true parent —
             * the on-disk '..' entry is never consulted, so "a/b/../c" resolves
             * against the root rather than against "a". */
            if (fat_root_origin(mnt, &r->cur_root, &r->cur_cluster, &r->cur_lba, &r->cur_sectors) !=
                0) {
                FAT_CO_DONE(r);
            }
            if (!fat_path_has_more(r->path, r->pos)) {
                FAT_CO_DONE(r);
            }
            continue;
        }

        r->scan.cont = 0;
        r->scan.target = r->component;
        r->scan.dir_lba = r->cur_lba;
        r->scan.dir_sectors = r->cur_sectors;
        r->scan.entry_limit = fat_dir_entry_limit(mnt, r->cur_root, r->cur_sectors);
        r->scan.cur_cluster = r->cur_cluster;
        r->scan.cur_root = r->cur_root;
        FAT_CO_AWAIT(r, fat_find_in_dir(&r->scan, blk, mnt));
        if (!r->scan.found.valid) {
            FAT_CO_DONE(r); /* component not found */
        }

        if (!fat_path_has_more(r->path, r->pos)) {
            r->found = r->scan.found;
            FAT_CO_DONE(r);
        }
        if (!(r->scan.found.attr & 0x10) || r->scan.found.cluster < 2) {
            FAT_CO_DONE(r); /* intermediate is not a directory */
        }
        r->cur_root = 0;
        r->cur_cluster = r->scan.found.cluster;
        r->cur_lba = fat_lba_for_cluster(mnt, r->scan.found.cluster);
        r->cur_sectors = mnt->sectors_per_cluster;
        if (r->cur_lba == 0 || r->cur_sectors == 0) {
            FAT_CO_DONE(r);
        }
    }

    FAT_CO_END(r);
}

fat_r_t fat_resolve_parent_dir(fat_resolve_parent_ctx_t* p, fat_block_t* blk,
                               const fat_mount_t* mnt) {
    uint32_t i;

    FAT_CO_BEGIN(p);

    p->found.valid = 0;
    if (!p->path ||
        fat_root_origin(mnt, &p->cur_root, &p->cur_cluster, &p->cur_lba, &p->cur_sectors) != 0) {
        FAT_CO_DONE(p);
    }
    p->pos = 0;

    if (p->path[0] != '\0' && p->path[0] != '/' && p->source == mnt->cwd_source && !mnt->cwd_root &&
        mnt->dir_lba != 0) {
        p->cur_root = 0;
        p->cur_cluster = mnt->cwd_cluster;
        p->cur_lba = mnt->dir_lba;
        p->cur_sectors = mnt->dir_sectors;
    }

    for (;;) {
        p->comp_rc = fat_path_next_component(p->path, &p->pos, p->component, sizeof(p->component));
        if (p->comp_rc <= 0) {
            FAT_CO_DONE(p);
        }
        if (p->component[0] == '.' && p->component[1] == '\0') {
            if (!fat_path_has_more(p->path, p->pos)) {
                FAT_CO_DONE(p);
            }
            continue;
        }
        if (p->component[0] == '.' && p->component[1] == '.' && p->component[2] == '\0') {
            if (fat_root_origin(mnt, &p->cur_root, &p->cur_cluster, &p->cur_lba, &p->cur_sectors) !=
                0) {
                FAT_CO_DONE(p);
            }
            if (!fat_path_has_more(p->path, p->pos)) {
                FAT_CO_DONE(p);
            }
            continue;
        }

        if (!fat_path_has_more(p->path, p->pos)) {
            /* Leaf component: this dir is the parent. */
            i = 0;
            while (p->component[i] && i + 1 < sizeof(p->name)) {
                p->name[i] = p->component[i];
                i++;
            }
            if (p->component[i] != '\0') {
                FAT_CO_DONE(p); /* leaf name too long */
            }
            p->name[i] = '\0';
            p->found.valid = 1;
            p->found.dir_lba = p->cur_lba;
            p->found.dir_sector = p->cur_sectors;             /* span, per header contract */
            p->found.attr = (uint8_t)(p->cur_root ? 1u : 0u); /* bit0: parent-is-root */
            p->found.cluster = p->cur_cluster;
            FAT_CO_DONE(p);
        }

        p->scan.cont = 0;
        p->scan.target = p->component;
        p->scan.dir_lba = p->cur_lba;
        p->scan.dir_sectors = p->cur_sectors;
        p->scan.entry_limit = fat_dir_entry_limit(mnt, p->cur_root, p->cur_sectors);
        p->scan.cur_cluster = p->cur_cluster;
        p->scan.cur_root = p->cur_root;
        FAT_CO_AWAIT(p, fat_find_in_dir(&p->scan, blk, mnt));
        if (!p->scan.found.valid) {
            FAT_CO_DONE(p);
        }
        if (!(p->scan.found.attr & 0x10) || p->scan.found.cluster < 2) {
            FAT_CO_DONE(p);
        }
        p->cur_root = 0;
        p->cur_cluster = p->scan.found.cluster;
        p->cur_lba = fat_lba_for_cluster(mnt, p->scan.found.cluster);
        p->cur_sectors = mnt->sectors_per_cluster;
        if (p->cur_lba == 0 || p->cur_sectors == 0) {
            FAT_CO_DONE(p);
        }
    }

    FAT_CO_END(p);
}

/* --- Directory mutation (create / delete / unlink / rmdir). --- */

/* Pure open-file-table check.  The reactor owns the open-file table (fat_file.h),
 * so it is passed in explicitly rather than reached from here; the unlink path
 * forwards the reactor's pool and a NULL table disables the guard. */
int fat_entry_is_open(const fat_dir_entry_info_t* entry, const fat_open_file_t* files,
                      uint32_t count) {
    uint32_t i;

    if (!entry || !files) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (!files[i].in_use) {
            continue;
        }
        if (files[i].dir_lba == entry->dir_lba && files[i].dir_sector == entry->dir_sector &&
            files[i].dir_index == entry->dir_index) {
            return 1;
        }
    }
    return 0;
}

fat_r_t fat_free_cluster_chain(fat_freechain_ctx_t* f, fat_block_t* blk, const fat_mount_t* mnt) {
    FAT_CO_BEGIN(f);

    while (f->cluster >= 2) {
        /* Resolve the successor BEFORE clearing this entry (order matters: once
         * the FAT entry is zeroed the link is gone). */
        f->chain.cont = 0;
        f->chain.cluster = f->cluster;
        FAT_CO_AWAIT(f, fat_chain_next(&f->chain, blk, mnt));
        f->next = f->chain.next;
        f->has_next = (uint8_t)(f->next != 0);

        f->ent.cont = 0;
        f->ent.cluster = f->cluster;
        f->ent.write_value = 0;
        FAT_CO_AWAIT(f, fat_fatent_write(&f->ent, blk, mnt));

        if (!f->has_next) {
            break;
        }
        f->cluster = f->next;
    }

    FAT_CO_END(f);
}

fat_r_t fat_short_name_exists_in_dir(fat_shortscan_ctx_t* s, fat_block_t* blk,
                                     const fat_mount_t* mnt) {
    uint8_t* ent;
    uint32_t entries_per_sector;
    uint32_t j;
    uint32_t matched;

    FAT_CO_BEGIN(s);

    s->result = 0;
    s->entries_left = s->entry_limit;
    s->hops = 0;

    /* Outer loop advances the directory a cluster at a time; see fat_find_in_dir
     * for the budget/hop rule, which this shares. A collision missed because it
     * sits past the first cluster would let fat_build_short_alias mint an alias
     * that already exists on the volume. */
    for (;;) {
        for (s->cur_sector = 0; s->cur_sector < s->dir_sectors && s->entries_left > 0;
             ++s->cur_sector) {
            entries_per_sector = mnt->bytes_per_sector / 32u;
            s->entries_total =
                s->entries_left < entries_per_sector ? s->entries_left : entries_per_sector;

            FAT_CO_READ(s, blk, s->dir_lba + s->cur_sector);

            for (s->scan_index = 0; s->scan_index < s->entries_total; ++s->scan_index) {
                ent = fat_block_sector(blk) + s->scan_index * 32u;
                if (ent[0] == 0x00) {
                    s->result = 0;
                    FAT_CO_DONE(s); /* end-of-directory: absent */
                }
                if (ent[0] == 0xE5 || (ent[11] & 0x0F) == 0x0F) {
                    continue;
                }
                matched = 1;
                for (j = 0; j < 11u; ++j) {
                    if (ent[j] != s->short_name[j]) {
                        matched = 0;
                        break;
                    }
                }
                if (matched) {
                    s->result = 1;
                    FAT_CO_DONE(s);
                }
            }
            s->entries_left -= s->entries_total;
        }

        if (s->cur_root) {
            break; /* root region: a single contiguous run */
        }
        if (s->hops >= fat_total_clusters(mnt)) {
            FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_CORRUPT);
        }
        s->hops++;
        s->chain.cont = 0;
        s->chain.cluster = s->cur_cluster;
        FAT_CO_AWAIT(s, fat_chain_next(&s->chain, blk, mnt));
        if (s->chain.next == 0) {
            break; /* end-of-chain */
        }
        s->cur_cluster = s->chain.next;
        s->dir_lba = fat_lba_for_cluster(mnt, s->cur_cluster);
        s->dir_sectors = mnt->sectors_per_cluster;
        if (s->dir_lba == 0 || s->dir_sectors == 0) {
            break;
        }
        s->entries_left = s->entry_limit;
    }

    s->result = 0;
    FAT_CO_END(s);
}

fat_r_t fat_find_free_dir_slots(fat_findslots_ctx_t* f, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t entries_per_sector;
    uint32_t index;
    uint8_t* ent;

    FAT_CO_BEGIN(f);

    f->result = -1;
    if (f->needed == 0) {
        FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_NO_SPACE);
    }
    entries_per_sector = mnt->bytes_per_sector / 32u;
    if (entries_per_sector == 0 || f->entry_limit == 0) {
        FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_NO_SPACE);
    }
    f->run = 0;
    f->run_start = 0;
    f->base = 0;
    f->hops = 0;
    f->grew = 0;
    f->cur_cluster = f->first_cluster;
    f->cur_lba = f->dir_lba;

    /* Outer loop advances a cluster at a time.  `entry` is CHAIN-RELATIVE --
     * the index the caller gets back and hands to fat_write_dir_entry, which
     * resolves it through fat_dir_entry_locate.  A run of free slots may
     * therefore straddle a cluster boundary, which is exactly what the old
     * flat-offset addressing could not express. */
    for (;;) {
        for (f->entry = f->base; f->entry < f->base + f->entry_limit; ++f->entry) {
            entries_per_sector = mnt->bytes_per_sector / 32u;
            f->sector = (f->entry - f->base) / entries_per_sector;
            index = (f->entry - f->base) % entries_per_sector;

            if (f->sector >= f->dir_sectors) {
                break;
            }
            /* Load the sector once, at its first entry. */
            if (index == 0) {
                FAT_CO_READ(f, blk, f->cur_lba + f->sector);
            }
            /* Recompute from ctx: `index` is a C local and the resume switch
             * jumps past its initializer after the FAT_CO_READ. */
            ent = fat_block_sector(blk) +
                  ((f->entry - f->base) % (mnt->bytes_per_sector / 32u)) * 32u;
            if (ent[0] == 0x00 || ent[0] == 0xE5) {
                if (f->run == 0) {
                    f->run_start = f->entry;
                }
                f->run++;
                if (f->run >= f->needed) {
                    f->out_entry = f->run_start;
                    f->result = 0;
                    FAT_CO_DONE(f);
                }
                continue;
            }
            f->run = 0;
        }

        if (f->root) {
            break; /* the fixed root region cannot grow */
        }
        /* Hop to the next cluster.  A run in progress is NOT reset: consecutive
         * free slots continue across the boundary. */
        if (f->hops >= fat_total_clusters(mnt)) {
            FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_CORRUPT);
        }
        f->hops++;
        f->chain.cont = 0;
        f->chain.cluster = f->cur_cluster;
        FAT_CO_AWAIT(f, fat_chain_next(&f->chain, blk, mnt));
        if (f->chain.next == 0) {
            break; /* end of chain: the directory is full */
        }
        f->cur_cluster = f->chain.next;
        f->cur_lba = fat_lba_for_cluster(mnt, f->cur_cluster);
        f->dir_sectors = mnt->sectors_per_cluster;
        if (f->cur_lba == 0) {
            FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_CORRUPT);
        }
        f->base += f->entry_limit;
    }

    /* Every cluster is full.  GROW the directory: allocate a cluster, mark it
     * end-of-chain, zero it so every slot reads free, then link it on.  The new
     * cluster is written and terminated BEFORE the link, so an interruption
     * leaves an allocated-but-unreferenced cluster rather than a directory whose
     * last cluster contains garbage. */
    if (f->root || f->grew) {
        FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_NO_SPACE);
    }
    f->findfree.cont = 0;
    FAT_CO_AWAIT(f, fat_find_free_cluster(&f->findfree, blk, mnt));
    f->grow_cluster = f->findfree.result;

    f->fatent.cont = 0;
    f->fatent.cluster = f->grow_cluster;
    f->fatent.write_value = fat_end_of_chain_marker(mnt);
    FAT_CO_AWAIT(f, fat_fatent_write(&f->fatent, blk, mnt));

    f->cur_lba = fat_lba_for_cluster(mnt, f->grow_cluster);
    if (f->cur_lba == 0) {
        FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_CORRUPT);
    }
    for (f->zero_sector = 0; f->zero_sector < mnt->sectors_per_cluster; ++f->zero_sector) {
        for (index = 0; index < mnt->bytes_per_sector; ++index) {
            fat_block_sector(blk)[index] = 0;
        }
        FAT_CO_WRITE(f, blk, f->cur_lba + f->zero_sector);
    }

    f->fatent.cont = 0;
    f->fatent.cluster = f->cur_cluster; /* the old last cluster */
    f->fatent.write_value = f->grow_cluster;
    FAT_CO_AWAIT(f, fat_fatent_write(&f->fatent, blk, mnt));

    /* The whole new cluster is free, so the run the caller needs starts at its
     * first entry.  A `needed` larger than one cluster is refused rather than
     * chained across two fresh clusters: no name in this driver needs it
     * (FAT_LFN_MAX yields at most 21 entries). */
    f->base += f->entry_limit;
    if (f->needed > f->entry_limit) {
        FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_NO_SPACE);
    }
    f->cur_cluster = f->grow_cluster;
    f->dir_sectors = mnt->sectors_per_cluster;
    f->grew = 1;
    f->out_entry = f->base;
    f->result = 0;
    FAT_CO_DONE(f);

    FAT_CO_END(f);
}

fat_r_t fat_dir_entry_locate(fat_dirloc_ctx_t* l, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t entries_per_sector;
    uint32_t entries_per_cluster;
    uint32_t within;

    FAT_CO_BEGIN(l);

    entries_per_sector = mnt->bytes_per_sector / 32u;
    if (entries_per_sector == 0 || mnt->sectors_per_cluster == 0) {
        FAT_CO_FAIL(l, blk, WASMOS_ERR_FS_CORRUPT);
    }

    if (l->root) {
        /* The fixed root region is one contiguous run, so the index addresses
         * it directly. */
        if (l->entry_index >= mnt->root_entry_count) {
            FAT_CO_FAIL(l, blk, WASMOS_ERR_FS_NO_SPACE);
        }
        l->out_lba = mnt->root_dir_lba + (l->entry_index / entries_per_sector);
        l->out_index = l->entry_index % entries_per_sector;
        FAT_CO_DONE(l);
    }

    if (l->first_cluster < 2) {
        FAT_CO_FAIL(l, blk, WASMOS_ERR_FS_CORRUPT);
    }
    entries_per_cluster = mnt->sectors_per_cluster * entries_per_sector;
    l->cluster_skip = l->entry_index / entries_per_cluster;
    within = l->entry_index % entries_per_cluster;
    l->cur_cluster = l->first_cluster;

    while (l->cluster_skip > 0) {
        l->chain.cont = 0;
        l->chain.cluster = l->cur_cluster;
        FAT_CO_AWAIT(l, fat_chain_next(&l->chain, blk, mnt));
        if (l->chain.next == 0) {
            /* The index names an entry past the end of the chain. */
            FAT_CO_FAIL(l, blk, WASMOS_ERR_FS_NO_SPACE);
        }
        l->cur_cluster = l->chain.next;
        l->cluster_skip--;
    }

    l->out_lba = fat_lba_for_cluster(mnt, l->cur_cluster) + (within / entries_per_sector);
    l->out_index = within % entries_per_sector;
    if (l->out_lba == 0) {
        FAT_CO_FAIL(l, blk, WASMOS_ERR_FS_CORRUPT);
    }
    FAT_CO_END(l);
}

fat_r_t fat_write_dir_entry(fat_writeent_ctx_t* w, fat_block_t* blk, const fat_mount_t* mnt) {
    uint8_t* ent;
    uint32_t i;

    FAT_CO_BEGIN(w);

    w->loc.cont = 0;
    w->loc.root = w->root;
    w->loc.first_cluster = w->first_cluster;
    w->loc.entry_index = w->entry_index;
    FAT_CO_AWAIT(w, fat_dir_entry_locate(&w->loc, blk, mnt));
    w->sector = w->loc.out_lba;
    w->index = w->loc.out_index;

    FAT_CO_READ(w, blk, w->sector);
    ent = fat_block_sector(blk) + w->index * 32u;
    for (i = 0; i < 32u; ++i) {
        ent[i] = w->entry[i];
    }
    FAT_CO_WRITE(w, blk, w->sector);

    FAT_CO_END(w);
}

fat_r_t fat_delete_dir_entry_chain(fat_delchain_ctx_t* d, fat_block_t* blk,
                                   const fat_mount_t* mnt) {
    uint8_t* ent;
    uint32_t i;

    FAT_CO_BEGIN(d);

    for (i = 0; i < 32u; ++i) {
        d->tombstone[i] = 0;
    }
    d->tombstone[0] = 0xE5;

    /* Tombstone the short entry itself. */
    d->wr.cont = 0;
    d->wr.dir_lba = d->dir_lba;
    d->wr.first_cluster = d->first_cluster;
    d->wr.root = d->root;
    d->wr.entry_index = d->entry_index;
    for (i = 0; i < 32u; ++i) {
        d->wr.entry[i] = d->tombstone[i];
    }
    FAT_CO_AWAIT(d, fat_write_dir_entry(&d->wr, blk, mnt));

    /* Walk backwards tombstoning the contiguous run of preceding LFN entries.
     * entries_per_sector is recomputed each iteration: the resume switch jumps
     * past a pre-loop initializer, so a cross-yield C local would be garbage. */
    while (d->entry_index > 0) {
        d->prev_index = d->entry_index - 1u;
        d->loc.cont = 0;
        d->loc.root = d->root;
        d->loc.first_cluster = d->first_cluster;
        d->loc.entry_index = d->prev_index;
        FAT_CO_AWAIT(d, fat_dir_entry_locate(&d->loc, blk, mnt));
        d->sector = d->loc.out_lba;
        d->index = d->loc.out_index;

        FAT_CO_READ(d, blk, d->sector);
        ent = fat_block_sector(blk) + d->index * 32u;
        if ((ent[11] & 0x0Fu) != 0x0Fu) {
            break; /* not an LFN entry: chain ends here */
        }
        d->wr.cont = 0;
        d->wr.dir_lba = d->dir_lba;
        d->wr.first_cluster = d->first_cluster;
        d->wr.root = d->root;
        d->wr.entry_index = d->prev_index;
        for (i = 0; i < 32u; ++i) {
            d->wr.entry[i] = d->tombstone[i];
        }
        FAT_CO_AWAIT(d, fat_write_dir_entry(&d->wr, blk, mnt));
        d->entry_index = d->prev_index;
    }

    FAT_CO_END(d);
}

fat_r_t fat_create_path_entry(fat_create_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t i;

    FAT_CO_BEGIN(c);

    c->found.valid = 0;
    if (!c->path) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_BAD_ARGS);
    }

    /* Resolve the parent directory + leaf name. */
    c->parent.cont = 0;
    c->parent.path = c->path;
    c->parent.source = c->source;
    FAT_CO_AWAIT(c, fat_resolve_parent_dir(&c->parent, blk, mnt));
    if (!c->parent.found.valid) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    c->dir_lba = c->parent.found.dir_lba;
    c->dir_sectors = c->parent.found.dir_sector; /* span, per parent contract */
    c->root = (uint8_t)(c->parent.found.attr & 1u);
    for (i = 0; i < sizeof(c->name) && c->parent.name[i]; ++i) {
        c->name[i] = c->parent.name[i];
    }
    c->name[i < sizeof(c->name) ? i : sizeof(c->name) - 1u] = '\0';
    c->entry_limit = fat_dir_entry_limit(mnt, c->root, c->dir_sectors);

    if (fat_validate_lfn_name(c->name, &c->name_len) != 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NAME);
    }

    /* Existence pre-check (reuse the read-side directory scan). */
    c->scan.cont = 0;
    c->scan.target = c->name;
    c->scan.dir_lba = c->dir_lba;
    c->scan.dir_sectors = c->dir_sectors;
    c->scan.entry_limit = c->entry_limit;
    c->scan.cur_cluster = c->parent.found.cluster;
    c->scan.cur_root = c->root;
    FAT_CO_AWAIT(c, fat_find_in_dir(&c->scan, blk, mnt));
    if (c->scan.found.valid) {
        if (c->fail_if_exists) {
            FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_EXISTS);
        }
        c->found = c->scan.found;
        FAT_CO_DONE(c);
    }

    /* Short name: exact 8.3 if possible, else search a BASE~N alias. */
    if (fat_encode_short_name(c->name, c->short_name) == 0) {
        c->exact_short = 1;
    } else {
        c->exact_short = 0;
        for (c->ordinal = 1; c->ordinal <= 9; ++c->ordinal) {
            if (fat_build_short_alias(c->name, c->ordinal, c->short_name) != 0) {
                FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NAME);
            }
            c->shortscan.cont = 0;
            c->shortscan.dir_lba = c->dir_lba;
            c->shortscan.dir_sectors = c->dir_sectors;
            c->shortscan.entry_limit = c->entry_limit;
            c->shortscan.cur_cluster = c->parent.found.cluster;
            c->shortscan.cur_root = c->root;
            for (i = 0; i < 11u; ++i) {
                c->shortscan.short_name[i] = c->short_name[i];
            }
            FAT_CO_AWAIT(c, fat_short_name_exists_in_dir(&c->shortscan, blk, mnt));
            if (!c->shortscan.result) {
                break;
            }
            if (c->ordinal == 9) {
                FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NAME);
            }
        }
    }

    c->lfn_count = 0;
    c->needed_entries = 1;
    if (!c->exact_short) {
        c->lfn_count = (c->name_len + 12u) / 13u;
        c->needed_entries = c->lfn_count + 1u;
    }

    c->findslots.cont = 0;
    c->findslots.dir_lba = c->dir_lba;
    c->findslots.dir_sectors = c->dir_sectors;
    c->findslots.entry_limit = c->entry_limit;
    c->findslots.first_cluster = c->parent.found.cluster;
    c->findslots.root = c->root;
    c->findslots.needed = c->needed_entries;
    FAT_CO_AWAIT(c, fat_find_free_dir_slots(&c->findslots, blk, mnt));
    c->slot_entry = c->findslots.out_entry;

    /* Write the LFN entries (highest ordinal first). */
    if (!c->exact_short) {
        c->checksum = fat_short_name_checksum(c->short_name);
        for (c->i = 0; c->i < c->lfn_count; ++c->i) {
            fat_fill_lfn_entry(
                c->entry, c->name, c->name_len, c->lfn_count - c->i, c->lfn_count, c->checksum);
            c->wr.cont = 0;
            c->wr.dir_lba = c->dir_lba;
            c->wr.first_cluster = c->parent.found.cluster;
            c->wr.root = c->root;
            c->wr.entry_index = c->slot_entry + c->i;
            for (i = 0; i < 32u; ++i) {
                c->wr.entry[i] = c->entry[i];
            }
            FAT_CO_AWAIT(c, fat_write_dir_entry(&c->wr, blk, mnt));
        }
        c->slot_entry += c->lfn_count;
    }

    /* Write the 8.3 short entry. */
    for (i = 0; i < 32u; ++i) {
        c->entry[i] = 0;
    }
    for (i = 0; i < 11u; ++i) {
        c->entry[i] = c->short_name[i];
    }
    c->entry[11] = c->attr;
    fat_dirent_set_cluster(c->entry, c->cluster);
    c->entry[28] = (uint8_t)(c->size & 0xFFu);
    c->entry[29] = (uint8_t)((c->size >> 8) & 0xFFu);
    c->entry[30] = (uint8_t)((c->size >> 16) & 0xFFu);
    c->entry[31] = (uint8_t)((c->size >> 24) & 0xFFu);
    c->wr.cont = 0;
    c->wr.dir_lba = c->dir_lba;
    c->wr.first_cluster = c->parent.found.cluster;
    c->wr.root = c->root;
    c->wr.entry_index = c->slot_entry;
    for (i = 0; i < 32u; ++i) {
        c->wr.entry[i] = c->entry[i];
    }
    FAT_CO_AWAIT(c, fat_write_dir_entry(&c->wr, blk, mnt));

    c->found.valid = 1;
    c->found.attr = c->attr;
    c->found.cluster = c->cluster;
    c->found.size = c->size;
    c->found.dir_lba = c->dir_lba;
    /* The physical slot comes from the locator the short-entry write just
     * resolved; deriving it from slot_entry would assume one cluster again. */
    c->found.dir_lba = c->wr.loc.out_lba;
    c->found.dir_sector = 0;
    c->found.dir_index = c->wr.loc.out_index;
    c->found.dir_entry_index = c->slot_entry;
    c->found.dir_first_cluster = c->root ? 0u : c->parent.found.cluster;
    c->found.dir_root = c->root;
    FAT_CO_END(c);
}

/* Thin wrapper for the empty-file create: fat_create_empty_file cannot be a
 * coroutine that AWAITs fat_create_path_entry on the SAME context (both switch
 * on c->cont — they would clobber each other's resume point).  Instead it fixes
 * the create inputs (attr/cluster/size/fail_if_exists = 0) once and then IS
 * fat_create_path_entry: the driver calls this exactly like the core create.
 * The input fields are only meaningful on the first step (c->cont == 0); once
 * the machine has yielded, they are already latched, so re-forcing them each
 * step is harmless and keeps the wrapper stateless. */
fat_r_t fat_create_empty_file(fat_create_ctx_t* c, fat_block_t* blk, const fat_mount_t* mnt) {
    if (c->cont == 0) {
        c->attr = 0;
        c->cluster = 0;
        c->size = 0;
        c->fail_if_exists = 0;
    }
    return fat_create_path_entry(c, blk, mnt);
}

/* Derive a directory's starting cluster from its first LBA; pure geometry. */
static int fat_dir_cluster_from_lba(const fat_mount_t* mnt, uint32_t dir_lba, uint32_t* out) {
    uint32_t first_data = fat_first_data_lba(mnt);
    uint32_t rel;

    if (!out || dir_lba < first_data || mnt->sectors_per_cluster == 0) {
        return -1;
    }
    rel = dir_lba - first_data;
    if ((rel % mnt->sectors_per_cluster) != 0) {
        return -1;
    }
    rel /= mnt->sectors_per_cluster;
    if (rel > 0xFFFDu) {
        return -1;
    }
    *out = (uint16_t)(rel + 2u);
    return 0;
}

static void fat_fill_dot_dir_entry(uint8_t entry[32], uint8_t dots, uint32_t cluster) {
    uint32_t i;

    for (i = 0; i < 32u; ++i) {
        entry[i] = 0;
    }
    entry[0] = '.';
    entry[1] = dots == 2u ? '.' : ' ';
    for (i = 2; i < 11u; ++i) {
        entry[i] = ' ';
    }
    entry[11] = 0x10;
    fat_dirent_set_cluster(entry, cluster);
}

fat_r_t fat_create_directory(fat_mkdir_ctx_t* m, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t i;

    FAT_CO_BEGIN(m);

    if (!m->path) {
        FAT_CO_FAIL(m, blk, WASMOS_ERR_FS_BAD_ARGS);
    }

    /* Resolve the parent to derive the parent cluster (for the '..' entry). */
    m->parent.cont = 0;
    m->parent.path = m->path;
    m->parent.source = m->source;
    FAT_CO_AWAIT(m, fat_resolve_parent_dir(&m->parent, blk, mnt));
    if (!m->parent.found.valid) {
        FAT_CO_FAIL(m, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    m->dir_lba = m->parent.found.dir_lba;
    m->dir_sectors = m->parent.found.dir_sector;
    m->root = (uint8_t)(m->parent.found.attr & 1u);

    m->cluster = 0;
    if (m->root) {
        /* The FAT12/16 root has no cluster number, and the specification's rule
         * for that case is the same one FAT32 needs below: '..' holds 0 when the
         * parent IS the root.  fsck_msdos reports a non-zero value here as
         * "`..' entry in <dir> has non-zero start cluster". */
        m->parent_cluster = 0;
    } else if (fat_dir_cluster_from_lba(mnt, m->dir_lba, &m->parent_cluster) != 0) {
        /* TODO: assumes the parent starts on a cluster boundary; wider chains
         * need explicit parent-cluster tracking. */
        FAT_CO_FAIL(m, blk, WASMOS_ERR_FS_CORRUPT);
    }
    /* The specification requires '..' to hold 0 when the parent IS the root
     * directory.  On FAT32 the root is an ordinary cluster, so the derivation
     * above yields its real number and it has to be mapped back to 0 -- other
     * implementations reading this volume test '..' against 0, not against
     * BPB_RootClus. */
    if (mnt->fat_type == FAT_TYPE_32 && m->parent_cluster == mnt->root_cluster) {
        m->parent_cluster = 0;
    }

    /* Allocate a cluster + mark it end-of-chain. */
    m->findfree.cont = 0;
    FAT_CO_AWAIT(m, fat_find_free_cluster(&m->findfree, blk, mnt));
    m->cluster = m->findfree.result;

    m->fatent.cont = 0;
    m->fatent.cluster = m->cluster;
    m->fatent.write_value = fat_end_of_chain_marker(mnt);
    FAT_CO_AWAIT(m, fat_fatent_write(&m->fatent, blk, mnt));

    /* Initialise the new directory cluster: zero every sector, then lay down
     * '.' and '..' in the first sector. */
    m->cluster_lba = fat_lba_for_cluster(mnt, m->cluster);
    if (m->cluster_lba == 0) {
        FAT_CO_FAIL(m, blk, WASMOS_ERR_FS_CORRUPT);
    }
    for (m->sector = 0; m->sector < mnt->sectors_per_cluster; ++m->sector) {
        for (i = 0; i < mnt->bytes_per_sector; ++i) {
            fat_block_sector(blk)[i] = 0;
        }
        FAT_CO_WRITE(m, blk, m->cluster_lba + m->sector);
    }
    /* Reload the first (now-zeroed) sector and place the dot entries. */
    FAT_CO_READ(m, blk, m->cluster_lba);
    fat_fill_dot_dir_entry(m->entry, 1, m->cluster);
    for (i = 0; i < 32u; ++i) {
        fat_block_sector(blk)[i] = m->entry[i];
    }
    fat_fill_dot_dir_entry(m->entry, 2, m->parent_cluster);
    for (i = 0; i < 32u; ++i) {
        fat_block_sector(blk)[32u + i] = m->entry[i];
    }
    FAT_CO_WRITE(m, blk, m->cluster_lba);

    /* Create the directory's entry in its parent (fail-if-exists). */
    m->create.cont = 0;
    m->create.path = m->path;
    m->create.source = m->source;
    m->create.attr = 0x10;
    m->create.cluster = m->cluster;
    m->create.size = 0;
    m->create.fail_if_exists = 1;
    FAT_CO_AWAIT(m, fat_create_path_entry(&m->create, blk, mnt));

    FAT_CO_END(m);
}

/* Directory-empty check: 1 = empty, 0 = has a real child.  On I/O fault the sub-read propagates
 * FAT_R_ERR.  See fat_dir.h. */
fat_r_t fat_dir_is_empty_step(fat_dirempty_ctx_t* e, fat_block_t* blk, const fat_mount_t* mnt) {
    uint8_t* ent;
    char entry_name[FAT_LFN_MAX + 1u];
    uint32_t entries_per_sector;

    FAT_CO_BEGIN(e);

    e->result = 1;
    e->entries_left = fat_dir_entry_limit(mnt, 0, e->dir_sectors);
    e->hops = 0;
    fat_lfn_reset(&e->lfn);

    /* The walk must cover the WHOLE chain: rmdir treats a 1 here as permission
     * to delete the entry and free every cluster, so a child missed in a later
     * cluster is destroyed rather than merely overlooked. */
    for (;;) {
        for (e->cur_sector = 0; e->cur_sector < e->dir_sectors && e->entries_left > 0;
             ++e->cur_sector) {
            entries_per_sector = mnt->bytes_per_sector / 32u;
            e->entries_total =
                e->entries_left < entries_per_sector ? e->entries_left : entries_per_sector;

            FAT_CO_READ(e, blk, e->dir_lba + e->cur_sector);

            for (e->scan_index = 0; e->scan_index < e->entries_total; ++e->scan_index) {
                ent = fat_block_sector(blk) + e->scan_index * 32u;
                if (ent[0] == 0x00) {
                    fat_lfn_reset(&e->lfn);
                    e->result = 1;
                    FAT_CO_DONE(e);
                }
                if (ent[0] == 0xE5) {
                    fat_lfn_reset(&e->lfn);
                    continue;
                }
                if ((ent[11] & 0x0F) == 0x0F) {
                    fat_lfn_collect(&e->lfn, ent);
                    continue;
                }
                if (ent[11] & 0x08) {
                    fat_lfn_reset(&e->lfn);
                    continue;
                }
                fat_entry_name_from_dirent(&e->lfn, ent, entry_name, sizeof(entry_name));
                fat_lfn_reset(&e->lfn);
                if (fat_name_eq(entry_name, ".") || fat_name_eq(entry_name, "..")) {
                    continue;
                }
                e->result = 0; /* a real child: not empty */
                FAT_CO_DONE(e);
            }
            e->entries_left -= e->entries_total;
        }

        if (e->hops >= fat_total_clusters(mnt)) {
            FAT_CO_FAIL(e, blk, WASMOS_ERR_FS_CORRUPT);
        }
        e->hops++;
        e->chain.cont = 0;
        e->chain.cluster = e->cur_cluster;
        FAT_CO_AWAIT(e, fat_chain_next(&e->chain, blk, mnt));
        if (e->chain.next == 0) {
            break; /* end-of-chain */
        }
        e->cur_cluster = e->chain.next;
        e->dir_lba = fat_lba_for_cluster(mnt, e->cur_cluster);
        e->dir_sectors = mnt->sectors_per_cluster;
        if (e->dir_lba == 0 || e->dir_sectors == 0) {
            break;
        }
        e->entries_left = fat_dir_entry_limit(mnt, 0, e->dir_sectors);
    }

    e->result = 1;
    FAT_CO_END(e);
}

fat_r_t fat_remove_path(fat_remove_ctx_t* r, fat_block_t* blk, const fat_mount_t* mnt,
                        const fat_open_file_t* files, uint32_t file_count) {
    FAT_CO_BEGIN(r);

    if (!r->path) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_BAD_ARGS);
    }

    /* Resolve the target entry. */
    r->resolve.cont = 0;
    r->resolve.path = r->path;
    r->resolve.source = r->source;
    FAT_CO_AWAIT(r, fat_resolve_path(&r->resolve, blk, mnt));
    if (!r->resolve.found.valid) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    r->entry = r->resolve.found;

    if (r->is_rmdir) {
        /* rmdir: must be a directory with a real cluster. */
        if ((r->entry.attr & 0x10) == 0 || r->entry.cluster < 2) {
            FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_NOT_DIR);
        }
        /* Refuse to remove the endpoint's current working directory. */
        if (mnt->cwd_source == r->source && !mnt->cwd_root &&
            mnt->dir_lba == fat_lba_for_cluster(mnt, r->entry.cluster)) {
            FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_BUSY);
        }
        /* Must be empty. */
        r->empty.cont = 0;
        r->empty.dir_lba = fat_lba_for_cluster(mnt, r->entry.cluster);
        r->empty.dir_sectors = mnt->sectors_per_cluster;
        r->empty.cur_cluster = r->entry.cluster;
        FAT_CO_AWAIT(r, fat_dir_is_empty_step(&r->empty, blk, mnt));
        if (r->empty.result <= 0) {
            FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_NOT_EMPTY);
        }
        /* rmdir order: delete the entry chain, THEN free the clusters. */
        r->entry_index = r->entry.dir_entry_index;
        r->delchain.cont = 0;
        r->delchain.dir_lba = r->entry.dir_lba;
        r->delchain.first_cluster = r->entry.dir_first_cluster;
        r->delchain.root = r->entry.dir_root;
        r->delchain.entry_index = r->entry_index;
        FAT_CO_AWAIT(r, fat_delete_dir_entry_chain(&r->delchain, blk, mnt));
        r->freechain.cont = 0;
        r->freechain.cluster = r->entry.cluster;
        FAT_CO_AWAIT(r, fat_free_cluster_chain(&r->freechain, blk, mnt));
        FAT_CO_DONE(r);
    }

    /* unlink: must NOT be a directory. */
    if (r->entry.attr & 0x10) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_IS_DIR);
    }
    /* Refuse to unlink a file that is currently open. */
    if (fat_entry_is_open(&r->entry, files, file_count)) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_OPEN);
    }
    /* unlink order: free the clusters, THEN delete the entry chain. */
    if (r->entry.cluster >= 2) {
        r->freechain.cont = 0;
        r->freechain.cluster = r->entry.cluster;
        FAT_CO_AWAIT(r, fat_free_cluster_chain(&r->freechain, blk, mnt));
    }
    r->entry_index = r->entry.dir_entry_index;
    r->delchain.cont = 0;
    r->delchain.dir_lba = r->entry.dir_lba;
    r->delchain.first_cluster = r->entry.dir_first_cluster;
    r->delchain.root = r->entry.dir_root;
    r->delchain.entry_index = r->entry_index;
    FAT_CO_AWAIT(r, fat_delete_dir_entry_chain(&r->delchain, blk, mnt));

    FAT_CO_END(r);
}

fat_r_t fat_rename_path(fat_rename_ctx_t* r, fat_block_t* blk, const fat_mount_t* mnt,
                        const fat_open_file_t* files, uint32_t file_count) {
    uint32_t i;

    FAT_CO_BEGIN(r);

    if (!r->old_path || !r->new_path) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_BAD_ARGS);
    }

    /* Resolve the source and LATCH its entry: everything below restages the
     * block buffer, so the entry cannot be re-read later. */
    r->resolve.cont = 0;
    r->resolve.path = r->old_path;
    r->resolve.source = r->source;
    FAT_CO_AWAIT(r, fat_resolve_path(&r->resolve, blk, mnt));
    if (!r->resolve.found.valid) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    r->entry = r->resolve.found;
    r->is_dir = (uint8_t)((r->entry.attr & 0x10) != 0 ? 1u : 0u);

    /* An open file's descriptor records where its directory entry lives, so
     * moving the entry out from under it would leave that slot pointing at a
     * tombstone. */
    if (fat_entry_is_open(&r->entry, files, file_count)) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_BUSY);
    }

    /* The destination must not exist.  Replacing it would mean freeing its
     * cluster chain here, which is a second failure mode inside an operation
     * that already has no way to roll back; POSIX rename() overwrites, this
     * does not (see docs/TASKS.md). */
    r->dest_probe.cont = 0;
    r->dest_probe.path = r->new_path;
    r->dest_probe.source = r->source;
    FAT_CO_AWAIT(r, fat_resolve_path(&r->dest_probe, blk, mnt));
    if (r->dest_probe.found.valid) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_EXISTS);
    }

    /* The source's parent, needed only to decide whether a directory's '..'
     * has to change. */
    r->old_parent.cont = 0;
    r->old_parent.path = r->old_path;
    r->old_parent.source = r->source;
    FAT_CO_AWAIT(r, fat_resolve_parent_dir(&r->old_parent, blk, mnt));
    if (!r->old_parent.found.valid) {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_NOT_FOUND);
    }

    /* Write the new name first, carrying the SAME start cluster and size: the
     * file's data is never touched by a rename. */
    r->create.cont = 0;
    r->create.path = r->new_path;
    r->create.source = r->source;
    r->create.attr = r->entry.attr;
    r->create.cluster = r->entry.cluster;
    r->create.size = r->entry.size;
    r->create.fail_if_exists = 1;
    FAT_CO_AWAIT(r, fat_create_path_entry(&r->create, blk, mnt));

    /* Now drop the old name.  Only the directory entry goes; the cluster chain
     * stays, which is the whole point. */
    r->entry_index = r->entry.dir_entry_index;
    r->delchain.cont = 0;
    r->delchain.dir_lba = r->entry.dir_lba;
    r->delchain.first_cluster = r->entry.dir_first_cluster;
    r->delchain.root = r->entry.dir_root;
    r->delchain.entry_index = r->entry_index;
    FAT_CO_AWAIT(r, fat_delete_dir_entry_chain(&r->delchain, blk, mnt));

    /* A directory that changed parents carries a stale '..'.  The convention is
     * the one fat_create_directory applies: 0 when the parent is the root, on
     * every FAT width. */
    if (!r->is_dir || r->entry.cluster < 2) {
        FAT_CO_DONE(r);
    }
    r->dotdot_cluster = r->create.parent.found.cluster;
    if ((r->create.parent.found.attr & 1u) != 0 ||
        (mnt->fat_type == FAT_TYPE_32 && r->dotdot_cluster == mnt->root_cluster)) {
        r->dotdot_cluster = 0;
    }
    if (r->dotdot_cluster == r->old_parent.found.cluster &&
        (r->create.parent.found.attr & 1u) == (r->old_parent.found.attr & 1u)) {
        FAT_CO_DONE(r); /* same parent: '..' already correct */
    }

    FAT_CO_READ(r, blk, fat_lba_for_cluster(mnt, r->entry.cluster));
    for (i = 0; i < 32u; ++i) {
        r->entry_buf[i] = fat_block_sector(blk)[32u + i];
    }
    if (r->entry_buf[0] != '.' || r->entry_buf[1] != '.') {
        FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_CORRUPT); /* second slot is not '..' */
    }
    fat_dirent_set_cluster(r->entry_buf, r->dotdot_cluster);
    for (i = 0; i < 32u; ++i) {
        fat_block_sector(blk)[32u + i] = r->entry_buf[i];
    }
    FAT_CO_WRITE(r, blk, fat_lba_for_cluster(mnt, r->entry.cluster));

    FAT_CO_END(r);
}

/* --- Directory navigation (READDIR / CHDIR). --- */

/* Stream a NUL-terminated string to the READDIR client, byte-packed 4 per
 * FS_IPC_STREAM message.  An IPC_ERR_FULL send is retried up to
 * FAT_STREAM_SEND_RETRIES (yielding between tries); any other failure, or
 * exhausting the retries, falls back to a single wasmos_console_write of the
 * whole string.  Not a coroutine: the send-retry loop spins in place rather than
 * yielding through the reactor. */
static void fat_readdir_stream(const fat_op_ctx_t* op, int32_t fs_endpoint, const char* s) {
    int32_t len;
    uint32_t pos;

    len = fat_str_len(s);
    if (len <= 0) {
        return;
    }
    if (!(op->source >= 0 && op->request_id != 0 && fs_endpoint >= 0)) {
        wasmos_console_write(addr_cast(int32_t, s), len);
        return;
    }
    pos = 0;
    while (pos < (uint32_t)len) {
        int32_t a0 = 0;
        int32_t a1 = 0;
        int32_t a2 = 0;
        int32_t a3 = 0;
        uint32_t tries;
        a0 = (int32_t)(uint8_t)s[pos++];
        if (pos < (uint32_t)len) {
            a1 = (int32_t)(uint8_t)s[pos++];
        }
        if (pos < (uint32_t)len) {
            a2 = (int32_t)(uint8_t)s[pos++];
        }
        if (pos < (uint32_t)len) {
            a3 = (int32_t)(uint8_t)s[pos++];
        }
        tries = 0;
        for (;;) {
            int32_t rc = wasmos_ipc_send(
                op->source, fs_endpoint, FS_IPC_STREAM, op->request_id, a0, a1, a2, a3);
            if (rc == 0) {
                break;
            }
            if (rc != IPC_ERR_FULL || ++tries >= FAT_STREAM_SEND_RETRIES) {
                wasmos_console_write(addr_cast(int32_t, s), len);
                return;
            }
            (void)wasmos_sched_yield();
        }
    }
}

fat_r_t fat_op_readdir(fat_op_ctx_t* op, fat_block_t* blk, const fat_mount_t* mnt,
                       int32_t fs_endpoint) {
    fat_readdir_ctx_t* s = &op->readdir;
    uint8_t* ent;
    uint32_t entries_per_sector;
    uint32_t j;

    FAT_CO_BEGIN(s);

    /* Latch the region being listed: the scan reads these, not mnt, so a CHDIR
     * that lands between this op and the next cannot shift the listing.  A cwd
     * of "root" resolves through fat_root_origin, which on FAT32 yields an
     * ordinary cluster chain rather than a fixed region. */
    if (mnt->cwd_root) {
        if (fat_root_origin(mnt, &s->cur_root, &s->cur_cluster, &s->base_lba, &s->dir_sectors) !=
            0) {
            fat_log("root listing unsupported\n");
            FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_NOT_FOUND);
        }
        s->entry_budget = fat_dir_entry_limit(mnt, s->cur_root, s->dir_sectors);
    } else {
        if (mnt->dir_lba == 0) {
            fat_log("cwd invalid\n");
            FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_NOT_FOUND);
        }
        s->cur_root = 0;
        s->base_lba = mnt->dir_lba;
        s->dir_sectors = mnt->dir_sectors;
        s->entry_budget = (mnt->dir_sectors * mnt->bytes_per_sector) / 32u;
        s->cur_cluster = mnt->cwd_cluster;
    }
    s->entries_left = s->entry_budget;
    s->hops = 0;
    fat_lfn_reset(&s->lfn);

    /* Outer loop advances the listing a cluster at a time; without it a listing
     * stopped at the first cluster and later entries never reached the client. */
    for (;;) {
        for (s->cur_sector = 0; s->cur_sector < s->dir_sectors && s->entries_left > 0;
             ++s->cur_sector) {
            entries_per_sector = mnt->bytes_per_sector / 32u;
            s->entries_total =
                s->entries_left < entries_per_sector ? s->entries_left : entries_per_sector;

            FAT_CO_READ(s, blk, s->base_lba + s->cur_sector);

            for (s->scan_index = 0; s->scan_index < s->entries_total; ++s->scan_index) {
                ent = fat_block_sector(blk) + s->scan_index * 32u;
                if (ent[0] == 0x00) {
                    fat_lfn_reset(&s->lfn);
                    FAT_CO_DONE(s); /* end-of-directory */
                }
                if (ent[0] == 0xE5) {
                    fat_lfn_reset(&s->lfn);
                    continue;
                }
                if ((ent[11] & 0x0F) == 0x0F) {
                    fat_lfn_collect(&s->lfn, ent);
                    continue;
                }
                const char* entry_name = 0;
                const int is_dir = (ent[11] & 0x10) != 0;
                if (s->lfn.valid && s->lfn.seen == s->lfn.total && s->lfn.buf[0]) {
                    fat_lfn_finalize(&s->lfn);
                    entry_name = s->lfn.buf;
                }
                if (ent[11] & 0x08) {
                    fat_lfn_reset(&s->lfn);
                    continue;
                }
                if (entry_name) {
                    fat_readdir_stream(op, fs_endpoint, entry_name);
                } else {
                    char name[12];
                    char ext[4];
                    uint32_t name_len;
                    uint32_t ext_len;
                    char nbuf[16];
                    uint32_t out;

                    for (j = 0; j < 8; ++j) {
                        name[j] = (char)ent[j];
                    }
                    name[8] = '\0';
                    for (j = 0; j < 3; ++j) {
                        ext[j] = (char)ent[8 + j];
                    }
                    ext[3] = '\0';

                    name_len = 8;
                    while (name_len > 0 && name[name_len - 1] == ' ') {
                        name_len--;
                    }
                    ext_len = 3;
                    while (ext_len > 0 && ext[ext_len - 1] == ' ') {
                        ext_len--;
                    }

                    out = 0;
                    for (j = 0; j < name_len && out + 1 < sizeof(nbuf); ++j) {
                        nbuf[out++] = name[j];
                    }
                    nbuf[out] = '\0';
                    fat_readdir_stream(op, fs_endpoint, nbuf);
                    if (ext_len > 0) {
                        fat_readdir_stream(op, fs_endpoint, ".");
                        out = 0;
                        for (j = 0; j < ext_len && out + 1 < sizeof(nbuf); ++j) {
                            nbuf[out++] = ext[j];
                        }
                        nbuf[out] = '\0';
                        fat_readdir_stream(op, fs_endpoint, nbuf);
                    }
                }
                if (is_dir) {
                    fat_readdir_stream(op, fs_endpoint, "/");
                }
                fat_readdir_stream(op, fs_endpoint, "\n");
                fat_lfn_reset(&s->lfn);
            }

            s->entries_left -= s->entries_total;
        }

        if (s->cur_root) {
            break; /* root region: a single contiguous run */
        }
        if (s->hops >= fat_total_clusters(mnt)) {
            FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_CORRUPT);
        }
        s->hops++;
        s->chain.cont = 0;
        s->chain.cluster = s->cur_cluster;
        FAT_CO_AWAIT(s, fat_chain_next(&s->chain, blk, mnt));
        if (s->chain.next == 0) {
            break; /* end-of-chain */
        }
        s->cur_cluster = s->chain.next;
        s->base_lba = fat_lba_for_cluster(mnt, s->cur_cluster);
        s->dir_sectors = mnt->sectors_per_cluster;
        if (s->base_lba == 0 || s->dir_sectors == 0) {
            break;
        }
        s->entries_left = s->entry_budget;
    }

    fat_lfn_reset(&s->lfn);
    FAT_CO_END(s);
}

/* Advance the CHDIR walk to the next real component in c->path (skipping '/'
 * runs and '.'; '..' resets the running target to the root region rather than
 * hopping to the true parent, matching fat_resolve_path).  Returns 1 with
 * c->name set, 0 at end of path, -1 on a too-long component.  Pure string work,
 * iterative so it needs no recursion / stack. */
static int fat_chdir_next_component(fat_chdir_ctx_t* c) {
    for (;;) {
        uint32_t len;

        while (c->path[c->pos] == '/') {
            c->pos++;
        }
        if (!c->path[c->pos]) {
            return 0;
        }
        len = 0;
        while (c->path[c->pos] && c->path[c->pos] != '/') {
            if (len + 1 >= sizeof(c->name)) {
                return -1;
            }
            c->name[len++] = c->path[c->pos++];
        }
        c->name[len] = '\0';
        if (c->name[0] == '.' && c->name[1] == '\0') {
            continue;
        }
        if (c->name[0] == '.' && c->name[1] == '.' && c->name[2] == '\0') {
            /* TODO: '..' resets to the root region rather than consulting the
             * on-disk '..' entry, matching fat_resolve_path; see docs/TASKS.md. */
            c->root = 1;
            c->cluster = 0;
            continue;
        }
        return 1;
    }
}

fat_r_t fat_op_chdir(fat_op_ctx_t* op, fat_block_t* blk, fat_mount_t* mnt) {
    fat_chdir_ctx_t* c = &op->chdir;
    uint32_t j;

    FAT_CO_BEGIN(c);

    /* Empty or "/"-only: reset the cwd to the mount root. */
    if (op->dir_name[0] == '\0' || (op->dir_name[0] == '/' && op->dir_name[1] == '\0')) {
        mnt->cwd_mount = VFS_MOUNT_BOOT;
        mnt->cwd_root = 1;
        mnt->cwd_cluster = 0;
        mnt->dir_lba = 0;
        mnt->dir_sectors = 0;
        mnt->cwd_source = op->source;
        FAT_CO_DONE(c);
    }

    if (fat_root_origin(mnt,
                        &c->root_probe,
                        &c->root_cluster_probe,
                        &c->root_lba_probe,
                        &c->root_sectors_probe) != 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
    }

    /* Copy the target into the working buffer and seed the running target from
     * the cwd (absolute paths restart at the root). */
    for (j = 0; j + 1 < sizeof(c->path) && op->dir_name[j]; ++j) {
        c->path[j] = op->dir_name[j];
    }
    c->path[j] = '\0';
    c->pos = 0;
    if (c->path[0] == '/') {
        c->root = c->root_probe;
        c->cluster = c->root_cluster_probe;
        c->pos = 1;
    } else {
        c->root = mnt->cwd_root;
        c->cluster = mnt->cwd_cluster;
    }

    c->next = fat_chdir_next_component(c);
    if (c->next < 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    if (c->next == 0) {
        /* Path resolved to a directory without any real component to descend. */
        mnt->cwd_mount = VFS_MOUNT_BOOT;
        mnt->cwd_root = c->root;
        mnt->cwd_cluster = c->cluster;
        if (c->root) {
            mnt->dir_lba = 0;
            mnt->dir_sectors = 0;
        } else {
            mnt->dir_lba = fat_lba_for_cluster(mnt, c->cluster);
            mnt->dir_sectors = mnt->sectors_per_cluster;
        }
        mnt->cwd_source = op->source;
        FAT_CO_DONE(c);
    }

    /* Walk the components. Each level goes through fat_find_in_dir -- the same
     * scan every other lookup uses -- rather than a second implementation of
     * one. That is what makes chdir follow a directory's cluster chain: this
     * function used to carry its own scan that stopped at the first cluster, so
     * `ls /a/b` listed entries `cd /a/b` reported as missing.
     *
     * One deliberate difference from the old scan: it skipped a name match that
     * was not a directory and kept looking, whereas this reports NOT_DIR. Two
     * entries sharing a name is a corrupt directory, and naming the reason
     * beats reporting the file as absent. */
    for (;;) {
        c->scan.cont = 0;
        c->scan.target = c->name;
        c->scan.cur_root = c->root;
        if (c->root) {
            c->scan.dir_lba = mnt->root_dir_lba;
            c->scan.dir_sectors = mnt->root_dir_sectors;
            c->scan.cur_cluster = 0;
        } else {
            c->scan.dir_lba = fat_lba_for_cluster(mnt, c->cluster);
            c->scan.dir_sectors = mnt->sectors_per_cluster;
            c->scan.cur_cluster = c->cluster;
            if (c->scan.dir_lba == 0) {
                FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
            }
        }
        c->scan.entry_limit = fat_dir_entry_limit(mnt, c->root, c->scan.dir_sectors);
        FAT_CO_AWAIT(c, fat_find_in_dir(&c->scan, blk, mnt));

        if (!c->scan.found.valid) {
            FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
        }
        if (!(c->scan.found.attr & 0x10) || c->scan.found.cluster < 2) {
            FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_DIR);
        }

        c->root = 0;
        c->cluster = c->scan.found.cluster;
        c->next = fat_chdir_next_component(c);
        if (c->next < 0) {
            FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
        }
        if (c->next == 0) {
            mnt->cwd_mount = VFS_MOUNT_BOOT;
            mnt->cwd_cluster = c->cluster;
            mnt->dir_lba = fat_lba_for_cluster(mnt, c->cluster);
            mnt->dir_sectors = mnt->sectors_per_cluster;
            mnt->cwd_root = 0;
            mnt->cwd_source = op->source;
            FAT_CO_DONE(c);
        }
    }

    FAT_CO_END(c);
}
