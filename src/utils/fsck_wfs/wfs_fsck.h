/* wfs_fsck.h - check a WFS volume, and repair what is defined to be repairable.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md section 24.
 *
 * This is the reference checker, built from the same wfs_format.h, wfs_crc32c.c
 * and wfs_super.c the driver reads through, for the reason the formatter is: a
 * checker with its own idea of the layout would agree with the driver only by
 * review, and every checksum seed and derived capacity would be a place for the
 * two to drift.
 *
 * WHAT IT REPAIRS, AND WHAT IT REFUSES TO. Section 24 states one repair rule:
 * the allocation bitmaps are authoritative and the free counters are derived, so
 * a counter mismatch is recomputed FROM the bitmap and never the reverse. This
 * implementation takes the same position one step further out -- the OBJECT
 * TABLE is authoritative over the bitmaps, because it is what records which
 * blocks an object actually holds, so the bitmaps are rebuilt from a walk of it.
 *
 * Everything structural is REPORTED and not rewritten: a record whose checksum
 * fails, an extent that overlaps another, a directory whose stride walks off its
 * block. Repairing those means inventing content, and a checker that invents
 * content turns a diagnosable volume into a plausible one. A run that finds any
 * such damage refuses to clear `state`, so the volume stays WFS_STATE_ERROR and
 * the next mount stays read-only -- which is the outcome section 4 asks for.
 *
 * I/O goes through callbacks rather than a device, so the same code path serves
 * the guest utility and the unit suites. The callbacks are SYNCHRONOUS, which is
 * correct for both: a one-shot utility runs, does its work and exits, so it may
 * block on a BLOCK request the way src/utils/blkinfo already does. A SERVICE
 * could not -- it would stall its event loop -- which is the distinction, not
 * host versus guest.
 */
#ifndef WFS_FSCK_H
#define WFS_FSCK_H

#include <stdint.h>

#include "wfs_format.h"
#include "wfs_status.h"
#include "wfs_super.h"

/* Block I/O. `read_block` is required. `write_block` is NULL for a check-only
 * run, which is what makes "report but do not touch" the default rather than a
 * flag a caller can forget to pass.
 *
 * Both address whole blocks and return WASMOS_ERR_NONE or a packed code. The
 * superblock is read through `read_block` too, as block 0, because a checker
 * that could not read block 0 has nothing to say about the volume anyway. */
typedef struct {
    wasmos_error_code_t (*read_block)(void* user, uint32_t block, void* out, uint32_t len);
    wasmos_error_code_t (*write_block)(void* user, uint32_t block, const void* in, uint32_t len);
    void* user;
} wfs_fsck_io_t;

/* What a run found. Counters rather than a list: a damaged volume can produce
 * unboundedly many of any one of these, and a caller that wants detail gets it
 * from the reporting callback below rather than from a buffer this has to size.
 */
typedef struct {
    /* Structural damage. Any of these being non-zero is what makes a volume
     * unrepairable by this tool. */
    uint32_t super_errors;  /* the primary failed and a backup was used */
    uint32_t group_errors;  /* a group descriptor failed its checksum */
    uint32_t object_errors; /* an object record failed its checksum or is malformed */
    uint32_t extent_errors; /* an extent or tree node is malformed, out of range, or overlaps */
    uint32_t dir_errors;    /* a directory block is malformed or fails its checksum */
    uint32_t link_errors;   /* link_count disagrees with the references counted */

    /* Derived state, which IS repairable. */
    uint32_t bitmap_errors;  /* bits disagreeing with the walk */
    uint32_t counter_errors; /* free counters disagreeing with the bitmaps */

    uint32_t repaired; /* of the two above, how many were written back */

    /* Volume shape, for a caller that wants to print it. */
    uint32_t objects_in_use;
    uint32_t blocks_in_use;
    uint8_t state_before;
    uint8_t state_after;
    uint8_t used_backup_super; /* the primary did not validate */
    uint8_t cleared_state;     /* the run left the volume WFS_STATE_CLEAN */
} wfs_fsck_report_t;

/* Called for each problem, with a stable one-line description. `block` and
 * `object` are the subject when they apply and WFS_BLOCK_NONE / 0 when they do
 * not. Optional: pass NULL to run silently and read the counters. */
typedef void (*wfs_fsck_log_fn)(void* user, const char* what, uint32_t block, uint32_t object);

/* Check the volume, repairing the derived state when `io->write_block` is set.
 *
 * Returns WASMOS_ERR_NONE when the volume is consistent, or was made so;
 * WASMOS_ERR_FS_CORRUPT when structural damage was found and reported;
 * otherwise whatever the I/O reported. `out` is filled either way.
 *
 * Fails with WASMOS_ERR_FS_NO_SPACE if the scratch this needs cannot be
 * allocated -- it holds one bit per block, one bit per object, and one link
 * count per object.
 */
wasmos_error_code_t wfs_fsck_run(const wfs_fsck_io_t* io, wfs_fsck_log_fn log, void* log_user,
                                 wfs_fsck_report_t* out);

#endif /* WFS_FSCK_H */
