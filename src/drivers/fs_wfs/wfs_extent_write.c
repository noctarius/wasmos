/* wfs_extent_write.c - add one extent to an object whose inline map is full.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §9, §13.
 */
#include "wfs_extent_write.h"

#include <stddef.h>

#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_journal.h"
#include "wfs_ops.h"

/* Byte offset of leaf record `i`. */
static uint32_t leaf_slot(uint32_t i) {
    return (uint32_t)sizeof(struct wfs_extent_header) + i * (uint32_t)sizeof(struct wfs_extent);
}

static void put_extent(uint8_t* node, uint32_t i, uint64_t logical, uint64_t physical,
                       uint32_t length) {
    uint32_t at = leaf_slot(i);

    wfs_wr64(node, at + (uint32_t)offsetof(struct wfs_extent, logical_block), logical);
    wfs_wr64(node, at + (uint32_t)offsetof(struct wfs_extent, physical_block), physical);
    wfs_wr32(node, at + (uint32_t)offsetof(struct wfs_extent, length), length);
    /* `reserved` occupies what would be tail padding so the checksum covers
     * fully defined records (§9). */
    wfs_wr32(node, at + (uint32_t)offsetof(struct wfs_extent, reserved), 0u);
}

static uint64_t get_logical(const uint8_t* node, uint32_t i) {
    return wfs_rd64(node, leaf_slot(i) + (uint32_t)offsetof(struct wfs_extent, logical_block));
}

static uint64_t get_physical(const uint8_t* node, uint32_t i) {
    return wfs_rd64(node, leaf_slot(i) + (uint32_t)offsetof(struct wfs_extent, physical_block));
}

static uint32_t get_length(const uint8_t* node, uint32_t i) {
    return wfs_rd32(node, leaf_slot(i) + (uint32_t)offsetof(struct wfs_extent, length));
}

/* Seal the node: the checksum is a header field, taken over the whole block and
 * seeded with the volume uuid and the block number (§13), which is exactly what
 * wfs_extent.c validates. */
static void seal_node(const wfs_volume_t* vol, uint8_t* node, uint32_t block) {
    uint32_t at = (uint32_t)offsetof(struct wfs_extent_header, checksum);

    wfs_wr32(node, at, 0u);
    wfs_wr32(
        node, at, wfs_checksum_struct(vol->super.uuid, block, node, vol->super.block_size, at));
}

/* ---- interior nodes (§9) -------------------------------------------------- */

/* Byte offset of interior record `i`. An index names the first logical block its
 * child covers and the block that child lives in. */
static uint32_t index_slot(uint32_t i) {
    return (uint32_t)sizeof(struct wfs_extent_header) +
           i * (uint32_t)sizeof(struct wfs_extent_index);
}

static void put_index(uint8_t* node, uint32_t i, uint64_t logical, uint64_t child) {
    uint32_t at = index_slot(i);

    wfs_wr64(node, at + (uint32_t)offsetof(struct wfs_extent_index, logical_block), logical);
    wfs_wr64(node, at + (uint32_t)offsetof(struct wfs_extent_index, child_block), child);
}

static uint64_t index_logical(const uint8_t* node, uint32_t i) {
    return wfs_rd64(node,
                    index_slot(i) + (uint32_t)offsetof(struct wfs_extent_index, logical_block));
}

static uint64_t index_child(const uint8_t* node, uint32_t i) {
    return wfs_rd64(node, index_slot(i) + (uint32_t)offsetof(struct wfs_extent_index, child_block));
}

/* Records a split moves out of a full leaf, plus the one being inserted.
 *
 * A file-scope buffer rather than a local: one staged block cannot hold both
 * halves of a split at once, so the half that moves has to live somewhere across
 * the two writes, and a task's C locals do not survive its awaits. */
#define WFS_SPLIT_MAX                                                                              \
    ((WFS_BLOCK_SIZE_MAX - sizeof(struct wfs_extent_header)) / sizeof(struct wfs_extent) / 2u + 2u)
static struct wfs_extent g_split[WFS_SPLIT_MAX];

/* Write the header of a fresh interior node. Zeroed first for the same reasons
 * init_leaf is: the checksum covers the whole block and fsck reads past
 * `entries`. */
static void init_interior(const wfs_volume_t* vol, uint8_t* node, uint32_t entries) {
    uint32_t i;

    for (i = 0; i < vol->super.block_size; ++i) {
        node[i] = 0u;
    }
    wfs_wr16(node, (uint32_t)offsetof(struct wfs_extent_header, magic), WFS_EXTENT_NODE_MAGIC);
    wfs_wr16(node, (uint32_t)offsetof(struct wfs_extent_header, depth), 1u);
    wfs_wr16(node, (uint32_t)offsetof(struct wfs_extent_header, entries), (uint16_t)entries);
    wfs_wr16(node,
             (uint32_t)offsetof(struct wfs_extent_header, capacity),
             (uint16_t)wfs_extent_interior_capacity(vol->super.block_size));
    wfs_wr32(node, (uint32_t)offsetof(struct wfs_extent_header, reserved), 0u);
}

/* Write the header of a fresh leaf. The block is zeroed first: it held arbitrary
 * content, the checksum covers the whole block, and fsck reads records past
 * `entries` -- none of which tolerates leftovers. */
static void init_leaf(const wfs_volume_t* vol, uint8_t* node, uint32_t entries) {
    uint32_t i;

    for (i = 0; i < vol->super.block_size; ++i) {
        node[i] = 0u;
    }
    wfs_wr16(node, (uint32_t)offsetof(struct wfs_extent_header, magic), WFS_EXTENT_NODE_MAGIC);
    wfs_wr16(node, (uint32_t)offsetof(struct wfs_extent_header, depth), 0u);
    wfs_wr16(node, (uint32_t)offsetof(struct wfs_extent_header, entries), (uint16_t)entries);
    wfs_wr16(node,
             (uint32_t)offsetof(struct wfs_extent_header, capacity),
             (uint16_t)wfs_extent_leaf_capacity(vol->super.block_size));
    wfs_wr32(node, (uint32_t)offsetof(struct wfs_extent_header, reserved), 0u);
}

/* The six inline extents plus the new one, sorted by logical_block, written into
 * a fresh leaf. Insertion sort over seven records: the array is tiny and fixed,
 * so the simplest correct order is the right one. */
static void fill_promoted_leaf(wfs_xtadd_ctx_t* ctx, uint8_t* node) {
    struct wfs_extent set[WFS_INLINE_EXTENTS + 1u];
    uint32_t n = 0u;
    uint32_t i;
    uint32_t j;

    for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
        if (ctx->obj->extents[i].length != 0u) {
            set[n] = ctx->obj->extents[i];
            n++;
        }
    }
    set[n].logical_block = ctx->logical;
    set[n].physical_block = (uint64_t)ctx->physical;
    set[n].length = ctx->length;
    set[n].reserved = 0u;
    n++;

    for (i = 1u; i < n; ++i) {
        struct wfs_extent key = set[i];

        j = i;
        while (j > 0u && set[j - 1u].logical_block > key.logical_block) {
            set[j] = set[j - 1u];
            j--;
        }
        set[j] = key;
    }

    init_leaf(ctx->vol, node, n);
    for (i = 0; i < n; ++i) {
        put_extent(node, i, set[i].logical_block, set[i].physical_block, set[i].length);
    }
    ctx->added = n;
}

/* Whether `node` verifies as the extent-tree node numbered `block`, on the same
 * terms wfs_extent.c reads it: a node failing these is not a node. Records
 * ctx->err and returns 0 so a caller can propagate it. */
static int node_verifies(wfs_xtadd_ctx_t* ctx, const uint8_t* node, uint32_t block) {
    if (wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, magic)) !=
        WFS_EXTENT_NODE_MAGIC) {
        ctx->err = WASMOS_ERR_FS_CORRUPT;
        return 0;
    }
    if (wfs_rd32(node, (uint32_t)offsetof(struct wfs_extent_header, checksum)) !=
        wfs_checksum_struct(ctx->vol->super.uuid,
                            block,
                            node,
                            ctx->vol->super.block_size,
                            (uint32_t)offsetof(struct wfs_extent_header, checksum))) {
        ctx->err = WASMOS_ERR_FS_CHECKSUM;
        return 0;
    }
    return 1;
}

int32_t wfs_extent_add_task(void* user, uintptr_t* out_value) {
    wfs_xtadd_ctx_t* ctx = (wfs_xtadd_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint8_t* node;
    uint32_t entries;
    uint32_t capacity;
    uint32_t depth;
    uint32_t at;
    uint32_t i;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_XTADD_PC_START:
            if (!ctx->vol || !ctx->obj || ctx->length == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            /* Block 0 holds the superblock, so it is never an object's data. */
            if (ctx->physical == 0u ||
                (uint64_t)ctx->physical + ctx->length > ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }

            if (ctx->obj->extent_tree_block != 0u) {
                ctx->node_block = (uint32_t)ctx->obj->extent_tree_block;
                if (ctx->node_block >= ctx->vol->super.total_blocks) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
                }
                WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->node_block), WFS_XTADD_PC_NODE_READY);
                continue;
            }

            /* Promotion. The caller records inline while the array has room, so
             * a short map here is a caller bug, not a volume problem. */
            if (ctx->obj->extent_count != WFS_INLINE_EXTENTS) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            if (ctx->leaf_block == 0u || ctx->leaf_block >= ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            /* A fresh block is never read: it holds whatever it held before, and
             * init_leaf zeroes it. Any earlier request is settled first, so its
             * completion cannot be mistaken for this write's. */
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            ctx->node_block = ctx->leaf_block;
            node = wfs_block_data(b);
            fill_promoted_leaf(ctx, node);
            seal_node(ctx->vol, node, ctx->node_block);
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->node_block), WFS_XTADD_PC_LEAF_WRITTEN);
            continue;

        case WFS_XTADD_PC_NODE_READY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);
            if (!node_verifies(ctx, node, ctx->node_block)) {
                return (int32_t)ctx->err;
            }
            depth = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, depth));
            if (depth == 0u) {
                /* The root IS the leaf; there is no interior level yet, and a
                 * split will have to add one. */
                ctx->root_block = 0u;
                ctx->pc = WFS_XTADD_PC_LEAF_READY;
                continue;
            }
            if (depth > 1u) {
                /* TODO: a tree two interior levels deep. Nothing writes one --
                 * the root below splits leaves under a SINGLE interior root, which
                 * reaches wfs_extent_interior_capacity() leaves and so far more
                 * extents than a uint32_t block number can address -- so refusing
                 * cannot strand a volume this driver made. */
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
            }
            entries = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            capacity = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, capacity));
            if (capacity != wfs_extent_interior_capacity(ctx->vol->super.block_size) ||
                entries > capacity || entries == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }

            /* The child covering the target: the LAST index whose logical_block
             * does not exceed it, which is the same descent wfs_extent.c makes.
             * A target before every index belongs to the first child, because the
             * map has no record of anything earlier. */
            ctx->root_block = ctx->node_block;
            ctx->child = 0u;
            for (i = 0; i < entries; ++i) {
                if (index_logical(node, i) <= ctx->logical) {
                    ctx->child = i;
                } else {
                    break;
                }
            }
            ctx->node_block = (uint32_t)index_child(node, ctx->child);
            if (ctx->node_block == 0u || ctx->node_block >= ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->node_block), WFS_XTADD_PC_LEAF_READY);
            continue;

        case WFS_XTADD_PC_LEAF_READY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);
            if (!node_verifies(ctx, node, ctx->node_block)) {
                return (int32_t)ctx->err;
            }
            if (wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, depth)) != 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            entries = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            capacity = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, capacity));
            if (capacity != wfs_extent_leaf_capacity(ctx->vol->super.block_size) ||
                entries > capacity || entries == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }

            /* Where the extent belongs: records stay sorted by logical_block. */
            at = entries;
            for (i = 0; i < entries; ++i) {
                if (get_logical(node, i) > ctx->logical) {
                    at = i;
                    break;
                }
            }

            /* Coalesce with the record it would follow when the run continues it
             * both logically and physically, which keeps a sequential append to
             * ONE extent instead of a record per block. */
            if (at > 0u) {
                uint64_t plog = get_logical(node, at - 1u);
                uint64_t pphys = get_physical(node, at - 1u);
                uint32_t plen = get_length(node, at - 1u);

                if (plog + plen == ctx->logical && pphys + plen == (uint64_t)ctx->physical) {
                    put_extent(node, at - 1u, plog, pphys, plen + ctx->length);
                    seal_node(ctx->vol, node, ctx->node_block);
                    ctx->added = 0u;
                    WFS_AWAIT(ctx,
                              wfs_txn_stage_begin(ctx->vol, ctx->node_block),
                              WFS_XTADD_PC_LEAF_WRITTEN);
                    continue;
                }
            }

            if (entries == capacity) {
                /* SPLIT. The upper half moves to a fresh leaf and the record
                 * being inserted joins whichever half now covers it.
                 *
                 * The moved records go through g_split rather than straight into
                 * the new leaf, because one staged block cannot hold both halves
                 * at once: the lower half is written from the block already in
                 * hand, and the upper is rebuilt into it afterwards. */
                uint32_t half = entries / 2u;
                uint32_t k;

                /* Nothing has been modified yet, which is what makes asking for
                 * the blocks safe: the caller allocates them and calls again,
                 * and the retry descends to the same leaf and finds it as it was.
                 * Asking beats allocating speculatively on every append -- a
                 * split happens once per leaf-full of extents, and paying for two
                 * allocations on all the other appends is I/O the common path
                 * does not owe. */
                if (ctx->leaf_block == 0u || ctx->leaf_block >= ctx->vol->super.total_blocks ||
                    (ctx->root_block == 0u &&
                     (ctx->root_spare == 0u || ctx->root_spare >= ctx->vol->super.total_blocks))) {
                    WFS_FAIL(ctx, WASMOS_ERR_FS_NEED_BLOCK);
                }

                ctx->split_count = 0u;
                for (k = half; k < entries; ++k) {
                    g_split[ctx->split_count].logical_block = get_logical(node, k);
                    g_split[ctx->split_count].physical_block = get_physical(node, k);
                    g_split[ctx->split_count].length = get_length(node, k);
                    g_split[ctx->split_count].reserved = 0u;
                    ctx->split_count++;
                }
                if (at >= half) {
                    /* Into the moved half, at the position it held in the whole. */
                    uint32_t pos = at - half;

                    for (k = ctx->split_count; k > pos; --k) {
                        g_split[k] = g_split[k - 1u];
                    }
                    g_split[pos].logical_block = ctx->logical;
                    g_split[pos].physical_block = (uint64_t)ctx->physical;
                    g_split[pos].length = ctx->length;
                    g_split[pos].reserved = 0u;
                    ctx->split_count++;
                    wfs_wr16(node,
                             (uint32_t)offsetof(struct wfs_extent_header, entries),
                             (uint16_t)half);
                } else {
                    for (k = half; k > at; --k) {
                        put_extent(node,
                                   k,
                                   get_logical(node, k - 1u),
                                   get_physical(node, k - 1u),
                                   get_length(node, k - 1u));
                    }
                    put_extent(node, at, ctx->logical, (uint64_t)ctx->physical, ctx->length);
                    wfs_wr16(node,
                             (uint32_t)offsetof(struct wfs_extent_header, entries),
                             (uint16_t)(half + 1u));
                }
                ctx->split_logical = g_split[0].logical_block;
                /* The records past the new count are left where they are: they are
                 * beyond `entries`, so no reader consults them, and clearing them
                 * would cost a pass for nothing. */
                seal_node(ctx->vol, node, ctx->node_block);
                WFS_AWAIT(
                    ctx, wfs_txn_stage_begin(ctx->vol, ctx->node_block), WFS_XTADD_PC_SPLIT_LOWER);
                continue;
            }

            /* Open the slot. Backwards, so a record is read before the copy of
             * its predecessor overwrites it. */
            for (i = entries; i > at; --i) {
                put_extent(node,
                           i,
                           get_logical(node, i - 1u),
                           get_physical(node, i - 1u),
                           get_length(node, i - 1u));
            }
            put_extent(node, at, ctx->logical, (uint64_t)ctx->physical, ctx->length);
            wfs_wr16(node,
                     (uint32_t)offsetof(struct wfs_extent_header, entries),
                     (uint16_t)(entries + 1u));
            seal_node(ctx->vol, node, ctx->node_block);
            ctx->added = 1u;
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->node_block), WFS_XTADD_PC_LEAF_WRITTEN);
            continue;

        case WFS_XTADD_PC_LEAF_WRITTEN:
            ctx->err = wfs_txn_stage_take(ctx->vol);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            /* Both halves of the exclusivity rule land together: the object names
             * the root AND its inline array is zeroed, so no reader can find two
             * answers for one logical block (§9). */
            if (ctx->obj->extent_tree_block == 0u) {
                ctx->obj->extent_tree_block = (uint64_t)ctx->node_block;
                for (i = 0; i < WFS_INLINE_EXTENTS; ++i) {
                    ctx->obj->extents[i].logical_block = 0u;
                    ctx->obj->extents[i].physical_block = 0u;
                    ctx->obj->extents[i].length = 0u;
                    ctx->obj->extents[i].reserved = 0u;
                }
                ctx->obj->extent_count = ctx->added;
            } else {
                ctx->obj->extent_count += ctx->added;
            }
            return WASMOS_WASM_TASK_COMPLETE;

        case WFS_XTADD_PC_SPLIT_LOWER:
            ctx->err = wfs_txn_stage_take(ctx->vol);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            /* The records the split gave up, rebuilt into the fresh leaf. */
            node = wfs_block_data(b);
            init_leaf(ctx->vol, node, ctx->split_count);
            for (i = 0; i < ctx->split_count; ++i) {
                put_extent(node,
                           i,
                           g_split[i].logical_block,
                           g_split[i].physical_block,
                           g_split[i].length);
            }
            seal_node(ctx->vol, node, ctx->leaf_block);
            ctx->leaf_used = 1u;
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->leaf_block), WFS_XTADD_PC_SPLIT_UPPER);
            continue;

        case WFS_XTADD_PC_SPLIT_UPPER:
            ctx->err = wfs_txn_stage_take(ctx->vol);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            if (ctx->root_block != 0u) {
                /* An interior root already exists; the new leaf is indexed into
                 * it. */
                WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->root_block), WFS_XTADD_PC_ROOT_READY);
                continue;
            }
            /* The FIRST split: an interior root goes above the two leaves. The
             * object still names the old leaf at this point, and a crash here
             * leaves it naming a leaf that lost half its records -- which is why
             * the caller seals the record only after this task completes. */
            node = wfs_block_data(b);
            init_interior(ctx->vol, node, 2u);
            put_index(node, 0u, 0u, (uint64_t)ctx->node_block);
            put_index(node, 1u, ctx->split_logical, (uint64_t)ctx->leaf_block);
            seal_node(ctx->vol, node, ctx->root_spare);
            ctx->root_used = 1u;
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->root_spare), WFS_XTADD_PC_ROOT_WRITTEN);
            continue;

        case WFS_XTADD_PC_ROOT_READY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);
            if (!node_verifies(ctx, node, ctx->root_block)) {
                return (int32_t)ctx->err;
            }
            entries = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            capacity = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, capacity));
            if (entries >= capacity) {
                /* TODO: split the interior root and add a level above it. The
                 * root holds wfs_extent_interior_capacity() leaves -- 255 at a
                 * 4096-byte block size, so over forty thousand extents -- and
                 * refusing past that leaves the map intact rather than half
                 * rewritten. */
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
            }
            /* The new leaf covers the range just above the one it split from, so
             * its index follows that child's. */
            for (i = entries; i > ctx->child + 1u; --i) {
                put_index(node, i, index_logical(node, i - 1u), index_child(node, i - 1u));
            }
            put_index(node, ctx->child + 1u, ctx->split_logical, (uint64_t)ctx->leaf_block);
            wfs_wr16(node,
                     (uint32_t)offsetof(struct wfs_extent_header, entries),
                     (uint16_t)(entries + 1u));
            seal_node(ctx->vol, node, ctx->root_block);
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->root_block), WFS_XTADD_PC_ROOT_WRITTEN);
            continue;

        case WFS_XTADD_PC_ROOT_WRITTEN:
            ctx->err = wfs_txn_stage_take(ctx->vol);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            if (ctx->root_used) {
                /* The tree gained its first interior level, so the object names
                 * the new root rather than the leaf it used to. */
                ctx->obj->extent_tree_block = (uint64_t)ctx->root_spare;
            }
            ctx->added = 1u;
            ctx->obj->extent_count += ctx->added;
            return WASMOS_WASM_TASK_COMPLETE;

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}

int32_t wfs_extent_trim_task(void* user, uintptr_t* out_value) {
    wfs_xttrim_ctx_t* ctx = (wfs_xttrim_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint8_t* node;
    uint32_t entries;
    uint32_t capacity;
    uint32_t i;
    uint32_t victim;
    uint64_t vlog;
    uint64_t vphys;
    uint32_t vlen;
    uint32_t survivors;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_XTTRIM_PC_START:
            ctx->freed_first = 0u;
            ctx->freed_length = 0u;
            ctx->freed_node = 0u;
            ctx->removed = 0u;
            ctx->remaining = 0u;
            ctx->root_block = 0u;
            if (!ctx->vol || ctx->node_block == 0u ||
                ctx->node_block >= ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
            }
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->node_block), WFS_XTTRIM_PC_ROOT_READY);
            continue;

        case WFS_XTTRIM_PC_ROOT_READY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);
            if (wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, magic)) !=
                WFS_EXTENT_NODE_MAGIC) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            if (wfs_rd32(node, (uint32_t)offsetof(struct wfs_extent_header, checksum)) !=
                wfs_checksum_struct(ctx->vol->super.uuid,
                                    ctx->node_block,
                                    node,
                                    ctx->vol->super.block_size,
                                    (uint32_t)offsetof(struct wfs_extent_header, checksum))) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
            }
            if (wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, depth)) == 0u) {
                /* The root IS the leaf. */
                ctx->pc = WFS_XTTRIM_PC_LEAF_READY;
                continue;
            }
            entries = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            capacity = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, capacity));
            if (capacity != wfs_extent_interior_capacity(ctx->vol->super.block_size) ||
                entries > capacity) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            if (entries == 0u) {
                /* Every child is gone, so the root is unreferenced too. */
                ctx->remaining = 0u;
                return WASMOS_WASM_TASK_COMPLETE;
            }
            /* The LAST child, because a trim removes from the END of the map and
             * the children are ordered by the logical range they cover. Taking
             * them in that order means no surviving child ever moves. */
            ctx->root_block = ctx->node_block;
            ctx->child = entries - 1u;
            ctx->node_block = (uint32_t)index_child(node, ctx->child);
            if (ctx->node_block == 0u || ctx->node_block >= ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->node_block), WFS_XTTRIM_PC_LEAF_READY);
            continue;

        case WFS_XTTRIM_PC_LEAF_READY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);

            if (wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, magic)) !=
                WFS_EXTENT_NODE_MAGIC) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            if (wfs_rd32(node, (uint32_t)offsetof(struct wfs_extent_header, checksum)) !=
                wfs_checksum_struct(ctx->vol->super.uuid,
                                    ctx->node_block,
                                    node,
                                    ctx->vol->super.block_size,
                                    (uint32_t)offsetof(struct wfs_extent_header, checksum))) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CHECKSUM);
            }
            if (wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, depth)) != 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            entries = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            capacity = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, capacity));
            if (capacity != wfs_extent_leaf_capacity(ctx->vol->super.block_size) ||
                entries > capacity) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }

            /* The LAST record reaching at or past `keep`: taking them from the
             * end means the survivors never move, so the leaf stays sorted with
             * no shifting at all. */
            victim = entries;
            for (i = entries; i > 0u; --i) {
                if (get_logical(node, i - 1u) + get_length(node, i - 1u) > ctx->keep) {
                    victim = i - 1u;
                    break;
                }
            }
            if (victim == entries) {
                if (entries == 0u && ctx->root_block != 0u) {
                    /* This child holds nothing: its index leaves the root and its
                     * block goes back to the allocator. Reported separately from
                     * the data runs because it is METADATA -- the caller frees it
                     * with the revoke §18 requires, since the log may still hold
                     * an image of it. */
                    ctx->freed_node = ctx->node_block;
                    WFS_AWAIT(
                        ctx, wfs_block_read_begin(b, ctx->root_block), WFS_XTTRIM_PC_ROOT_WRITTEN);
                    continue;
                }
                /* Nothing reaches past the cut: the trim is complete. `remaining`
                 * is this leaf's count for a single-leaf map. Under an interior
                 * root it only has to be non-zero -- the caller keeps the real
                 * total by `removed`, which no one step could recompute without
                 * reading every leaf. */
                ctx->remaining = ctx->root_block != 0u ? entries + 1u : entries;
                return WASMOS_WASM_TASK_COMPLETE;
            }

            vlog = get_logical(node, victim);
            vphys = get_physical(node, victim);
            vlen = get_length(node, victim);
            if (vlog >= ctx->keep) {
                /* Wholly past the cut: the record goes, and with it its blocks.
                 * Records after it were already trimmed, so shortening `entries`
                 * is the whole removal. */
                ctx->freed_first = (uint32_t)vphys;
                ctx->freed_length = vlen;
                ctx->removed = 1u;
                entries = victim;
            } else {
                /* Straddles the cut: only the tail is released. */
                survivors = (uint32_t)(ctx->keep - vlog);
                ctx->freed_first = (uint32_t)vphys + survivors;
                ctx->freed_length = vlen - survivors;
                put_extent(node, victim, vlog, vphys, survivors);
                entries = victim + 1u;
            }
            /* Zero the vacated slots so the checksum covers defined bytes and
             * fsck does not read a stale record past `entries`. */
            for (i = entries; i < capacity; ++i) {
                if (get_length(node, i) == 0u && get_logical(node, i) == 0u) {
                    break;
                }
                put_extent(node, i, 0u, 0u, 0u);
            }
            wfs_wr16(
                node, (uint32_t)offsetof(struct wfs_extent_header, entries), (uint16_t)entries);
            seal_node(ctx->vol, node, ctx->node_block);
            ctx->remaining = ctx->root_block != 0u ? entries + 1u : entries;
            /* The leaf is rewritten BEFORE the run is released: a crash here
             * leaves blocks nothing names, which fsck reclaims, whereas freeing
             * first could hand a still-referenced block to another object. */
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->node_block), WFS_XTTRIM_PC_LEAF_WRITTEN);
            continue;

        case WFS_XTTRIM_PC_LEAF_WRITTEN:
            ctx->err = wfs_txn_stage_take(ctx->vol);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            return WASMOS_WASM_TASK_COMPLETE;

        case WFS_XTTRIM_PC_ROOT_WRITTEN:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);
            entries = wfs_rd16(node, (uint32_t)offsetof(struct wfs_extent_header, entries));
            if (entries == 0u) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
            }
            /* The emptied child is the LAST index, so dropping it is a shortening
             * -- no surviving index moves, and the block it named is reported for
             * release. */
            entries--;
            put_index(node, entries, 0u, 0u);
            wfs_wr16(
                node, (uint32_t)offsetof(struct wfs_extent_header, entries), (uint16_t)entries);
            seal_node(ctx->vol, node, ctx->root_block);
            /* Zero means the root is unreferenced now and the caller frees it
             * too, which is the same signal a single-leaf map gives. */
            ctx->remaining = entries;
            /* The root is rewritten BEFORE the leaf is released, for the reason
             * the leaf case gives: a crash leaves a block nothing names rather
             * than an index naming a freed one. */
            WFS_AWAIT(
                ctx, wfs_txn_stage_begin(ctx->vol, ctx->root_block), WFS_XTTRIM_PC_LEAF_WRITTEN);
            continue;

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}
