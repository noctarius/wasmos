/* wfs_mkfs.c - layout and emission for a WFS volume.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md. Region order follows §3.
 */
#include "wfs_mkfs.h"

#include <string.h>

#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_super.h"

#define MKFS_DEFAULT_BLOCK_SIZE 4096u
#define MKFS_DEFAULT_BYTES_PER_OBJECT 16384u
#define MKFS_MIN_JOURNAL_BLOCKS 64u
#define MKFS_MAX_JOURNAL_BLOCKS 32768u

/* One block staged at a time. The volume is emitted in ascending block order
 * and every block is written exactly once, so a sink never has to seek and this
 * buffer never has to hold more than the block being built. */
static uint8_t g_block[WFS_BLOCK_SIZE_MAX];

static uint32_t div_up(uint32_t a, uint32_t b) {
    return (a + b - 1u) / b;
}

static uint32_t round_up(uint32_t a, uint32_t m) {
    return div_up(a, m) * m;
}

/* Set bit `i` in a little-endian bitmap, least-significant bit of byte 0 first
 * (§12). */
static void bitmap_set(uint8_t* map, uint32_t i) {
    map[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

/* Bytes a directory needs for `.`, `..` and its children, packed the way a
 * directory block is: records never straddle a block boundary, so a record that
 * would not fit in the block being filled starts the next one (§10). */
static uint32_t dir_blocks_for(const wfs_mkfs_entry_t* entries, uint32_t count, uint32_t parent,
                               uint32_t block_size) {
    uint32_t usable = wfs_dir_usable_bytes(block_size);
    uint32_t used = wfs_dir_record_length(1u) + wfs_dir_record_length(2u); /* . and .. */
    uint32_t blocks = 1u;
    uint32_t i;

    for (i = 0; i < count; ++i) {
        uint32_t need;

        if (entries[i].parent != parent) {
            continue;
        }
        need = wfs_dir_record_length(entries[i].name_len);
        if (used + need > usable) {
            blocks++;
            used = 0u;
        }
        used += need;
    }
    return blocks;
}

/* A leaf name, not a path. "." and ".." are the two records every directory
 * already carries, so an entry may not claim either. */
static wasmos_error_code_t check_name(const wfs_mkfs_entry_t* e) {
    uint32_t i;

    if (!e->name || e->name_len == 0u || e->name_len > WFS_NAME_MAX) {
        return WASMOS_ERR_FS_NAME;
    }
    for (i = 0; i < e->name_len; ++i) {
        if (e->name[i] == '/' || e->name[i] == '\0') {
            return WASMOS_ERR_FS_NAME;
        }
    }
    if (e->name_len == 1u && e->name[0] == '.') {
        return WASMOS_ERR_FS_NAME;
    }
    if (e->name_len == 2u && e->name[0] == '.' && e->name[1] == '.') {
        return WASMOS_ERR_FS_NAME;
    }
    return WASMOS_ERR_NONE;
}

wasmos_error_code_t wfs_mkfs_plan(const wfs_mkfs_params_t* params, wfs_mkfs_layout_t* out) {
    wfs_mkfs_layout_t L;
    uint32_t bs;
    uint64_t blocks64;
    uint32_t objects_target;
    uint32_t metadata_end;
    uint32_t reserved_backups;
    uint32_t g;

    if (!params || !out) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    memset(&L, 0, sizeof(L));

    bs = params->block_size ? params->block_size : MKFS_DEFAULT_BLOCK_SIZE;
    if (bs != 4096u && bs != 8192u && bs != 16384u) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    L.block_size = bs;
    L.blocks_per_group = WFS_BLOCKS_PER_GROUP(bs);

    blocks64 = params->size_bytes / bs;
    if (blocks64 > 0xFFFFFFFFu) {
        return WASMOS_ERR_FS_VOLUME_TOO_LARGE;
    }
    L.total_blocks = (uint32_t)blocks64;
    if (L.total_blocks < 16u) {
        return WASMOS_ERR_FS_NO_SPACE;
    }
    L.group_count = div_up(L.total_blocks, L.blocks_per_group);

    /* Objects are provisioned by a size ratio, as no filesystem can know how
     * many a volume will want. The count is rounded up to whole object-table
     * blocks and made uniform across groups, so a group's slice of the table is
     * a plain multiple of its index. */
    {
        uint64_t t =
            params->size_bytes /
            (params->bytes_per_object ? params->bytes_per_object : MKFS_DEFAULT_BYTES_PER_OBJECT);
        if (t < WFS_OBJECT_FIRST + 1u) {
            t = WFS_OBJECT_FIRST + 1u;
        }
        objects_target = (t > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)t;
    }
    L.objects_per_group =
        round_up(div_up(objects_target, L.group_count), wfs_objects_per_block(bs));
    /* One block of object bitmap covers one group, which is the same bound the
     * block bitmap has: 8 * block_size bits. */
    if (L.objects_per_group > L.blocks_per_group) {
        L.objects_per_group = L.blocks_per_group;
    }
    L.object_table_blocks_per_group = L.objects_per_group / wfs_objects_per_block(bs);
    L.total_objects = L.objects_per_group * L.group_count;

    /* Region order is §3: superblock, group descriptors, journal, object table,
     * bitmaps, then data. Block 0 carries the boot area and the primary
     * superblock together, which is why the table starts at 1. */
    L.group_table_start = 1u;
    L.group_table_blocks = div_up(L.group_count, wfs_group_descs_per_block(bs));

    L.journal_blocks = params->journal_blocks;
    if (L.journal_blocks == 0u) {
        L.journal_blocks = L.total_blocks / 64u;
        if (L.journal_blocks < MKFS_MIN_JOURNAL_BLOCKS) {
            L.journal_blocks = MKFS_MIN_JOURNAL_BLOCKS;
        }
        if (L.journal_blocks > MKFS_MAX_JOURNAL_BLOCKS) {
            L.journal_blocks = MKFS_MAX_JOURNAL_BLOCKS;
        }
    }
    L.journal_start = L.group_table_start + L.group_table_blocks;

    L.object_table_start = L.journal_start + L.journal_blocks;
    L.object_table_blocks = L.object_table_blocks_per_group * L.group_count;

    L.bitmap_start = L.object_table_start + L.object_table_blocks;
    /* Two per group: one block bitmap, one object bitmap. */
    L.bitmap_blocks = 2u * L.group_count;

    metadata_end = L.bitmap_start + L.bitmap_blocks;
    L.first_data_block = metadata_end;
    L.root_data_block = L.first_data_block;
    L.root_blocks = 1u;
    L.used_blocks = 0u;
    L.entry_count = 0u;

    /* The root's own directory block is the first data block, so the volume
     * must hold at least one block past its metadata. */
    if (L.first_data_block + 1u > L.total_blocks) {
        return WASMOS_ERR_FS_NO_SPACE;
    }

    /* A backup superblock occupies the first block of each odd group, and the
     * allocator must never hand those out (§5). */
    reserved_backups = 0u;
    for (g = 1u; g < L.group_count; ++g) {
        if (wfs_super_group_has_backup(g)) {
            reserved_backups++;
        }
    }

    L.free_blocks = L.total_blocks - (L.first_data_block + 1u) - reserved_backups;
    L.free_objects = L.total_objects - WFS_OBJECT_FIRST;

    *out = L;
    return WASMOS_ERR_NONE;
}

/* ---- emission ------------------------------------------------------------ */

static wasmos_error_code_t emit(const wfs_mkfs_sink_t* sink, uint32_t block, uint32_t len) {
    if (sink->write_block(sink->ctx, block, g_block, len) != 0) {
        return WASMOS_ERR_FS_IO;
    }
    return WASMOS_ERR_NONE;
}

/* Lay a superblock into `dst` and seal it for `location`. */
static void build_super(uint8_t* dst, const wfs_mkfs_params_t* params, const wfs_mkfs_layout_t* L,
                        uint64_t location) {
    memset(dst, 0, WFS_SUPER_SIZE);

    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, magic), WFS_MAGIC);
    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, version), WFS_VERSION);
    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, block_size), L->block_size);
    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, blocks_per_group), L->blocks_per_group);

    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, total_blocks), L->total_blocks);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, total_objects), L->total_objects);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, free_blocks), L->free_blocks);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, free_objects), L->free_objects);

    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, root_object_id), WFS_OBJECT_ROOT);

    wfs_wr64(
        dst, (uint32_t)offsetof(struct wfs_superblock, group_table_start), L->group_table_start);
    wfs_wr64(
        dst, (uint32_t)offsetof(struct wfs_superblock, group_table_blocks), L->group_table_blocks);
    wfs_wr64(
        dst, (uint32_t)offsetof(struct wfs_superblock, object_table_start), L->object_table_start);
    wfs_wr64(dst,
             (uint32_t)offsetof(struct wfs_superblock, object_table_blocks),
             L->object_table_blocks);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, bitmap_start), L->bitmap_start);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, bitmap_blocks), L->bitmap_blocks);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, journal_start), L->journal_start);
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, journal_blocks), L->journal_blocks);

    /* A fresh volume starts at generation 1. Zero is left unused so an all-zero
     * region never compares as a plausible newest copy during a backup scan. */
    wfs_wr64(dst, (uint32_t)offsetof(struct wfs_superblock, generation), 1u);

    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, feature_compat), 0u);
    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, feature_ro_compat), 0u);
    wfs_wr32(dst,
             (uint32_t)offsetof(struct wfs_superblock, feature_incompat),
             WFS_FEATURE_INCOMPAT_EXTENTS | WFS_FEATURE_INCOMPAT_JOURNAL);

    wfs_wr32(dst, (uint32_t)offsetof(struct wfs_superblock, state), WFS_STATE_CLEAN);

    memcpy(dst + offsetof(struct wfs_superblock, uuid), params->uuid, WFS_UUID_LEN);

    wfs_wr32(dst,
             (uint32_t)offsetof(struct wfs_superblock, checksum),
             wfs_checksum_struct(params->uuid,
                                 location,
                                 dst,
                                 WFS_SUPER_SIZE,
                                 (uint32_t)offsetof(struct wfs_superblock, checksum)));
}

/* Blocks of group `g` that are already spoken for: the metadata regions that
 * overlap it, its backup superblock, every block the entries consumed, and
 * anything past the end of a partially populated final group. */
static uint32_t group_mark_used(uint8_t* map, const wfs_mkfs_layout_t* L, uint32_t g) {
    uint32_t base = g * L->blocks_per_group;
    uint32_t data_end = L->first_data_block + L->root_blocks + L->used_blocks;
    uint32_t used = 0u;
    uint32_t i;

    for (i = 0u; i < L->blocks_per_group; ++i) {
        uint32_t block = base + i;
        int taken = 0;

        if (block >= L->total_blocks) {
            /* Past the volume. Marked allocated so the allocator can never hand
             * out a block the device does not have. */
            taken = 1;
        } else if (block < data_end) {
            /* Every metadata region, the root directory, and the entries. */
            taken = 1;
        } else if (wfs_super_group_has_backup(g) && i == 0u) {
            taken = 1;
        }

        if (taken) {
            bitmap_set(map, i);
            if (block < L->total_blocks) {
                used++;
            }
        }
    }
    return used;
}

/* ---- object records ----------------------------------------------------- */

/* Lay one object record into `d`, which must be WFS_OBJECT_SIZE bytes, and seal
 * it for `object_id` (§13). */
static void build_object(uint8_t* d, const wfs_mkfs_params_t* params, uint32_t object_id,
                         uint16_t type, uint16_t flags, uint32_t mode, uint64_t size,
                         uint32_t link_count, uint32_t first_block, uint32_t block_count) {
    memset(d, 0, WFS_OBJECT_SIZE);

    wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, object_id), object_id);
    wfs_wr16(d, (uint32_t)offsetof(struct wfs_object, type), type);
    wfs_wr16(d, (uint32_t)offsetof(struct wfs_object, flags), flags);
    wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, mode), mode);
    wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, size), size);
    wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, atime), params->now_ns);
    wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), params->now_ns);
    wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), params->now_ns);
    wfs_wr64(d, (uint32_t)offsetof(struct wfs_object, btime), params->now_ns);
    wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, link_count), link_count);

    if (block_count != 0u) {
        /* Blocks are bump-allocated, so an entry's run is always contiguous and
         * one extent maps all of it. */
        uint32_t e = (uint32_t)offsetof(struct wfs_object, extents);

        wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, extent_count), 1u);
        wfs_wr64(d, e + (uint32_t)offsetof(struct wfs_extent, logical_block), 0u);
        wfs_wr64(d, e + (uint32_t)offsetof(struct wfs_extent, physical_block), first_block);
        wfs_wr32(d, e + (uint32_t)offsetof(struct wfs_extent, length), block_count);
    }

    wfs_wr32(d,
             (uint32_t)offsetof(struct wfs_object, checksum),
             wfs_checksum_struct(params->uuid,
                                 object_id,
                                 d,
                                 WFS_OBJECT_SIZE,
                                 (uint32_t)offsetof(struct wfs_object, checksum)));
}

/* ---- directory blocks --------------------------------------------------- */

/* Where a directory record's fields sit, written into `d` at `off`. */
static uint32_t put_record(uint8_t* d, uint32_t off, uint32_t object_id, const char* name,
                           uint32_t name_len, uint8_t type) {
    uint32_t rec = wfs_dir_record_length(name_len);
    uint32_t i;

    wfs_wr64(d, off, object_id);
    wfs_wr16(d, off + 8u, (uint16_t)rec);
    d[off + 10u] = (uint8_t)name_len;
    d[off + 11u] = type;
    for (i = 0; i < name_len; ++i) {
        d[off + WFS_DIR_ENTRY_HEADER + i] = (uint8_t)name[i];
    }
    return rec;
}

/* Finish a directory block: stretch its last record to meet the tail, write the
 * tail, and seal it over the whole block (§10, §13). */
static void close_dir_block(uint8_t* d, const wfs_mkfs_params_t* params, uint32_t block,
                            uint32_t block_size, uint32_t last_off) {
    uint32_t usable = wfs_dir_usable_bytes(block_size);
    uint8_t* t = d + usable;

    wfs_wr16(d, last_off + 8u, (uint16_t)(usable - last_off));

    wfs_wr64(t, (uint32_t)offsetof(struct wfs_dir_tail, object_id), 0u);
    wfs_wr16(t, (uint32_t)offsetof(struct wfs_dir_tail, record_length), WFS_DIR_TAIL_SIZE);
    t[offsetof(struct wfs_dir_tail, name_length)] = 0u;
    t[offsetof(struct wfs_dir_tail, type)] = (uint8_t)WFS_DIR_TAIL_TYPE;
    wfs_wr32(t,
             (uint32_t)offsetof(struct wfs_dir_tail, checksum),
             wfs_checksum_struct(params->uuid,
                                 block,
                                 d,
                                 block_size,
                                 usable + (uint32_t)offsetof(struct wfs_dir_tail, checksum)));
}

/* Fill the `index`-th block of `parent`'s directory data.
 *
 * Packed the same way dir_blocks_for measured it, so the two cannot disagree
 * about which record lands in which block. */
static void build_dir_block(uint8_t* d, const wfs_mkfs_params_t* params,
                            const wfs_mkfs_entry_t* entries, const wfs_mkfs_node_t* plan,
                            uint32_t count, uint32_t parent, uint32_t self_id, uint32_t parent_id,
                            uint32_t block, uint32_t index, uint32_t block_size) {
    uint32_t usable = wfs_dir_usable_bytes(block_size);
    uint32_t cur = 0u;  /* block being filled */
    uint32_t off = 0u;  /* offset within it */
    uint32_t last = 0u; /* last record placed in the block being filled */
    uint32_t i;

    memset(d, 0, block_size);

    /* Dot and dotdot lead every directory and live in its first block. */
    if (index == 0u) {
        off += put_record(d, 0u, self_id, ".", 1u, (uint8_t)WFS_TYPE_DIR);
        last = off;
        off += put_record(d, off, parent_id, "..", 2u, (uint8_t)WFS_TYPE_DIR);
    } else {
        /* Replay the packing to find where this block's records begin. */
        off = wfs_dir_record_length(1u) + wfs_dir_record_length(2u);
    }

    for (i = 0; i < count; ++i) {
        uint32_t need;

        if (entries[i].parent != parent) {
            continue;
        }
        need = wfs_dir_record_length(entries[i].name_len);
        if (off + need > usable) {
            cur++;
            off = 0u;
            if (cur > index) {
                break; /* the rest belong to a later block */
            }
        }
        if (cur == index) {
            last = off;
            (void)put_record(d,
                             off,
                             plan[i].object_id,
                             entries[i].name,
                             entries[i].name_len,
                             (uint8_t)(entries[i].is_dir ? WFS_TYPE_DIR : WFS_TYPE_FILE));
        }
        off += need;
    }

    close_dir_block(d, params, block, block_size, last);
}

/* ---- emission ----------------------------------------------------------- */

wasmos_error_code_t wfs_mkfs_format_tree(const wfs_mkfs_params_t* params,
                                         const wfs_mkfs_entry_t* entries, uint32_t count,
                                         wfs_mkfs_node_t* plan, const wfs_mkfs_sink_t* sink,
                                         wfs_mkfs_layout_t* out_layout) {
    wfs_mkfs_layout_t L;
    wasmos_error_code_t rc;
    uint32_t bs;
    uint32_t block;
    uint32_t g;
    uint32_t i;
    uint32_t next;
    uint32_t root_slot;

    if (!params || !sink || !sink->write_block) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (count != 0u && (!entries || !plan)) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    rc = wfs_mkfs_plan(params, &L);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    bs = L.block_size;
    root_slot = count;

    /* ---- plan the tree ---- */

    if (count + 1u > L.total_objects - WFS_OBJECT_FIRST + 1u) {
        return WASMOS_ERR_FS_NO_SPACE;
    }
    for (i = 0; i < count; ++i) {
        rc = check_name(&entries[i]);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        /* A parent must be a directory declared EARLIER, which is what makes one
         * pass enough and what rules out a cycle. */
        if (entries[i].parent != WFS_MKFS_ROOT) {
            if (entries[i].parent >= i || !entries[entries[i].parent].is_dir) {
                return WASMOS_ERR_FS_CORRUPT;
            }
        }
        plan[i].object_id = WFS_OBJECT_FIRST + i;
        plan[i].first_block = 0u;
        plan[i].block_count = 0u;
        plan[i].child_count = 0u;
        plan[i].inline_data = 0u;
    }
    plan[root_slot].object_id = WFS_OBJECT_ROOT;
    plan[root_slot].child_count = 0u;
    plan[root_slot].inline_data = 0u;

    for (i = 0; i < count; ++i) {
        uint32_t slot = entries[i].parent == WFS_MKFS_ROOT ? root_slot : entries[i].parent;

        plan[slot].child_count++;
    }

    /* The root's blocks come first in the data area, as the empty-volume layout
     * already promised, then every entry in array order. Blocks are handed out
     * by a bump allocator, so each entry's run is contiguous. */
    L.root_blocks = dir_blocks_for(entries, count, WFS_MKFS_ROOT, bs);
    plan[root_slot].first_block = L.first_data_block;
    plan[root_slot].block_count = L.root_blocks;
    next = L.first_data_block + L.root_blocks;

    for (i = 0; i < count; ++i) {
        if (entries[i].is_dir) {
            plan[i].block_count = dir_blocks_for(entries, count, i, bs);
        } else if (entries[i].size <= WFS_INLINE_DATA_MAX) {
            /* Small enough to live in the record itself, costing no data block
             * at all (§7). An empty file takes this path too. */
            plan[i].inline_data = 1u;
            plan[i].block_count = 0u;
        } else {
            uint64_t blocks = (entries[i].size + bs - 1u) / bs;

            if (blocks > 0xFFFFFFFFu) {
                return WASMOS_ERR_FS_NO_SPACE;
            }
            plan[i].block_count = (uint32_t)blocks;
        }
        plan[i].first_block = next;
        if (plan[i].block_count > L.total_blocks - next) {
            return WASMOS_ERR_FS_NO_SPACE;
        }
        next += plan[i].block_count;
    }
    L.used_blocks = next - (L.first_data_block + L.root_blocks);
    L.entry_count = count;

    /* A backup superblock is not allocatable, so the data area must not have
     * grown over one. */
    for (g = 1u; g < L.group_count; ++g) {
        uint32_t backup = g * L.blocks_per_group;

        if (wfs_super_group_has_backup(g) && backup < next) {
            return WASMOS_ERR_FS_NO_SPACE;
        }
    }
    if (next > L.total_blocks) {
        return WASMOS_ERR_FS_NO_SPACE;
    }

    /* ---- emit ---- */

    /* Block 0: the reserved boot area, then the primary superblock at byte
     * WFS_SUPER_OFFSET. Both live in block 0 at every permitted block size. */
    memset(g_block, 0, bs);
    build_super(g_block + WFS_SUPER_OFFSET, params, &L, 0u);
    rc = emit(sink, 0u, bs);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }

    /* Group descriptors. Each names its group's slice of the object table and
     * its two bitmap blocks; the superblock bounds the regions those slices sit
     * in (§11). */
    for (block = 0u; block < L.group_table_blocks; ++block) {
        uint32_t per_block = wfs_group_descs_per_block(bs);
        uint32_t first = block * per_block;

        memset(g_block, 0, bs);
        for (i = 0u; i < per_block && first + i < L.group_count; ++i) {
            uint32_t gi = first + i;
            uint8_t* d = g_block + (uint32_t)(i * WFS_GROUP_DESC_SIZE);
            uint32_t group_blocks = L.blocks_per_group;
            uint32_t used;

            if (gi == L.group_count - 1u) {
                group_blocks = L.total_blocks - gi * L.blocks_per_group;
            }

            wfs_wr64(d,
                     (uint32_t)offsetof(struct wfs_group_desc, block_bitmap),
                     L.bitmap_start + 2u * gi);
            wfs_wr64(d,
                     (uint32_t)offsetof(struct wfs_group_desc, object_bitmap),
                     L.bitmap_start + 2u * gi + 1u);
            wfs_wr64(d,
                     (uint32_t)offsetof(struct wfs_group_desc, object_table),
                     L.object_table_start + gi * L.object_table_blocks_per_group);

            /* Recomputed the same way the bitmap emission below marks bits, so
             * the counter and the bitmap it summarises cannot disagree. */
            {
                static uint8_t scratch[WFS_BLOCK_SIZE_MAX];
                memset(scratch, 0, bs);
                used = group_mark_used(scratch, &L, gi);
            }
            wfs_wr32(
                d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks), group_blocks - used);
            wfs_wr32(d,
                     (uint32_t)offsetof(struct wfs_group_desc, free_objects),
                     gi == 0u ? L.objects_per_group - WFS_OBJECT_FIRST - count
                              : L.objects_per_group);
            wfs_wr32(d,
                     (uint32_t)offsetof(struct wfs_group_desc, flags),
                     wfs_super_group_has_backup(gi) ? WFS_GROUP_HAS_SUPER_BACKUP : 0u);
            wfs_wr32(d,
                     (uint32_t)offsetof(struct wfs_group_desc, checksum),
                     wfs_checksum_struct(params->uuid,
                                         gi,
                                         d,
                                         WFS_GROUP_DESC_SIZE,
                                         (uint32_t)offsetof(struct wfs_group_desc, checksum)));
        }
        rc = emit(sink, L.group_table_start + block, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    /* Journal. Its first block is the journal superblock; the log behind it is
     * empty, which recovery sees as a head at the first block whose magic does
     * not validate (§21). */
    for (block = 0u; block < L.journal_blocks; ++block) {
        memset(g_block, 0, bs);
        if (block == 0u) {
            uint8_t* d = g_block;
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_super, magic), WFS_JOURNAL_MAGIC);
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_super, version), WFS_VERSION);
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_super, block_size), bs);
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_super, blocks), L.journal_blocks);
            wfs_wr64(d, (uint32_t)offsetof(struct wfs_journal_super, first_sequence), 1u);
            wfs_wr32(d, (uint32_t)offsetof(struct wfs_journal_super, first_block), 1u);
            wfs_wr32(d,
                     (uint32_t)offsetof(struct wfs_journal_super, checksum),
                     wfs_checksum_struct(params->uuid,
                                         L.journal_start,
                                         d,
                                         (uint32_t)sizeof(struct wfs_journal_super),
                                         (uint32_t)offsetof(struct wfs_journal_super, checksum)));
        }
        rc = emit(sink, L.journal_start + block, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    /* Object table. The root, then one record per entry; everything else is zero
     * and governed by the object bitmap. */
    for (block = 0u; block < L.object_table_blocks; ++block) {
        uint32_t per_block = wfs_objects_per_block(bs);
        uint32_t first_id = block * per_block;

        memset(g_block, 0, bs);
        for (i = 0; i < per_block; ++i) {
            uint32_t id = first_id + i;
            uint8_t* d = g_block + i * WFS_OBJECT_SIZE;

            if (id == WFS_OBJECT_ROOT) {
                /* Two links: the root's own "." and its "..", both naming it. */
                build_object(d,
                             params,
                             id,
                             (uint16_t)WFS_TYPE_DIR,
                             0u,
                             0755u,
                             (uint64_t)L.root_blocks * bs,
                             2u,
                             plan[root_slot].first_block,
                             plan[root_slot].block_count);
            } else if (id >= WFS_OBJECT_FIRST && id < WFS_OBJECT_FIRST + count) {
                uint32_t k = id - WFS_OBJECT_FIRST;
                uint16_t type = (uint16_t)(entries[k].is_dir ? WFS_TYPE_DIR : WFS_TYPE_FILE);
                uint16_t flags = (uint16_t)(plan[k].inline_data ? WFS_OBJ_INLINE_DATA : 0u);
                /* A directory is linked by its parent's record and by its own
                 * ".", plus one per child's "..". A file has one name. */
                uint32_t links = entries[k].is_dir ? 2u + plan[k].child_count : 1u;
                uint64_t size =
                    entries[k].is_dir ? (uint64_t)plan[k].block_count * bs : entries[k].size;

                build_object(d,
                             params,
                             id,
                             type,
                             flags,
                             entries[k].mode,
                             size,
                             links,
                             plan[k].first_block,
                             plan[k].block_count);

                if (plan[k].inline_data && entries[k].size != 0u) {
                    /* The content lives where the extents would be (§7), so it
                     * is written after the record is laid out and the record is
                     * then resealed over it. */
                    uint32_t off = (uint32_t)offsetof(struct wfs_object, extents);

                    if (!entries[k].read ||
                        entries[k].read(
                            entries[k].read_ctx, 0u, d + off, (uint32_t)entries[k].size) != 0) {
                        return WASMOS_ERR_FS_IO;
                    }
                    wfs_wr32(d, (uint32_t)offsetof(struct wfs_object, checksum), 0u);
                    wfs_wr32(d,
                             (uint32_t)offsetof(struct wfs_object, checksum),
                             wfs_checksum_struct(params->uuid,
                                                 id,
                                                 d,
                                                 WFS_OBJECT_SIZE,
                                                 (uint32_t)offsetof(struct wfs_object, checksum)));
                }
            }
        }
        rc = emit(sink, L.object_table_start + block, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    /* Bitmaps, two blocks per group. */
    for (g = 0u; g < L.group_count; ++g) {
        memset(g_block, 0, bs);
        (void)group_mark_used(g_block, &L, g);
        rc = emit(sink, L.bitmap_start + 2u * g, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }

        memset(g_block, 0, bs);
        if (g == 0u) {
            /* Ids 0..15 are reserved (§25) and the entries follow them; marking
             * both keeps an allocator from ever returning one. */
            for (i = 0u; i < WFS_OBJECT_FIRST + count; ++i) {
                bitmap_set(g_block, i);
            }
        }
        rc = emit(sink, L.bitmap_start + 2u * g + 1u, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    /* Data blocks, in ascending order: the root's directory blocks, then each
     * entry's run, then the free tail with its backup superblocks. */
    for (block = L.first_data_block; block < L.total_blocks; ++block) {
        memset(g_block, 0, bs);

        if (block < L.first_data_block + L.root_blocks) {
            build_dir_block(g_block,
                            params,
                            entries,
                            plan,
                            count,
                            WFS_MKFS_ROOT,
                            WFS_OBJECT_ROOT,
                            WFS_OBJECT_ROOT,
                            block,
                            block - L.first_data_block,
                            bs);
        } else if (block < L.first_data_block + L.root_blocks + L.used_blocks) {
            /* Which entry owns this block. Runs are contiguous and assigned in
             * array order, so a scan forward from the previous owner would do;
             * this stays simple instead, which a formatter can afford. */
            uint32_t owner = count;

            for (i = 0; i < count; ++i) {
                if (plan[i].block_count != 0u && block >= plan[i].first_block &&
                    block < plan[i].first_block + plan[i].block_count) {
                    owner = i;
                    break;
                }
            }
            if (owner == count) {
                return WASMOS_ERR_FS_CORRUPT;
            }
            if (entries[owner].is_dir) {
                uint32_t parent_id = entries[owner].parent == WFS_MKFS_ROOT
                                         ? WFS_OBJECT_ROOT
                                         : plan[entries[owner].parent].object_id;

                build_dir_block(g_block,
                                params,
                                entries,
                                plan,
                                count,
                                owner,
                                plan[owner].object_id,
                                parent_id,
                                block,
                                block - plan[owner].first_block,
                                bs);
            } else {
                uint64_t offset = (uint64_t)(block - plan[owner].first_block) * bs;
                uint64_t left = entries[owner].size - offset;
                uint32_t len = left > bs ? bs : (uint32_t)left;

                if (!entries[owner].read ||
                    entries[owner].read(entries[owner].read_ctx, offset, g_block, len) != 0) {
                    return WASMOS_ERR_FS_IO;
                }
            }
        } else if ((block % L.blocks_per_group) == 0u &&
                   wfs_super_group_has_backup(block / L.blocks_per_group)) {
            build_super(g_block, params, &L, block);
        }

        rc = emit(sink, block, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    if (out_layout) {
        *out_layout = L;
    }
    return WASMOS_ERR_NONE;
}

wasmos_error_code_t wfs_mkfs_format(const wfs_mkfs_params_t* params, const wfs_mkfs_sink_t* sink,
                                    wfs_mkfs_layout_t* out_layout) {
    wfs_mkfs_node_t root_only;

    /* The empty volume is the populated path with no entries, so both share one
     * implementation rather than drifting apart. */
    return wfs_mkfs_format_tree(params, NULL, 0u, &root_only, sink, out_layout);
}
