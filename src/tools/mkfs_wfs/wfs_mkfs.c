/* wfs_mkfs.c - layout and emission for a WFS volume.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md. Region order follows §3.
 */
#include "wfs_mkfs.h"

#include <string.h>

#include "wfs_crc32c.h"
#include "wfs_super.h"

#define MKFS_DEFAULT_BLOCK_SIZE 4096u
#define MKFS_DEFAULT_BYTES_PER_OBJECT 16384u
#define MKFS_MIN_JOURNAL_BLOCKS 64u
#define MKFS_MAX_JOURNAL_BLOCKS 32768u

/* One block staged at a time. The volume is emitted in ascending block order
 * and every block is written exactly once, so a sink never has to seek and this
 * buffer never has to hold more than the block being built. */
static uint8_t g_block[WFS_BLOCK_SIZE_MAX];

static void wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr64(uint8_t* p, uint32_t off, uint64_t v) {
    wr32(p, off, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p, off + 4u, (uint32_t)(v >> 32));
}

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

    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, magic), WFS_MAGIC);
    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, version), WFS_VERSION);
    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, block_size), L->block_size);
    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, blocks_per_group), L->blocks_per_group);

    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, total_blocks), L->total_blocks);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, total_objects), L->total_objects);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, free_blocks), L->free_blocks);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, free_objects), L->free_objects);

    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, root_object_id), WFS_OBJECT_ROOT);

    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, group_table_start), L->group_table_start);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, group_table_blocks), L->group_table_blocks);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, object_table_start), L->object_table_start);
    wr64(dst,
         (uint32_t)offsetof(struct wfs_superblock, object_table_blocks),
         L->object_table_blocks);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, bitmap_start), L->bitmap_start);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, bitmap_blocks), L->bitmap_blocks);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, journal_start), L->journal_start);
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, journal_blocks), L->journal_blocks);

    /* A fresh volume starts at generation 1. Zero is left unused so an all-zero
     * region never compares as a plausible newest copy during a backup scan. */
    wr64(dst, (uint32_t)offsetof(struct wfs_superblock, generation), 1u);

    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, feature_compat), 0u);
    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, feature_ro_compat), 0u);
    wr32(dst,
         (uint32_t)offsetof(struct wfs_superblock, feature_incompat),
         WFS_FEATURE_INCOMPAT_EXTENTS | WFS_FEATURE_INCOMPAT_JOURNAL);

    wr32(dst, (uint32_t)offsetof(struct wfs_superblock, state), WFS_STATE_CLEAN);

    memcpy(dst + offsetof(struct wfs_superblock, uuid), params->uuid, WFS_UUID_LEN);

    wr32(dst,
         (uint32_t)offsetof(struct wfs_superblock, checksum),
         wfs_checksum_struct(params->uuid,
                             location,
                             dst,
                             WFS_SUPER_SIZE,
                             (uint32_t)offsetof(struct wfs_superblock, checksum)));
}

/* Blocks of group `g` that are already spoken for: the metadata regions that
 * overlap it, its backup superblock, the root's data block, and anything past
 * the end of a partially populated final group. */
static uint32_t group_mark_used(uint8_t* map, const wfs_mkfs_layout_t* L, uint32_t g) {
    uint32_t base = g * L->blocks_per_group;
    uint32_t used = 0u;
    uint32_t i;

    for (i = 0u; i < L->blocks_per_group; ++i) {
        uint32_t block = base + i;
        int taken = 0;

        if (block >= L->total_blocks) {
            /* Past the volume. Marked allocated so the allocator can never
             * hand out a block the device does not have. */
            taken = 1;
        } else if (block <= L->root_data_block) {
            /* Every metadata region plus the root's directory block, which the
             * layout places first in the data area. */
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

wasmos_error_code_t wfs_mkfs_format(const wfs_mkfs_params_t* params, const wfs_mkfs_sink_t* sink,
                                    wfs_mkfs_layout_t* out_layout) {
    wfs_mkfs_layout_t L;
    wasmos_error_code_t rc;
    uint32_t bs;
    uint32_t block;
    uint32_t g;

    if (!params || !sink || !sink->write_block) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    rc = wfs_mkfs_plan(params, &L);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    bs = L.block_size;

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
        uint32_t i;

        memset(g_block, 0, bs);
        for (i = 0u; i < per_block && first + i < L.group_count; ++i) {
            uint32_t gi = first + i;
            uint8_t* d = g_block + (uint32_t)(i * WFS_GROUP_DESC_SIZE);
            uint32_t group_blocks = L.blocks_per_group;
            uint32_t used;

            if (gi == L.group_count - 1u) {
                group_blocks = L.total_blocks - gi * L.blocks_per_group;
            }

            wr64(d,
                 (uint32_t)offsetof(struct wfs_group_desc, block_bitmap),
                 L.bitmap_start + 2u * gi);
            wr64(d,
                 (uint32_t)offsetof(struct wfs_group_desc, object_bitmap),
                 L.bitmap_start + 2u * gi + 1u);
            wr64(d,
                 (uint32_t)offsetof(struct wfs_group_desc, object_table),
                 L.object_table_start + gi * L.object_table_blocks_per_group);

            /* Recomputed the same way the bitmap emission below marks bits, so
             * the counter and the bitmap it summarises cannot disagree. */
            {
                static uint8_t scratch[WFS_BLOCK_SIZE_MAX];
                memset(scratch, 0, bs);
                used = group_mark_used(scratch, &L, gi);
            }
            wr32(d, (uint32_t)offsetof(struct wfs_group_desc, free_blocks), group_blocks - used);
            wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, free_objects),
                 gi == 0u ? L.objects_per_group - WFS_OBJECT_FIRST : L.objects_per_group);
            wr32(d,
                 (uint32_t)offsetof(struct wfs_group_desc, flags),
                 wfs_super_group_has_backup(gi) ? WFS_GROUP_HAS_SUPER_BACKUP : 0u);
            wr32(d,
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
            wr32(d, (uint32_t)offsetof(struct wfs_journal_super, magic), WFS_JOURNAL_MAGIC);
            wr32(d, (uint32_t)offsetof(struct wfs_journal_super, version), WFS_VERSION);
            wr32(d, (uint32_t)offsetof(struct wfs_journal_super, block_size), bs);
            wr32(d, (uint32_t)offsetof(struct wfs_journal_super, blocks), L.journal_blocks);
            wr64(d, (uint32_t)offsetof(struct wfs_journal_super, first_sequence), 1u);
            wr32(d, (uint32_t)offsetof(struct wfs_journal_super, first_block), 1u);
            wr32(d,
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

    /* Object table. Only the root exists; every other record is zero and is
     * governed by the object bitmap. */
    for (block = 0u; block < L.object_table_blocks; ++block) {
        memset(g_block, 0, bs);
        if (block == 0u) {
            /* The root is object 1, so it is the second record of the first
               block of group 0's slice. */
            uint8_t* d = g_block + WFS_OBJECT_SIZE;

            wr64(d, (uint32_t)offsetof(struct wfs_object, object_id), WFS_OBJECT_ROOT);
            wr16(d, (uint32_t)offsetof(struct wfs_object, type), (uint16_t)WFS_TYPE_DIR);
            wr16(d, (uint32_t)offsetof(struct wfs_object, flags), 0u);
            wr32(d, (uint32_t)offsetof(struct wfs_object, mode), 0755u);
            wr32(d, (uint32_t)offsetof(struct wfs_object, uid), 0u);
            wr32(d, (uint32_t)offsetof(struct wfs_object, gid), 0u);
            wr64(d, (uint32_t)offsetof(struct wfs_object, size), bs);
            wr64(d, (uint32_t)offsetof(struct wfs_object, atime), params->now_ns);
            wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), params->now_ns);
            wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), params->now_ns);
            wr64(d, (uint32_t)offsetof(struct wfs_object, btime), params->now_ns);
            /* Two: the root's own "." and its ".." , both of which name it. */
            wr32(d, (uint32_t)offsetof(struct wfs_object, link_count), 2u);
            wr32(d, (uint32_t)offsetof(struct wfs_object, extent_count), 1u);
            {
                uint32_t e = (uint32_t)offsetof(struct wfs_object, extents);
                wr64(d, e + (uint32_t)offsetof(struct wfs_extent, logical_block), 0u);
                wr64(d,
                     e + (uint32_t)offsetof(struct wfs_extent, physical_block),
                     L.root_data_block);
                wr32(d, e + (uint32_t)offsetof(struct wfs_extent, length), 1u);
            }
            wr64(d, (uint32_t)offsetof(struct wfs_object, extent_tree_block), 0u);
            wr32(d,
                 (uint32_t)offsetof(struct wfs_object, checksum),
                 wfs_checksum_struct(params->uuid,
                                     WFS_OBJECT_ROOT,
                                     d,
                                     WFS_OBJECT_SIZE,
                                     (uint32_t)offsetof(struct wfs_object, checksum)));
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
            uint32_t i;
            /* Ids 0..15 are reserved (§25); marking them allocated is what
             * keeps an allocator from ever returning one. */
            for (i = 0u; i < WFS_OBJECT_FIRST; ++i) {
                bitmap_set(g_block, i);
            }
        }
        rc = emit(sink, L.bitmap_start + 2u * g + 1u, bs);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    /* Data blocks. The first is the root directory; the rest are free, and
     * blocks that carry a backup superblock are written as such. */
    for (block = L.first_data_block; block < L.total_blocks; ++block) {
        memset(g_block, 0, bs);

        if (block == L.root_data_block) {
            uint32_t dot = wfs_dir_record_length(1u);
            uint32_t dotdot_len = wfs_dir_usable_bytes(bs) - dot;
            uint8_t* t;

            wr64(g_block, 0u, WFS_OBJECT_ROOT);
            wr16(g_block, 8u, (uint16_t)dot);
            g_block[10] = 1u;
            g_block[11] = (uint8_t)WFS_TYPE_DIR;
            g_block[12] = '.';

            /* The last record before the tail stretches to meet it, so a scan
               of the block ends exactly where the tail begins (§10). */
            wr64(g_block, dot, WFS_OBJECT_ROOT);
            wr16(g_block, dot + 8u, (uint16_t)dotdot_len);
            g_block[dot + 10u] = 2u;
            g_block[dot + 11u] = (uint8_t)WFS_TYPE_DIR;
            g_block[dot + 12u] = '.';
            g_block[dot + 13u] = '.';

            t = g_block + wfs_dir_usable_bytes(bs);
            wr64(t, (uint32_t)offsetof(struct wfs_dir_tail, object_id), 0u);
            wr16(t, (uint32_t)offsetof(struct wfs_dir_tail, record_length), WFS_DIR_TAIL_SIZE);
            t[offsetof(struct wfs_dir_tail, name_length)] = 0u;
            t[offsetof(struct wfs_dir_tail, type)] = (uint8_t)WFS_DIR_TAIL_TYPE;
            wr32(t,
                 (uint32_t)offsetof(struct wfs_dir_tail, checksum),
                 wfs_checksum_struct(params->uuid,
                                     block,
                                     g_block,
                                     bs,
                                     wfs_dir_usable_bytes(bs) +
                                         (uint32_t)offsetof(struct wfs_dir_tail, checksum)));
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
