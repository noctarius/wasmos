/* wfs_super.h - reading and validating a WFS superblock.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §4, §5, §15, §22.
 *
 * Mount reads a fixed 1024 bytes at byte offset WFS_SUPER_OFFSET and hands them
 * here. The read is expressed in bytes because block_size is itself a
 * superblock field: no block unit exists until this parse completes.
 *
 * The parsed form carries block numbers as uint32_t (§22). Host calls take i32
 * scalars and IPC carries four 32-bit arguments, so a 64-bit block number
 * cannot cross either boundary; a uint32_t block number still reaches 16 TiB at
 * a 4096-byte block size, past the 2 TiB the block layer can address. A volume
 * whose on-disk counts exceed that is refused with
 * WASMOS_ERR_FS_VOLUME_TOO_LARGE rather than silently truncated.
 */
#ifndef FS_WFS_WFS_SUPER_H
#define FS_WFS_WFS_SUPER_H

#include <stdint.h>

#include "wfs_status.h"
#include "wfs_format.h"

/* A validated superblock, in the widths the driver carries. */
typedef struct {
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t group_count;

    uint32_t total_blocks;
    uint32_t total_objects;
    uint32_t free_blocks;  /* advisory; the bitmaps are authoritative */
    uint32_t free_objects; /* advisory */

    uint32_t root_object_id;

    uint32_t group_table_start;
    uint32_t group_table_blocks;
    uint32_t object_table_start;
    uint32_t object_table_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t journal_start;
    uint32_t journal_blocks;

    uint64_t generation;

    uint32_t feature_compat;
    uint32_t feature_ro_compat;
    uint32_t feature_incompat;

    uint32_t state;

    uint8_t uuid[WFS_UUID_LEN];

    /* An unknown RO_COMPAT flag is present: the volume is readable but must not
     * be written, because a writer that does not understand the feature would
     * corrupt what it does not maintain (§6). */
    uint8_t read_only;

    /* state is not WFS_STATE_CLEAN, so the journal must be replayed before the
     * volume is used. A clean volume skips the scan entirely (§15). */
    uint8_t needs_replay;
} wfs_super_t;

/* Parse and validate the superblock image at `image` (`len` bytes, at least
 * WFS_SUPER_SIZE). `location` is the seed location for the checksum: 0 for the
 * primary superblock, the containing block number for a backup (§13).
 *
 * Returns WASMOS_ERR_NONE and fills `out` on success. On failure `out` is
 * untouched and the result names which check failed:
 *
 *   FS_BAD_MAGIC         not a WFS volume at all
 *   FS_VERSION           a WFS volume of a version this driver does not implement
 *   FS_CHECKSUM          the image did not verify against the checksum it carries
 *   FS_FEATURE_INCOMPAT  an INCOMPAT flag this driver does not implement
 *   FS_GEOMETRY          block size not permitted, or group size not derived from it
 *   FS_VOLUME_TOO_LARGE  counts above what a uint32_t block number addresses
 *   FS_CORRUPT           a field is inconsistent with the geometry
 *
 * The order is deliberate: identity before integrity before capability before
 * geometry. Reporting a checksum failure on a block that was never a superblock
 * would send a reader to fsck over what is merely an unformatted device.
 */
/* Whether `candidate` should replace the copy the scan currently holds.
 *
 * Highest `generation` wins, which is what makes "choose the valid copy" (§5)
 * decidable when more than one validates: generation increments on every
 * superblock write, so the highest valid one is the most recent. An equal
 * generation keeps what is already held, leaving scan order to decide.
 *
 * `have_current` is zero before any copy has been accepted, in which case the
 * candidate is always taken. */
int wfs_super_backup_prefer(const wfs_super_t* current, int have_current,
                            const wfs_super_t* candidate);

/* The permitted block sizes a backup scan must try, and how many backup-bearing
 * groups it reaches before giving up. The scan is bounded on purpose: a volume
 * whose primary AND first few backups are all unreadable is a case for fsck
 * (§24), not for reading the whole device looking for a superblock. */
#define WFS_SUPER_SCAN_SIZES 3u
#define WFS_SUPER_SCAN_GROUPS 4u
#define WFS_SUPER_SCAN_CANDIDATES (WFS_SUPER_SCAN_SIZES * WFS_SUPER_SCAN_GROUPS)

/* Enumerate the scan's candidates as (block_size, group) pairs, in the order the
 * scan should try them: all three sizes for the nearest backup group first, so a
 * volume whose group 1 copy is intact is found without reading further out.
 *
 * Returns 1 while `index` names a candidate, 0 once the bound is reached. */
int wfs_super_backup_candidate(uint32_t index, uint32_t* out_block_size, uint32_t* out_group);

wasmos_error_code_t wfs_super_parse(const void* image, uint32_t len, uint64_t location,
                                    wfs_super_t* out);

/* Byte offset of the backup superblock in group `group`, or 0 when the group
 * carries none. Backups sit at the first block of odd-numbered groups (§5).
 *
 * Expressed in bytes for the same reason the primary is: a scan that runs
 * because the primary is unreadable has no block_size to work from, and
 * enumerates the three permitted sizes instead.
 */
uint64_t wfs_super_backup_offset(uint32_t block_size, uint32_t group);

/* Whether `group` carries a backup superblock. */
int wfs_super_group_has_backup(uint32_t group);

#endif /* FS_WFS_WFS_SUPER_H */
