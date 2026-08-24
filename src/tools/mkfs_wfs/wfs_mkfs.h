/* wfs_mkfs.h - build a WFS volume.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md.
 *
 * This is the reference writer for the format. It is built from the same
 * wfs_format.h and wfs_crc32c.c the driver reads through, which is the point:
 * a formatter with its own private idea of the layout would agree with the
 * driver only by review, and every checksum seed, every reserved byte, and
 * every derived capacity would be a place for the two to drift apart.
 *
 * Emission goes through a sink rather than to a file, so the same code path
 * produces the images the host tool writes and the in-memory volumes the unit
 * tests validate. Nothing here opens a file or allocates the volume.
 */
#ifndef WFS_MKFS_H
#define WFS_MKFS_H

#include <stdint.h>

#include "wasmos_status.h"
#include "wfs_format.h"

typedef struct {
    uint64_t size_bytes;       /* volume size; truncated down to a whole block */
    uint32_t block_size;       /* 0 selects 4096 */
    uint32_t bytes_per_object; /* object-count ratio; 0 selects 16384 */
    uint32_t journal_blocks;   /* 0 derives one from the volume size */
    uint64_t now_ns;           /* stamped into the root object's timestamps */
    uint8_t uuid[WFS_UUID_LEN];
} wfs_mkfs_params_t;

/* Where every region lands. Computed before anything is written so a caller can
 * report the layout, and a test can assert on it, without re-deriving it. */
typedef struct {
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t total_blocks;
    uint32_t group_count;

    uint32_t total_objects;
    uint32_t objects_per_group;
    uint32_t object_table_blocks_per_group;

    uint32_t group_table_start;
    uint32_t group_table_blocks;
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t object_table_start;
    uint32_t object_table_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;

    uint32_t first_data_block; /* first block an allocator may hand out */
    uint32_t root_data_block;  /* the root directory's single data block */

    uint32_t free_blocks;
    uint32_t free_objects;
} wfs_mkfs_layout_t;

/* Receives the volume one block at a time, in ascending block order. A sink
 * returns 0 on success and non-zero to abort the format. */
typedef struct {
    void* ctx;
    int (*write_block)(void* ctx, uint32_t block, const void* data, uint32_t len);
} wfs_mkfs_sink_t;

/* Compute the layout for `params` without writing anything.
 *
 * Fails with FS_GEOMETRY for a block size outside the three permitted,
 * FS_NO_SPACE for a volume too small to hold its own metadata, and
 * FS_VOLUME_TOO_LARGE for one past what a uint32_t block number addresses.
 */
wasmos_error_code_t wfs_mkfs_plan(const wfs_mkfs_params_t* params, wfs_mkfs_layout_t* out);

/* Plan, then emit every block of the volume to `sink`. `out_layout` may be NULL.
 *
 * Blocks are written in ascending order and each is written exactly once, so a
 * sink may append rather than seek.
 */
wasmos_error_code_t wfs_mkfs_format(const wfs_mkfs_params_t* params, const wfs_mkfs_sink_t* sink,
                                    wfs_mkfs_layout_t* out_layout);

#endif /* WFS_MKFS_H */
