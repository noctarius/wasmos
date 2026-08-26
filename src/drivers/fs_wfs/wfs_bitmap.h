/* wfs_bitmap.h - block and object allocation bitmaps (§12).
 *
 * Allocation is bitmap-based: one bit maps one block or one object record,
 * LEAST-SIGNIFICANT BIT FIRST within each byte, and a SET bit means allocated.
 * `blocks_per_group` is 8 * block_size precisely so one block of bitmap covers
 * exactly one group's blocks, which is what makes a group's allocation state a
 * single-block read.
 *
 * The bitmaps are AUTHORITATIVE. The free counters in the superblock and the
 * group descriptors are derived from them and are rebuilt by fsck, so a counter
 * that disagrees is a stale counter, never a reason to hand out a set bit.
 *
 * Nothing here does I/O or holds state: these operate on one bitmap block a
 * caller has already staged, which is what keeps the allocation POLICY testable
 * without a device under it.
 */
#ifndef FS_WFS_WFS_BITMAP_H
#define FS_WFS_WFS_BITMAP_H

#include <stdint.h>

/* Whether bit `i` is set, i.e. whether that block or object is allocated. */
int wfs_bitmap_test(const uint8_t* map, uint32_t i);

void wfs_bitmap_set(uint8_t* map, uint32_t i);
void wfs_bitmap_clear(uint8_t* map, uint32_t i);

/* Free bits among the first `bits`, which is what a rebuilt free counter is. */
uint32_t wfs_bitmap_count_free(const uint8_t* map, uint32_t bits);

/* Find a run of clear bits to allocate, over the first `bits` of `map`.
 *
 * Implements the contiguous-then-fragmented half of the allocation policy
 * (§12): the FIRST run long enough for `want` is taken as soon as it is found,
 * and if no run is that long the LONGEST available run is returned instead, so
 * the caller can take it and come back for the remainder. A caller that needs
 * contiguity therefore checks the returned length rather than assuming it.
 *
 * Returns the run length, capped at `want`, with *out_start set to its first
 * bit. Returns 0 when every bit is set, when `want` is 0, or on a NULL argument,
 * leaving *out_start untouched.
 */
uint32_t wfs_bitmap_find_run(const uint8_t* map, uint32_t bits, uint32_t want, uint32_t* out_start);

#endif /* FS_WFS_WFS_BITMAP_H */
