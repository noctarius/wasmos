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
 *
 * The sink is not usable from a guest-side mkfs.wfs: both callbacks are
 * synchronous, while in a guest a block write is a BLOCK request and reading
 * source content is an FS request, each of which must be awaited from a
 * coroutine rather than returned inline. A guest uses wfs_mkfs_plan_tree and
 * wfs_mkfs_block_at instead -- planning and serialization with no loop and no
 * I/O of their own -- and drives the writes itself, awaiting between blocks.
 * Formatting an EMPTY volume needs only the write side, because the read
 * callback is reachable only through entries.
 */
#ifndef WFS_MKFS_H
#define WFS_MKFS_H

#include <stdint.h>

#include "wfs_status.h"
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
    uint32_t root_data_block;  /* the root directory's first block */
    uint32_t root_blocks;      /* blocks the root directory occupies */
    uint32_t used_blocks;      /* data blocks the entries consumed */
    uint32_t entry_count;      /* entries placed, excluding the root */

    uint32_t free_blocks;
    uint32_t free_objects;
} wfs_mkfs_layout_t;

/* Receives the volume one block at a time, in ascending block order. A sink
 * returns 0 on success and non-zero to abort the format. */
typedef struct {
    void* ctx;
    int (*write_block)(void* ctx, uint32_t block, const void* data, uint32_t len);
} wfs_mkfs_sink_t;

/* ---- populating a volume ------------------------------------------------ */

/* `parent` of an entry that belongs to the root directory. */
#define WFS_MKFS_ROOT 0xFFFFFFFFu

/* One entry to place in the volume.
 *
 * The caller enumerates the tree and reads the content; this module knows
 * nothing about a host filesystem, which keeps it testable without one and
 * leaves the directory walk in the tool where it belongs.
 *
 * That separation is NOT enough to reuse this module inside the OS as a
 * guest-side mkfs.wfs. Both callbacks below are synchronous, and in a guest both
 * would be IPC: writing a block is a BLOCK request, reading source content is an
 * FS request, and each must be awaited from a coroutine rather than returned
 * from inline. What would port is the planning and serialisation — the layout
 * arithmetic, the record packing, the checksum seeding — and that would first
 * have to be split out of the emission loop that drives the callbacks. Until
 * then this is a host formatter.
 *
 * Entries must be ordered so that a parent appears before its children: the
 * formatter assigns object ids in array order and sizes each directory from the
 * children that name it.
 */
typedef struct {
    const char* name; /* leaf name, never a path; must not be "." or ".." */
    uint32_t name_len;
    uint32_t parent; /* index of the parent entry, or WFS_MKFS_ROOT */
    uint8_t is_dir;
    uint32_t mode; /* permission bits */
    uint64_t size; /* bytes; ignored for a directory */

    /* Read `len` bytes of this entry's content at `offset`. Called only for a
     * file, once per block in ascending order, and expected to zero-fill a short
     * tail. Returns 0, or non-zero to abort the format. */
    int (*read)(void* ctx, uint64_t offset, void* dst, uint32_t len);
    void* read_ctx;
} wfs_mkfs_entry_t;

/* Per-entry scratch the formatter fills in. The caller owns it so this module
 * allocates nothing at all.
 *
 * The array must hold `count + 1` slots: the last one is the root directory's. */
typedef struct {
    uint32_t object_id;
    uint32_t first_block; /* first block of the entry's data, 0 if it has none */
    uint32_t block_count; /* blocks of data; 0 for an empty or inline entry */
    uint32_t child_count; /* entries naming this one as parent */
    uint8_t inline_data;  /* the content lives in the object record (§7) */
} wfs_mkfs_node_t;

/* Format a volume and place `count` entries in it.
 *
 * `plan` must hold `count + 1` slots and is filled in by this call. Blocks are
 * still emitted in ascending order and written exactly once, so an appending
 * sink stays correct.
 *
 * Fails with FS_NO_SPACE when the entries do not fit, FS_NAME for a name that is
 * empty, over-long, contains '/', or is "." or "..", FS_CORRUPT for a parent
 * index that is not a directory declared earlier, and FS_IO when a read callback
 * or the sink reports failure.
 *
 * wfs_mkfs_format is this with no entries, so the empty-volume path and the
 * populated one are the same code.
 */
wasmos_error_code_t wfs_mkfs_format_tree(const wfs_mkfs_params_t* params,
                                         const wfs_mkfs_entry_t* entries, uint32_t count,
                                         wfs_mkfs_node_t* plan, const wfs_mkfs_sink_t* sink,
                                         wfs_mkfs_layout_t* out_layout);

/* Compute the layout for `params` without writing anything.
 *
 * Fails with FS_GEOMETRY for a block size outside the three permitted,
 * FS_NO_SPACE for a volume too small to hold its own metadata, and
 * FS_VOLUME_TOO_LARGE for one past what a uint32_t block number addresses.
 */
wasmos_error_code_t wfs_mkfs_plan(const wfs_mkfs_params_t* params, wfs_mkfs_layout_t* out);

/* Plan the layout AND the tree: where each entry's blocks and object id land.
 *
 * `plan` must hold `count + 1` slots; slot `count` describes the root. The
 * layout returned carries root_blocks, used_blocks and entry_count, which the
 * bare wfs_mkfs_plan cannot know. Same failures as wfs_mkfs_format_tree minus
 * the ones only a sink or a read callback can raise.
 *
 * Nothing is written. This is the half of formatting a guest can run inline.
 */
wasmos_error_code_t wfs_mkfs_plan_tree(const wfs_mkfs_params_t* params,
                                       const wfs_mkfs_entry_t* entries, uint32_t count,
                                       wfs_mkfs_node_t* plan, wfs_mkfs_layout_t* out_layout);

/* Serialize ONE block of the volume a format produces into `buf`.
 *
 * Pure: the same inputs give the same block, in any order and any number of
 * times, which is what lets a guest write blocks one at a time and await each
 * one. `plan` and `layout` must both come from one wfs_mkfs_plan_tree call over
 * the same entries; `buf` is layout->block_size bytes and is fully written,
 * zeroes included.
 *
 * `block` is absolute and every block below layout->total_blocks has an answer,
 * because a format writes each one exactly once: the regions tile the volume and
 * the tail is free space carrying the backup superblocks.
 *
 * Fails with FS_BAD_ARGS for a null argument or a block at or past the end of
 * the volume, FS_IO when an entry's read callback fails, and FS_CORRUPT for a
 * data block the plan assigns to no entry. Only the first is reachable when
 * count is 0, which is why an empty format cannot fail on content.
 */
wasmos_error_code_t wfs_mkfs_block_at(const wfs_mkfs_params_t* params,
                                      const wfs_mkfs_entry_t* entries, uint32_t count,
                                      const wfs_mkfs_node_t* plan, const wfs_mkfs_layout_t* layout,
                                      uint32_t block, uint8_t* buf);

/* Plan, then emit every block of the volume to `sink`. `out_layout` may be NULL.
 *
 * Blocks are written in ascending order and each is written exactly once, so a
 * sink may append rather than seek.
 */
wasmos_error_code_t wfs_mkfs_format(const wfs_mkfs_params_t* params, const wfs_mkfs_sink_t* sink,
                                    wfs_mkfs_layout_t* out_layout);

#endif /* WFS_MKFS_H */
