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

int32_t wfs_extent_add_task(void* user, uintptr_t* out_value) {
    wfs_xtadd_ctx_t* ctx = (wfs_xtadd_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint8_t* node;
    uint32_t entries;
    uint32_t capacity;
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
                WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->node_block), WFS_XTADD_PC_LEAF_READY);
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

        case WFS_XTADD_PC_LEAF_READY:
            ctx->err = wfs_block_take(b);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            node = wfs_block_data(b);

            /* Validated before a record is believed or moved, on the same terms
             * wfs_extent.c reads it: a node failing these is not a node. */
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
                /* Only a single leaf is ever written here, so an interior root is
                 * a shape this does not maintain.
                 *
                 * TODO: descend to the leaf covering the target and split it,
                 * adding an interior level, so a tree may exceed one leaf. */
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
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
                /* TODO: split the leaf and add an interior root, so an object may
                 * exceed wfs_extent_leaf_capacity() extents. Refusing leaves the
                 * map intact; a half-written node would be unreadable. */
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
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
            ctx->remaining = 0u;
            if (!ctx->vol || ctx->node_block == 0u ||
                ctx->node_block >= ctx->vol->super.total_blocks) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BAD_ARGS);
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
                /* TODO: trim an interior tree, once one can be written. Nothing
                 * produces that shape, so refusing cannot strand a volume this
                 * driver made. */
                WFS_FAIL(ctx, WASMOS_ERR_FS_UNSUPPORTED);
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
                /* Nothing reaches past the cut: the trim is complete. */
                ctx->remaining = entries;
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
            ctx->remaining = entries;
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

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}
