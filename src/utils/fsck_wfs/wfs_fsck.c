/* wfs_fsck.c - the consistency checks of docs/WFS_WASMOS_FILE_SYSTEM.md §24. */
#include "wfs_fsck.h"

#include <stdlib.h>
#include <string.h>

#include "wfs_crc32c.h"
#include "wfs_endian.h"

/* Everything one run needs to carry between its passes.
 *
 * The two bitmaps are what the walk BUILDS; the volume's own bitmaps are then
 * compared against them, never the reverse (§24: the walk over the object table
 * is what knows which blocks an object holds). `links` counts the directory
 * references seen, so a link_count can be checked against something derived
 * rather than trusted. */
typedef struct {
    const wfs_fsck_io_t* io;
    /* Set while walking a DIRECTORY's extents, so each data block the walk
     * claims is also validated as a directory block. A directory's blocks are
     * exactly the blocks its extents name, so this is the one place that knows
     * them without resolving the extent map a second time. */
    uint8_t walking_dir;
    uint32_t dir_block_index;
    wfs_fsck_log_fn log;
    void* log_user;
    wfs_fsck_report_t* rep;

    wfs_super_t sb;
    uint8_t* block_used;  /* one bit per block, built by the walk */
    uint8_t* object_used; /* one bit per object, built by the walk */
    uint32_t* links;      /* references counted per object */
    uint8_t* buf;         /* one block of scratch */
    uint8_t* buf2;        /* a second block, for reading a bitmap while walking */
} fsck_t;

static void report(fsck_t* f, const char* what, uint32_t block, uint32_t object) {
    if (f->log) {
        f->log(f->log_user, what, block, object);
    }
}

static int bit_get(const uint8_t* map, uint32_t i) {
    return (map[i >> 3] >> (i & 7u)) & 1u;
}

static void bit_set(uint8_t* map, uint32_t i) {
    map[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static wasmos_error_code_t read_block(fsck_t* f, uint32_t block, uint8_t* into) {
    if (block >= f->sb.total_blocks) {
        return WASMOS_ERR_FS_CORRUPT;
    }
    return f->io->read_block(f->io->user, block, into, f->sb.block_size);
}

/* Claim `count` blocks from `first` for the walk's picture of the volume.
 *
 * Claiming a block twice is how a cross-link is detected -- two objects naming
 * the same block, or an object naming metadata -- and it is structural damage,
 * not a bitmap discrepancy: rebuilding the bitmap from a walk that saw the block
 * twice would produce a bitmap that agrees with a filesystem which is still
 * wrong. */
static int claim(fsck_t* f, uint32_t first, uint32_t count, const char* what, uint32_t object) {
    uint32_t i;

    if (count == 0u || first >= f->sb.total_blocks || count > f->sb.total_blocks - first) {
        f->rep->extent_errors++;
        report(f, what, first, object);
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (bit_get(f->block_used, first + i)) {
            f->rep->extent_errors++;
            report(f, "block claimed twice", first + i, object);
            return -1;
        }
        bit_set(f->block_used, first + i);
        f->rep->blocks_in_use++;
    }
    return 0;
}

/* ---- pass 1: the superblock (§5, §24) ------------------------------------ */

/* Take the primary if it validates, otherwise the highest-generation backup the
 * §5 scan reaches. A backup being used is recorded as structural damage: the
 * volume is readable, but the primary is not what it should be and this tool
 * does not rewrite it -- doing so would commit to a copy that may itself be
 * older than a write the primary was in the middle of. */
static wasmos_error_code_t load_super(fsck_t* f) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t candidate;
    uint32_t index;
    int have = 0;
    wasmos_error_code_t rc;

    /* The primary lives at byte WFS_SUPER_OFFSET of block 0 and ends well inside
     * the first WFS_BLOCK_SIZE_MIN bytes, so that is what is read -- the block
     * size is not known yet, and reading a whole maximum-sized block to reach a
     * structure at a fixed byte offset asks the transport for four times what
     * the probe needs. A guest's block buffer is 8 KiB, so the larger read was
     * simply refused. */
    rc = f->io->read_block(f->io->user, 0u, f->buf, WFS_BLOCK_SIZE_MIN);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    memcpy(image, f->buf + WFS_SUPER_OFFSET, WFS_SUPER_SIZE);
    if (wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &f->sb) == WASMOS_ERR_NONE) {
        return WASMOS_ERR_NONE;
    }

    f->rep->super_errors++;
    f->rep->used_backup_super = 1u;
    report(f, "the primary superblock did not validate", 0u, 0u);

    for (index = 0; index < WFS_SUPER_SCAN_CANDIDATES; ++index) {
        uint32_t block_size = 0;
        uint32_t group = 0;
        uint64_t offset;
        uint32_t block;

        if (!wfs_super_backup_candidate(index, &block_size, &group)) {
            break;
        }
        offset = wfs_super_backup_offset(block_size, group);
        if (offset == 0u) {
            continue;
        }
        block = (uint32_t)(offset / block_size);
        if (f->io->read_block(f->io->user, block, f->buf, block_size) != WASMOS_ERR_NONE) {
            continue;
        }
        /* A backup sits at byte 0 of its block, unlike the primary. */
        memcpy(image, f->buf, WFS_SUPER_SIZE);
        if (wfs_super_parse(image, WFS_SUPER_SIZE, block, &candidate) != WASMOS_ERR_NONE) {
            continue;
        }
        if (wfs_super_backup_prefer(&f->sb, have, &candidate)) {
            f->sb = candidate;
            have = 1;
        }
    }
    return have ? WASMOS_ERR_NONE : WASMOS_ERR_FS_BAD_MAGIC;
}

/* ---- pass 2: the regions the volume reserves to itself ------------------- */

/* Metadata is claimed before any object is walked, so an extent pointing into
 * the object table or the journal collides here and is reported as a cross-link
 * rather than silently accepted. */
static int claim_metadata(fsck_t* f) {
    int bad = 0;

    /* Block 0 carries the boot area and the primary superblock. */
    bad |= claim(f, 0u, 1u, "block 0 is not free", 0u);
    bad |=
        claim(f, f->sb.group_table_start, f->sb.group_table_blocks, "group table out of range", 0u);
    bad |= claim(
        f, f->sb.object_table_start, f->sb.object_table_blocks, "object table out of range", 0u);
    bad |= claim(f, f->sb.bitmap_start, f->sb.bitmap_blocks, "bitmaps out of range", 0u);
    if (f->sb.journal_blocks != 0u) {
        bad |= claim(f, f->sb.journal_start, f->sb.journal_blocks, "journal out of range", 0u);
    }
    return bad;
}

/* ---- pass 3: the group descriptor table (§11) ---------------------------- */

static wasmos_error_code_t check_groups(fsck_t* f, uint32_t* out_backup_blocks) {
    uint32_t per_block = wfs_group_descs_per_block(f->sb.block_size);
    uint32_t group;
    uint32_t loaded = 0xFFFFFFFFu;

    *out_backup_blocks = 0;
    for (group = 0; group < f->sb.group_count; ++group) {
        uint32_t block = f->sb.group_table_start + group / per_block;
        const uint8_t* d;
        uint32_t stored;
        uint32_t computed;

        if (block != loaded) {
            wasmos_error_code_t rc = read_block(f, block, f->buf);

            if (rc != WASMOS_ERR_NONE) {
                return rc;
            }
            loaded = block;
        }
        d = f->buf + (group % per_block) * WFS_GROUP_DESC_SIZE;
        stored = wfs_rd32(d, offsetof(struct wfs_group_desc, checksum));
        /* Seeded with the GROUP INDEX, not the block it sits in: a descriptor is
         * identified by which group it describes, so moving the table does not
         * invalidate it. Seeding with the block would reject every descriptor. */
        computed = wfs_checksum_struct(
            f->sb.uuid, group, d, WFS_GROUP_DESC_SIZE, offsetof(struct wfs_group_desc, checksum));
        if (stored != computed) {
            f->rep->group_errors++;
            report(f, "group descriptor checksum", block, group);
            continue;
        }
        /* A backup superblock occupies the first block of the groups that carry
         * one, and nothing else may claim it. */
        if ((wfs_rd32(d, offsetof(struct wfs_group_desc, flags)) & WFS_GROUP_HAS_SUPER_BACKUP) &&
            wfs_super_group_has_backup(group)) {
            uint32_t first = group * f->sb.blocks_per_group;

            if (first != 0u && claim(f, first, 1u, "backup superblock out of range", 0u) == 0) {
                (*out_backup_blocks)++;
            }
        }
    }
    return WASMOS_ERR_NONE;
}

/* ---- pass 4: extents and extent-tree nodes (§9) -------------------------- */

static int walk_extent_node(fsck_t* f, uint32_t block, uint32_t depth, uint32_t object,
                            uint64_t* logical);
static int check_dir_block(fsck_t* f, uint32_t block, uint32_t object_id, uint32_t index);

/* Claim one extent record and check its shape. `logical` carries the previous
 * record's end so the caller can require ascending, non-overlapping logical
 * coverage -- a descent takes the last index not exceeding the target, so an
 * out-of-order node silently returns the wrong extent rather than failing. */
static int check_extent(fsck_t* f, const uint8_t* rec, uint32_t object, uint64_t* logical) {
    uint64_t lb = wfs_rd64(rec, offsetof(struct wfs_extent, logical_block));
    uint64_t pb = wfs_rd64(rec, offsetof(struct wfs_extent, physical_block));
    uint32_t len = wfs_rd32(rec, offsetof(struct wfs_extent, length));

    if (len == 0u || pb >= f->sb.total_blocks || lb < *logical) {
        f->rep->extent_errors++;
        report(f, "malformed extent", (uint32_t)pb, object);
        return -1;
    }
    *logical = lb + len;
    if (claim(f, (uint32_t)pb, len, "extent out of range", object) != 0) {
        return -1;
    }
    if (f->walking_dir) {
        uint32_t i;
        int bad = 0;

        for (i = 0; i < len; ++i) {
            bad |= check_dir_block(f, (uint32_t)pb + i, object, f->dir_block_index++);
        }
        return bad;
    }
    return 0;
}

/* One interior node: claim it, verify its header, then descend each child in
 * order. Depth decreases by exactly one per level, which is what bounds the
 * recursion -- a node claiming the same depth as its parent would otherwise let
 * a cycle in the tree run forever. */
static int walk_extent_node(fsck_t* f, uint32_t block, uint32_t depth, uint32_t object,
                            uint64_t* logical) {
    uint8_t* node;
    uint32_t entries;
    uint32_t capacity;
    uint32_t want_capacity;
    uint32_t i;
    uint32_t stored;
    uint32_t computed;
    int bad = 0;

    if (depth > WFS_EXTENT_MAX_DEPTH) {
        f->rep->extent_errors++;
        report(f, "extent tree deeper than the format permits", block, object);
        return -1;
    }
    if (claim(f, block, 1u, "extent node out of range", object) != 0) {
        return -1;
    }
    /* A second scratch block per level would grow with depth, so the node is
     * read into a stack copy of the header and re-read per record instead. The
     * tree is at most WFS_EXTENT_MAX_DEPTH deep and each level reads once. */
    node = malloc(f->sb.block_size);
    if (!node) {
        return -1;
    }
    if (read_block(f, block, node) != WASMOS_ERR_NONE) {
        free(node);
        f->rep->extent_errors++;
        report(f, "extent node unreadable", block, object);
        return -1;
    }
    stored = wfs_rd32(node, offsetof(struct wfs_extent_header, checksum));
    computed = wfs_checksum_struct(
        f->sb.uuid, block, node, f->sb.block_size, offsetof(struct wfs_extent_header, checksum));
    entries = wfs_rd16(node, offsetof(struct wfs_extent_header, entries));
    capacity = wfs_rd16(node, offsetof(struct wfs_extent_header, capacity));
    want_capacity = (depth == 0u) ? wfs_extent_leaf_capacity(f->sb.block_size)
                                  : wfs_extent_interior_capacity(f->sb.block_size);

    if (wfs_rd16(node, offsetof(struct wfs_extent_header, magic)) != WFS_EXTENT_NODE_MAGIC ||
        wfs_rd16(node, offsetof(struct wfs_extent_header, depth)) != depth ||
        capacity != want_capacity || entries > capacity) {
        f->rep->extent_errors++;
        report(f, "malformed extent node header", block, object);
        free(node);
        return -1;
    }
    if (stored != computed) {
        f->rep->extent_errors++;
        report(f, "extent node checksum", block, object);
        free(node);
        return -1;
    }

    for (i = 0; i < entries; ++i) {
        if (depth == 0u) {
            const uint8_t* rec =
                node + sizeof(struct wfs_extent_header) + i * sizeof(struct wfs_extent);

            bad |= check_extent(f, rec, object, logical);
        } else {
            const uint8_t* rec =
                node + sizeof(struct wfs_extent_header) + i * sizeof(struct wfs_extent_index);
            uint64_t child = wfs_rd64(rec, offsetof(struct wfs_extent_index, child_block));

            if (child == 0u || child >= f->sb.total_blocks) {
                f->rep->extent_errors++;
                report(f, "interior record names no child", block, object);
                bad = -1;
                continue;
            }
            bad |= walk_extent_node(f, (uint32_t)child, depth - 1u, object, logical);
        }
    }
    free(node);
    return bad;
}

/* ---- pass 5: the object table (§7) --------------------------------------- */

/* Which objects EXIST is read from the object bitmap, not inferred from the
 * records. A deleted object leaves its record behind until the slot is reused,
 * so treating any record with a plausible type as live would resurrect deleted
 * files. The bitmap is authoritative for liveness; the record is authoritative
 * for what a live object holds, which is why the BLOCK bitmap can be rebuilt
 * from a walk and the OBJECT bitmap cannot. */
static wasmos_error_code_t load_object_bitmap(fsck_t* f, uint8_t* into) {
    uint32_t objects_per_block = f->sb.block_size * 8u;
    uint32_t group;
    uint32_t per_desc_block = wfs_group_descs_per_block(f->sb.block_size);
    uint32_t objects_per_group = f->sb.total_objects / f->sb.group_count;
    uint32_t loaded = 0xFFFFFFFFu;

    memset(into, 0, (f->sb.total_objects + 7u) / 8u);
    for (group = 0; group < f->sb.group_count; ++group) {
        uint32_t desc_block = f->sb.group_table_start + group / per_desc_block;
        const uint8_t* d;
        uint64_t bitmap_block;
        uint32_t first = group * objects_per_group;
        uint32_t count = objects_per_group;
        uint32_t i;

        if (desc_block != loaded) {
            wasmos_error_code_t rc = read_block(f, desc_block, f->buf);

            if (rc != WASMOS_ERR_NONE) {
                return rc;
            }
            loaded = desc_block;
        }
        d = f->buf + (group % per_desc_block) * WFS_GROUP_DESC_SIZE;
        bitmap_block = wfs_rd64(d, offsetof(struct wfs_group_desc, object_bitmap));
        if (bitmap_block >= f->sb.total_blocks) {
            f->rep->group_errors++;
            report(f, "group object bitmap out of range", group, 0u);
            continue;
        }
        if (read_block(f, (uint32_t)bitmap_block, f->buf2) != WASMOS_ERR_NONE) {
            f->rep->group_errors++;
            report(f, "group object bitmap unreadable", (uint32_t)bitmap_block, 0u);
            continue;
        }
        if (first >= f->sb.total_objects) {
            break;
        }
        if (count > f->sb.total_objects - first) {
            count = f->sb.total_objects - first;
        }
        if (count > objects_per_block) {
            count = objects_per_block;
        }
        for (i = 0; i < count; ++i) {
            if (bit_get(f->buf2, i)) {
                bit_set(into, first + i);
            }
        }
        /* The descriptor block was overwritten by the bitmap read above. */
        loaded = 0xFFFFFFFFu;
    }
    return WASMOS_ERR_NONE;
}

/* Validate one live object's record and claim everything it holds. */
static int check_object(fsck_t* f, uint32_t object_id, uint8_t* rec_out) {
    uint32_t per_block = wfs_objects_per_block(f->sb.block_size);
    uint32_t block = f->sb.object_table_start + object_id / per_block;
    const uint8_t* rec;
    uint32_t stored;
    uint32_t computed;
    uint32_t type;
    uint32_t flags;
    uint32_t inline_count;
    uint64_t logical = 0;
    uint64_t tree;
    uint32_t i;
    int bad = 0;

    if (read_block(f, block, f->buf) != WASMOS_ERR_NONE) {
        f->rep->object_errors++;
        report(f, "object table block unreadable", block, object_id);
        return -1;
    }
    rec = f->buf + (object_id % per_block) * WFS_OBJECT_SIZE;
    memcpy(rec_out, rec, WFS_OBJECT_SIZE);

    stored = wfs_rd32(rec, offsetof(struct wfs_object, checksum));
    /* Seeded with the OBJECT ID, not the block it lands in -- the same rule the
     * group descriptor follows, and for the same reason: a record is identified
     * by what it describes, so relocating the table does not invalidate it. */
    computed = wfs_checksum_struct(
        f->sb.uuid, object_id, rec, WFS_OBJECT_SIZE, offsetof(struct wfs_object, checksum));
    if (stored != computed) {
        f->rep->object_errors++;
        report(f, "object record checksum", block, object_id);
        return -1;
    }
    type = wfs_rd16(rec, offsetof(struct wfs_object, type));
    if (type < WFS_TYPE_FILE || type > WFS_TYPE_SPECIAL) {
        f->rep->object_errors++;
        report(f, "object record has no valid type", block, object_id);
        return -1;
    }
    f->rep->objects_in_use++;
    /* Set before the extents are walked, not after: a directory's data blocks
     * are exactly the blocks its extents name, so one walk both claims them and
     * validates them. Walking twice would claim every block a second time and
     * report the directory as cross-linked with itself. */
    f->walking_dir = (type == WFS_TYPE_DIR) ? 1u : 0u;
    f->dir_block_index = 0u;

    flags = wfs_rd16(rec, offsetof(struct wfs_object, flags));
    if (flags & WFS_OBJ_INLINE_DATA) {
        /* Inline content lives in the extents array, so there is nothing to
         * claim -- and a size past what the record holds is malformed. */
        if (wfs_rd64(rec, offsetof(struct wfs_object, size)) > WFS_INLINE_DATA_MAX) {
            f->rep->object_errors++;
            report(f, "inline object larger than its record", block, object_id);
            bad = -1;
        }
        return bad;
    }

    inline_count = wfs_rd32(rec, offsetof(struct wfs_object, extent_count));
    tree = wfs_rd64(rec, offsetof(struct wfs_object, extent_tree_block));
    if (tree != 0u) {
        /* A tree root's depth is its own header's; the walk validates it. */
        uint8_t* root = malloc(f->sb.block_size);
        uint32_t depth;

        if (!root) {
            return -1;
        }
        if (tree >= f->sb.total_blocks ||
            f->io->read_block(f->io->user, (uint32_t)tree, root, f->sb.block_size) !=
                WASMOS_ERR_NONE) {
            f->rep->extent_errors++;
            report(f, "extent tree root unreadable", (uint32_t)tree, object_id);
            free(root);
            return -1;
        }
        depth = wfs_rd16(root, offsetof(struct wfs_extent_header, depth));
        free(root);
        bad |= walk_extent_node(f, (uint32_t)tree, depth, object_id, &logical);
        return bad;
    }
    if (inline_count > WFS_INLINE_EXTENTS) {
        f->rep->object_errors++;
        report(f, "object claims more inline extents than the record holds", block, object_id);
        return -1;
    }
    for (i = 0; i < inline_count; ++i) {
        const uint8_t* e =
            rec + offsetof(struct wfs_object, extents) + i * sizeof(struct wfs_extent);

        bad |= check_extent(f, e, object_id, &logical);
    }
    return bad;
}

/* ---- pass 6: directory blocks (§10) -------------------------------------- */

/* Validate one directory block and count the references it makes.
 *
 * Records are strided by `record_length` and never by sizeof: the header is 12
 * bytes and a name begins at offset 12, while sizeof rounds to 16. A stride that
 * is not a multiple of 8, or shorter than WFS_DIR_RECORD_MIN, is refused rather
 * than followed -- a stride of 0 would make this scan never terminate, which is
 * the reason the minimum exists. */
static int check_dir_block(fsck_t* f, uint32_t block, uint32_t object_id, uint32_t index) {
    uint8_t* d = f->buf2;
    uint32_t usable = wfs_dir_usable_bytes(f->sb.block_size);
    uint32_t off = 0;
    uint32_t stored;
    uint32_t computed;
    int saw_dot = 0;
    int saw_dotdot = 0;
    int bad = 0;

    if (read_block(f, block, d) != WASMOS_ERR_NONE) {
        f->rep->dir_errors++;
        report(f, "directory block unreadable", block, object_id);
        return -1;
    }
    stored = wfs_rd32(d, usable + offsetof(struct wfs_dir_tail, checksum));
    computed = wfs_checksum_struct(
        f->sb.uuid, block, d, f->sb.block_size, usable + offsetof(struct wfs_dir_tail, checksum));
    if (wfs_rd16(d, usable + offsetof(struct wfs_dir_tail, record_length)) != WFS_DIR_TAIL_SIZE ||
        d[usable + offsetof(struct wfs_dir_tail, type)] != WFS_DIR_TAIL_TYPE) {
        f->rep->dir_errors++;
        report(f, "directory block has no tail", block, object_id);
        return -1;
    }
    if (stored != computed) {
        f->rep->dir_errors++;
        report(f, "directory block checksum", block, object_id);
        return -1;
    }

    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint64_t target = wfs_rd64(d, off + offsetof(struct wfs_dir_entry, object_id));
        uint32_t stride = wfs_rd16(d, off + offsetof(struct wfs_dir_entry, record_length));
        uint32_t name_len = d[off + offsetof(struct wfs_dir_entry, name_length)];

        if (stride < WFS_DIR_RECORD_MIN || (stride & 7u) != 0u || off + stride > usable) {
            f->rep->dir_errors++;
            report(f, "directory record stride walks off the block", block, object_id);
            return -1;
        }
        if (target != WFS_OBJECT_INVALID) {
            if (name_len == 0u || name_len > WFS_NAME_MAX ||
                WFS_DIR_ENTRY_HEADER + name_len > stride) {
                f->rep->dir_errors++;
                report(f, "directory record name does not fit its stride", block, object_id);
                bad = -1;
            } else if (target >= f->sb.total_objects) {
                f->rep->dir_errors++;
                report(f, "directory record names an object outside the table", block, object_id);
                bad = -1;
            } else {
                const char* name = (const char*)(d + off + WFS_DIR_ENTRY_HEADER);

                /* `.` and `..` are references like any other and are counted:
                 * a directory's link_count includes its own `.` and the `..` of
                 * every child, which is what makes the count checkable. */
                if (name_len == 1u && name[0] == '.') {
                    saw_dot = 1;
                } else if (name_len == 2u && name[0] == '.' && name[1] == '.') {
                    saw_dotdot = 1;
                }
                f->links[target]++;
            }
        }
        off += stride;
    }
    /* Only the FIRST block of a directory carries `.` and `..` (§10). */
    if (index == 0u && (!saw_dot || !saw_dotdot)) {
        f->rep->dir_errors++;
        report(f, "directory's first block lacks . or ..", block, object_id);
        bad = -1;
    }
    return bad;
}

/* ---- pass 7: the bitmaps and the counters (§24) -------------------------- */

/* Reconcile one group's block bitmap against the walk, and recompute its free
 * counters from the result.
 *
 * The direction is the rule §24 states and the reason this tool exists: the walk
 * over the object table knows which blocks are actually held, so the bitmap is
 * corrected FROM the walk and the counters FROM the bitmap. Nothing here reads a
 * counter to decide anything. */
static wasmos_error_code_t repair_group(fsck_t* f, uint32_t group, uint32_t* out_free_blocks,
                                        uint32_t* out_free_objects) {
    uint32_t per_desc_block = wfs_group_descs_per_block(f->sb.block_size);
    uint32_t desc_block = f->sb.group_table_start + group / per_desc_block;
    uint32_t first = group * f->sb.blocks_per_group;
    uint32_t count = f->sb.blocks_per_group;
    uint64_t bitmap_block;
    uint8_t* d;
    uint32_t i;
    uint32_t free_blocks = 0;
    uint32_t free_objects = 0;
    uint32_t objects_per_group;
    uint32_t first_object;
    int bitmap_changed = 0;
    wasmos_error_code_t rc;

    if (first >= f->sb.total_blocks) {
        *out_free_blocks = 0;
        *out_free_objects = 0;
        return WASMOS_ERR_NONE;
    }
    if (count > f->sb.total_blocks - first) {
        count = f->sb.total_blocks - first;
    }
    rc = read_block(f, desc_block, f->buf);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    d = f->buf + (group % per_desc_block) * WFS_GROUP_DESC_SIZE;
    bitmap_block = wfs_rd64(d, offsetof(struct wfs_group_desc, block_bitmap));
    if (bitmap_block >= f->sb.total_blocks) {
        f->rep->group_errors++;
        report(f, "group block bitmap out of range", group, 0u);
        *out_free_blocks = 0;
        *out_free_objects = 0;
        return WASMOS_ERR_NONE;
    }
    rc = read_block(f, (uint32_t)bitmap_block, f->buf2);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    for (i = 0; i < count; ++i) {
        int want = bit_get(f->block_used, first + i);

        if (bit_get(f->buf2, i) != want) {
            f->rep->bitmap_errors++;
            bitmap_changed = 1;
            if (want) {
                f->buf2[i >> 3] |= (uint8_t)(1u << (i & 7u));
            } else {
                f->buf2[i >> 3] &= (uint8_t)~(1u << (i & 7u));
            }
        }
        if (!want) {
            free_blocks++;
        }
    }
    /* Blocks past the end of the volume in the last group's bitmap must read as
     * ALLOCATED, so nothing hands them out; a zero there is a real discrepancy. */
    for (i = count; i < f->sb.blocks_per_group && i < f->sb.block_size * 8u; ++i) {
        if (!bit_get(f->buf2, i)) {
            f->rep->bitmap_errors++;
            bitmap_changed = 1;
            f->buf2[i >> 3] |= (uint8_t)(1u << (i & 7u));
        }
    }
    if (bitmap_changed && f->io->write_block) {
        rc = f->io->write_block(f->io->user, (uint32_t)bitmap_block, f->buf2, f->sb.block_size);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        f->rep->repaired++;
    }

    /* Objects are counted from the bitmap the walk did NOT rebuild: liveness
     * comes from there, so the counter is derived from it unchanged. */
    objects_per_group = f->sb.total_objects / f->sb.group_count;
    first_object = group * objects_per_group;
    if (first_object < f->sb.total_objects) {
        uint32_t n = objects_per_group;

        if (n > f->sb.total_objects - first_object) {
            n = f->sb.total_objects - first_object;
        }
        for (i = 0; i < n; ++i) {
            if (!bit_get(f->object_used, first_object + i)) {
                free_objects++;
            }
        }
    }

    if (wfs_rd32(d, offsetof(struct wfs_group_desc, free_blocks)) != free_blocks ||
        wfs_rd32(d, offsetof(struct wfs_group_desc, free_objects)) != free_objects) {
        f->rep->counter_errors++;
        if (f->io->write_block) {
            uint32_t sum;

            wfs_wr32(d, offsetof(struct wfs_group_desc, free_blocks), free_blocks);
            wfs_wr32(d, offsetof(struct wfs_group_desc, free_objects), free_objects);
            sum = wfs_checksum_struct(f->sb.uuid,
                                      group,
                                      d,
                                      WFS_GROUP_DESC_SIZE,
                                      offsetof(struct wfs_group_desc, checksum));
            wfs_wr32(d, offsetof(struct wfs_group_desc, checksum), sum);
            rc = f->io->write_block(f->io->user, desc_block, f->buf, f->sb.block_size);
            if (rc != WASMOS_ERR_NONE) {
                return rc;
            }
            f->rep->repaired++;
        }
    }
    *out_free_blocks = free_blocks;
    *out_free_objects = free_objects;
    return WASMOS_ERR_NONE;
}

/* Write the superblock's recomputed counters, and clear `state` when the run
 * found nothing it could not repair. A volume with structural damage keeps the
 * state it had: §4 makes WFS_STATE_ERROR mean "mount read-only", and clearing it
 * over damage this tool declined to touch would hand a writer a volume no one
 * checked. */
static wasmos_error_code_t finish_super(fsck_t* f, uint32_t free_blocks, uint32_t free_objects,
                                        int structural) {
    uint8_t* d;
    uint32_t sum;
    /* Block 0's first WFS_BLOCK_SIZE_MIN bytes, for the reason load_super reads
     * them: the superblock lives inside them whatever the volume's block size
     * is, and the write below puts back exactly what was read. */
    wasmos_error_code_t rc = f->io->read_block(f->io->user, 0u, f->buf, WFS_BLOCK_SIZE_MIN);

    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    d = f->buf + WFS_SUPER_OFFSET;
    f->rep->state_before = (uint8_t)wfs_rd32(d, offsetof(struct wfs_superblock, state));
    f->rep->state_after = f->rep->state_before;

    if (wfs_rd64(d, offsetof(struct wfs_superblock, free_blocks)) != free_blocks ||
        wfs_rd64(d, offsetof(struct wfs_superblock, free_objects)) != free_objects) {
        f->rep->counter_errors++;
    }
    if (!f->io->write_block) {
        return WASMOS_ERR_NONE;
    }
    wfs_wr64(d, offsetof(struct wfs_superblock, free_blocks), free_blocks);
    wfs_wr64(d, offsetof(struct wfs_superblock, free_objects), free_objects);
    wfs_wr64(d, offsetof(struct wfs_superblock, generation), f->sb.generation + 1u);
    if (!structural) {
        wfs_wr32(d, offsetof(struct wfs_superblock, state), (uint32_t)WFS_STATE_CLEAN);
        f->rep->state_after = (uint8_t)WFS_STATE_CLEAN;
        f->rep->cleared_state = 1u;
    }
    sum = wfs_checksum_struct(
        f->sb.uuid, 0u, d, WFS_SUPER_SIZE, offsetof(struct wfs_superblock, checksum));
    wfs_wr32(d, offsetof(struct wfs_superblock, checksum), sum);
    rc = f->io->write_block(f->io->user, 0u, f->buf, WFS_BLOCK_SIZE_MIN);
    if (rc == WASMOS_ERR_NONE) {
        f->rep->repaired++;
    }
    return rc;
}

/* ---- the run ------------------------------------------------------------- */

wasmos_error_code_t wfs_fsck_run(const wfs_fsck_io_t* io, wfs_fsck_log_fn log, void* log_user,
                                 wfs_fsck_report_t* out) {
    fsck_t f;
    wasmos_error_code_t rc;
    uint32_t backup_blocks = 0;
    uint32_t object_id;
    uint32_t group;
    uint64_t free_blocks_total = 0;
    uint64_t free_objects_total = 0;
    uint8_t* record = 0;
    int structural;

    if (!io || !io->read_block || !out) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    memset(&f, 0, sizeof(f));
    memset(out, 0, sizeof(*out));
    f.io = io;
    f.log = log;
    f.log_user = log_user;
    f.rep = out;

    /* The first read is block 0 at the largest permitted block size, before any
     * geometry is known; every buffer below is sized from the superblock. */
    f.buf = malloc(WFS_BLOCK_SIZE_MAX); /* resized reads share it; the largest is a block */
    if (!f.buf) {
        return WASMOS_ERR_FS_NO_SPACE;
    }
    rc = load_super(&f);
    if (rc != WASMOS_ERR_NONE) {
        free(f.buf);
        return rc;
    }

    f.buf2 = malloc(f.sb.block_size);
    f.block_used = calloc((f.sb.total_blocks + 7u) / 8u, 1u);
    f.object_used = calloc((f.sb.total_objects + 7u) / 8u, 1u);
    f.links = calloc(f.sb.total_objects, sizeof(uint32_t));
    record = malloc(WFS_OBJECT_SIZE);
    if (!f.buf2 || !f.block_used || !f.object_used || !f.links || !record) {
        rc = WASMOS_ERR_FS_NO_SPACE;
        goto done;
    }

    if (claim_metadata(&f) != 0) {
        /* The volume's own regions do not fit inside it. Nothing further can be
         * trusted, and a walk would report every object as a cross-link. */
        rc = WASMOS_ERR_FS_CORRUPT;
        goto done;
    }
    rc = check_groups(&f, &backup_blocks);
    if (rc != WASMOS_ERR_NONE) {
        goto done;
    }
    rc = load_object_bitmap(&f, f.object_used);
    if (rc != WASMOS_ERR_NONE) {
        goto done;
    }

    for (object_id = 0; object_id < f.sb.total_objects; ++object_id) {
        if (!bit_get(f.object_used, object_id)) {
            continue;
        }
        /* Ids 0..15 are reserved (§25). They are marked allocated so nothing
         * hands them out, and all but the root carry no record at all, so a walk
         * that read them would report every one as a typeless object. */
        if (object_id < WFS_OBJECT_FIRST && object_id != f.sb.root_object_id) {
            continue;
        }
        (void)check_object(&f, object_id, record);
    }

    for (group = 0; group < f.sb.group_count; ++group) {
        uint32_t gb = 0;
        uint32_t go = 0;

        rc = repair_group(&f, group, &gb, &go);
        if (rc != WASMOS_ERR_NONE) {
            goto done;
        }
        free_blocks_total += gb;
        free_objects_total += go;
    }

    /* Link counts, checked against what the directory walk counted. */
    for (object_id = 0; object_id < f.sb.total_objects; ++object_id) {
        if (!bit_get(f.object_used, object_id)) {
            continue;
        }
        /* Reserved ids (§25) are allocated so nothing hands them out, and no
         * directory names them; the root is named by its own `.` but is the tree
         * root, so neither is an orphan. */
        if (object_id < WFS_OBJECT_FIRST || object_id == f.sb.root_object_id) {
            continue;
        }
        if (f.links[object_id] == 0u) {
            f.rep->link_errors++;
            report(&f, "object is allocated but no directory names it", 0u, object_id);
        }
    }

    structural = (f.rep->super_errors | f.rep->group_errors | f.rep->object_errors |
                  f.rep->extent_errors | f.rep->dir_errors | f.rep->link_errors) != 0u;
    rc = finish_super(&f, (uint32_t)free_blocks_total, (uint32_t)free_objects_total, structural);
    if (rc == WASMOS_ERR_NONE && structural) {
        rc = WASMOS_ERR_FS_CORRUPT;
    }

done:
    free(record);
    free(f.links);
    free(f.object_used);
    free(f.block_used);
    free(f.buf2);
    free(f.buf);
    return rc;
}
