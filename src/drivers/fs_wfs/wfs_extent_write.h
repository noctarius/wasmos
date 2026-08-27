/* wfs_extent_write.h - add one extent to an object whose inline map is full (§9).
 *
 * The write side of wfs_extent.c. Where that resolves a logical block through an
 * object's map, this GROWS the map past the six extents an object record holds:
 *
 *   inline (6 extents)  --promote-->  one leaf node  --insert-->  the same leaf
 *
 * The two maps are exclusive (§9). A promotion therefore moves the six inline
 * extents into the leaf and ZEROES the inline array in the same update, because
 * two sources of truth for one logical range is a corruption nothing could
 * detect: a reader would take whichever it consulted first.
 *
 * Records in a node are sorted by `logical_block` and cover disjoint ranges. The
 * inline array is not sorted -- it is scanned linearly, so writes may append to
 * it in any order -- so a promotion SORTS what it moves. An interior node's
 * descent takes the last index whose logical_block does not exceed the target
 * and would silently return the wrong extent for an unsorted node.
 *
 * Ordering: the caller must have the extent's DATA block on disk before calling
 * this on an object that already has a tree. A leaf is reachable from the object
 * record the moment it is written, so publishing an extent before its content
 * would name a block still holding whatever it held before. On a promotion the
 * order does not matter, because the record naming the new tree is sealed later
 * by the caller and a crash before that leaves the leaf unreferenced -- a leak,
 * which fsck reclaims.
 */
#ifndef FS_WFS_WFS_EXTENT_WRITE_H
#define FS_WFS_WFS_EXTENT_WRITE_H

#include "wfs_types.h"

/* Records a node holds at this block size, for a caller deciding whether a
 * further extent fits. Mirrors wfs_extent_leaf_capacity(). */
static inline uint32_t wfs_extent_add_leaf_capacity(uint32_t block_size) {
    return wfs_extent_leaf_capacity(block_size);
}

/* Add the extent [logical, logical+length) -> physical to `obj`'s map.
 *
 * `obj` is updated IN PLACE: extent_tree_block, extent_count and the inline
 * array are this task's outputs, and the caller seals them into the object
 * record afterwards. Nothing here touches the record.
 *
 * `leaf_block` must name a freshly allocated block when the object has no tree
 * yet (obj->extent_tree_block == 0), and is ignored once it has one. The caller
 * allocates it rather than this task, so the allocator runs as the caller's
 * sub-task instead of nesting one level deeper.
 *
 * Coalesces into the record it would follow when the new extent continues it
 * both logically and physically, which keeps an append to ONE extent; in that
 * case extent_count is unchanged.
 *
 * Fails with WASMOS_ERR_FS_BAD_ARGS when called with an inline map that still
 * has room (the caller records those without a node) or without a leaf_block for
 * a promotion, and WASMOS_ERR_FS_UNSUPPORTED when the leaf is full or the tree
 * is deeper than one leaf -- splitting a node and adding an interior level are
 * not implemented, so a tree only ever grows to a single leaf.
 */
int32_t wfs_extent_add_task(void* user, uintptr_t* out_value);

/* Detach ONE run at or past `keep` logical blocks from a tree's leaf, rewriting
 * the leaf without it and reporting the run for the caller to release.
 *
 * Called in a loop: `freed_length` zero means nothing was left to trim.
 * `remaining` zero means the leaf holds no records and the caller releases the
 * leaf block itself and clears the object's extent_tree_block. keep == 0 drops
 * every record, which is what releasing a whole object needs.
 *
 * The leaf is rewritten BEFORE the run is released, so an interruption leaks
 * blocks rather than leaving a freed block still named by a live extent.
 *
 * Fails with WASMOS_ERR_FS_UNSUPPORTED on an interior tree, which nothing
 * writes.
 */
int32_t wfs_extent_trim_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_EXTENT_WRITE_H */
