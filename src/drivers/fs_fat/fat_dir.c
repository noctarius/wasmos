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
                s->found.cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
                s->found.size = (uint32_t)ent[28] | ((uint32_t)ent[29] << 8) |
                                ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
                s->found.dir_lba = s->dir_lba;
                s->found.dir_sector = s->cur_sector;
                s->found.dir_index = s->scan_index;
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
    if (!r->path || mnt->root_entry_count == 0 || mnt->root_dir_sectors == 0) {
        FAT_CO_DONE(r);
    }

    r->cur_root = 1;
    r->cur_cluster = 0;
    r->cur_lba = mnt->root_dir_lba;
    r->cur_sectors = mnt->root_dir_sectors;
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
            r->cur_root = 1;
            r->cur_cluster = 0;
            r->cur_lba = mnt->root_dir_lba;
            r->cur_sectors = mnt->root_dir_sectors;
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
    if (!p->path || mnt->root_entry_count == 0 || mnt->root_dir_sectors == 0) {
        FAT_CO_DONE(p);
    }

    p->cur_root = 1;
    p->cur_cluster = 0;
    p->cur_lba = mnt->root_dir_lba;
    p->cur_sectors = mnt->root_dir_sectors;
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
            p->cur_root = 1;
            p->cur_cluster = 0;
            p->cur_lba = mnt->root_dir_lba;
            p->cur_sectors = mnt->root_dir_sectors;
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
    f->run = 0;
    f->run_start = 0;

    for (f->entry = 0; f->entry < f->entry_limit; ++f->entry) {
        entries_per_sector = mnt->bytes_per_sector / 32u;
        f->sector = f->entry / entries_per_sector;
        index = f->entry % entries_per_sector;

        if (f->sector >= f->dir_sectors) {
            FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_NO_SPACE);
        }
        /* Load the sector once at its first entry (index 0). */
        if (index == 0) {
            FAT_CO_READ(f, blk, f->dir_lba + f->sector);
        }
        /* Recompute the in-sector index from ctx: `index` is a C local and the
         * resume switch jumps past its initializer after the FAT_CO_READ. */
        ent = fat_block_sector(blk) + (f->entry % (mnt->bytes_per_sector / 32u)) * 32u;
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

    FAT_CO_FAIL(f, blk, WASMOS_ERR_FS_NO_SPACE);
    FAT_CO_END(f);
}

fat_r_t fat_write_dir_entry(fat_writeent_ctx_t* w, fat_block_t* blk, const fat_mount_t* mnt) {
    uint32_t entries_per_sector;
    uint8_t* ent;
    uint32_t i;

    FAT_CO_BEGIN(w);

    entries_per_sector = mnt->bytes_per_sector / 32u;
    w->sector = w->entry_index / entries_per_sector;
    w->index = w->entry_index % entries_per_sector;

    FAT_CO_READ(w, blk, w->dir_lba + w->sector);
    ent = fat_block_sector(blk) + w->index * 32u;
    for (i = 0; i < 32u; ++i) {
        ent[i] = w->entry[i];
    }
    FAT_CO_WRITE(w, blk, w->dir_lba + w->sector);

    FAT_CO_END(w);
}

fat_r_t fat_delete_dir_entry_chain(fat_delchain_ctx_t* d, fat_block_t* blk,
                                   const fat_mount_t* mnt) {
    uint32_t entries_per_sector;
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
    d->wr.entry_index = d->entry_index;
    for (i = 0; i < 32u; ++i) {
        d->wr.entry[i] = d->tombstone[i];
    }
    FAT_CO_AWAIT(d, fat_write_dir_entry(&d->wr, blk, mnt));

    /* Walk backwards tombstoning the contiguous run of preceding LFN entries.
     * entries_per_sector is recomputed each iteration: the resume switch jumps
     * past a pre-loop initializer, so a cross-yield C local would be garbage. */
    while (d->entry_index > 0) {
        entries_per_sector = mnt->bytes_per_sector / 32u;
        d->prev_index = d->entry_index - 1u;
        d->sector = d->prev_index / entries_per_sector;
        d->index = d->prev_index % entries_per_sector;

        FAT_CO_READ(d, blk, d->dir_lba + d->sector);
        ent = fat_block_sector(blk) + d->index * 32u;
        if ((ent[11] & 0x0Fu) != 0x0Fu) {
            break; /* not an LFN entry: chain ends here */
        }
        d->wr.cont = 0;
        d->wr.dir_lba = d->dir_lba;
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
    c->entry[26] = (uint8_t)(c->cluster & 0xFFu);
    c->entry[27] = (uint8_t)((c->cluster >> 8) & 0xFFu);
    c->entry[28] = (uint8_t)(c->size & 0xFFu);
    c->entry[29] = (uint8_t)((c->size >> 8) & 0xFFu);
    c->entry[30] = (uint8_t)((c->size >> 16) & 0xFFu);
    c->entry[31] = (uint8_t)((c->size >> 24) & 0xFFu);
    c->wr.cont = 0;
    c->wr.dir_lba = c->dir_lba;
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
    c->found.dir_sector = c->slot_entry / (mnt->bytes_per_sector / 32u);
    c->found.dir_index = c->slot_entry % (mnt->bytes_per_sector / 32u);
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
static int fat_dir_cluster_from_lba(const fat_mount_t* mnt, uint32_t dir_lba, uint16_t* out) {
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

static void fat_fill_dot_dir_entry(uint8_t entry[32], uint8_t dots, uint16_t cluster) {
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
    entry[26] = (uint8_t)(cluster & 0xFFu);
    entry[27] = (uint8_t)((cluster >> 8) & 0xFFu);
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
        /* TODO: FAT12/16 root has no real cluster number; self-reference in '..'
         * (vvfat-compatible) until explicit root-parent handling lands. */
        m->parent_cluster = m->cluster;
    } else if (fat_dir_cluster_from_lba(mnt, m->dir_lba, &m->parent_cluster) != 0) {
        /* TODO: assumes the parent starts on a cluster boundary; wider chains
         * need explicit parent-cluster tracking. */
        FAT_CO_FAIL(m, blk, WASMOS_ERR_FS_CORRUPT);
    }

    /* Allocate a cluster + mark it end-of-chain. */
    m->findfree.cont = 0;
    FAT_CO_AWAIT(m, fat_find_free_cluster(&m->findfree, blk, mnt));
    m->cluster = m->findfree.result;
    if (m->root) {
        m->parent_cluster = m->cluster; /* root '..' self-reference, per above */
    }

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
 * FAT_R_ERR. */
static fat_r_t fat_dir_is_empty_step(fat_dirempty_ctx_t* e, fat_block_t* blk,
                                     const fat_mount_t* mnt) {
    uint8_t* ent;
    char entry_name[FAT_LFN_MAX + 1u];
    uint32_t entries_per_sector;

    FAT_CO_BEGIN(e);

    e->result = 1;
    e->entries_left = fat_dir_entry_limit(mnt, 0, e->dir_sectors);
    fat_lfn_reset(&e->lfn);

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
        FAT_CO_AWAIT(r, fat_dir_is_empty_step(&r->empty, blk, mnt));
        if (r->empty.result <= 0) {
            FAT_CO_FAIL(r, blk, WASMOS_ERR_FS_NOT_EMPTY);
        }
        /* rmdir order: delete the entry chain, THEN free the clusters. */
        r->entry_index = r->entry.dir_sector * (mnt->bytes_per_sector / 32u) + r->entry.dir_index;
        r->delchain.cont = 0;
        r->delchain.dir_lba = r->entry.dir_lba;
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
    r->entry_index = r->entry.dir_sector * (mnt->bytes_per_sector / 32u) + r->entry.dir_index;
    r->delchain.cont = 0;
    r->delchain.dir_lba = r->entry.dir_lba;
    r->delchain.entry_index = r->entry_index;
    FAT_CO_AWAIT(r, fat_delete_dir_entry_chain(&r->delchain, blk, mnt));

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

    /* Root region must exist; a non-root cwd must be valid. */
    if (mnt->root_entry_count == 0 || mnt->root_dir_sectors == 0) {
        fat_log("root listing unsupported\n");
        FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_NOT_FOUND);
    }
    if (!mnt->cwd_root && mnt->dir_lba == 0) {
        fat_log("cwd invalid\n");
        FAT_CO_FAIL(s, blk, WASMOS_ERR_FS_NOT_FOUND);
    }

    /* Latch the region being listed: the scan reads these, not mnt, so a CHDIR
     * that lands between this op and the next cannot shift the listing. */
    s->cur_root = mnt->cwd_root ? 1u : 0u;
    if (s->cur_root) {
        s->base_lba = mnt->root_dir_lba;
        s->dir_sectors = mnt->root_dir_sectors;
        s->entries_left = mnt->root_entry_count;
    } else {
        s->base_lba = mnt->dir_lba;
        s->dir_sectors = mnt->dir_sectors;
        s->entries_left = (mnt->dir_sectors * mnt->bytes_per_sector) / 32u;
    }
    fat_lfn_reset(&s->lfn);

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
            c->root = 1;
            c->cluster = 0;
            c->dir_lba = 0;
            c->dir_sectors = 0;
            continue;
        }
        return 1;
    }
}

/* Begin scanning the directory selected by (root, cluster): set the scan cursors
 * and read the first sector.  Returns 0, or -1 if the cluster maps to no LBA.
 * Pure setup: the read itself is issued by the caller via FAT_CO_READ. */
static int fat_chdir_begin_dir(fat_chdir_ctx_t* c, const fat_mount_t* mnt, uint8_t root,
                               uint16_t cluster) {
    if (root) {
        c->dir_lba = mnt->root_dir_lba;
        c->dir_sectors = mnt->root_dir_sectors;
        c->cur_sector = 0;
        c->entries_left = mnt->root_entry_count;
    } else {
        c->dir_lba = fat_lba_for_cluster(mnt, cluster);
        if (c->dir_lba == 0) {
            return -1;
        }
        c->dir_sectors = mnt->sectors_per_cluster;
        c->cur_sector = 0;
        c->entries_left = (c->dir_sectors * mnt->bytes_per_sector) / 32u;
    }
    fat_lfn_reset(&c->lfn);
    return 0;
}

fat_r_t fat_op_chdir(fat_op_ctx_t* op, fat_block_t* blk, fat_mount_t* mnt) {
    fat_chdir_ctx_t* c = &op->chdir;
    uint8_t* ent;
    uint32_t entries_per_sector;
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

    if (mnt->root_entry_count == 0 || mnt->root_dir_sectors == 0) {
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
        c->root = 1;
        c->cluster = 0;
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

    if (fat_chdir_begin_dir(c, mnt, c->root, c->cluster) != 0) {
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
    }

    /* Walk directories: each level scans its sectors looking for c->name; a match
     * that is a directory advances to the next component (or finishes). */
    for (;;) {
        for (; c->cur_sector < c->dir_sectors && c->entries_left > 0; ++c->cur_sector) {
            entries_per_sector = mnt->bytes_per_sector / 32u;
            c->entries_total =
                c->entries_left < entries_per_sector ? c->entries_left : entries_per_sector;

            FAT_CO_READ(c, blk, c->dir_lba + c->cur_sector);

            for (c->scan_index = 0; c->scan_index < c->entries_total; ++c->scan_index) {
                ent = fat_block_sector(blk) + c->scan_index * 32u;
                if (ent[0] == 0x00) {
                    fat_lfn_reset(&c->lfn);
                    FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND); /* end-of-dir: miss */
                }
                if (ent[0] == 0xE5) {
                    fat_lfn_reset(&c->lfn);
                    continue;
                }
                if ((ent[11] & 0x0F) == 0x0F) {
                    fat_lfn_collect(&c->lfn, ent);
                    continue;
                }
                const char* entry_name = 0;
                char entry[13];
                uint16_t cluster;
                if (c->lfn.valid && c->lfn.seen == c->lfn.total && c->lfn.buf[0]) {
                    fat_lfn_finalize(&c->lfn);
                    entry_name = c->lfn.buf;
                }
                if (!(ent[11] & 0x10)) {
                    fat_lfn_reset(&c->lfn);
                    continue; /* not a directory */
                }
                if (!entry_name) {
                    uint32_t p = 0;
                    for (j = 0; j < 8; ++j) {
                        if (ent[j] != ' ') {
                            entry[p++] = (char)ent[j];
                        }
                    }
                    entry[p] = '\0';
                    entry_name = entry;
                }
                if (!fat_name_eq(entry_name, c->name)) {
                    fat_lfn_reset(&c->lfn);
                    continue;
                }
                cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
                if (cluster < 2) {
                    fat_lfn_reset(&c->lfn);
                    FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_DIR);
                }
                c->root = 0;
                c->cluster = cluster;
                c->next = fat_chdir_next_component(c);
                if (c->next < 0) {
                    fat_lfn_reset(&c->lfn);
                    FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
                }
                if (c->next == 0) {
                    mnt->cwd_mount = VFS_MOUNT_BOOT;
                    mnt->cwd_cluster = c->cluster;
                    mnt->dir_lba = fat_lba_for_cluster(mnt, cluster);
                    mnt->dir_sectors = mnt->sectors_per_cluster;
                    mnt->cwd_root = 0;
                    mnt->cwd_source = op->source;
                    fat_lfn_reset(&c->lfn);
                    FAT_CO_DONE(c);
                }
                /* Descend into the matched subdirectory and restart the scan. */
                if (fat_chdir_begin_dir(c, mnt, 0, cluster) != 0) {
                    fat_lfn_reset(&c->lfn);
                    FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
                }
                goto rescan;
            }

            if (c->entries_left <= c->entries_total) {
                FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
            }
            c->entries_left -= c->entries_total;
        }
        /* Exhausted the directory's sectors without a match. */
        FAT_CO_FAIL(c, blk, WASMOS_ERR_FS_NOT_FOUND);
    rescan:;
    }

    FAT_CO_END(c);
}
