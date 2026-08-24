/* wfs_types.h - the reactor's vocabulary.
 *
 * No block I/O and no global mutable state live here.
 */
#ifndef FS_WFS_WFS_TYPES_H
#define FS_WFS_WFS_TYPES_H

#include <stdint.h>

#include "wasmos_status.h"
#include "wfs_format.h"
#include "wfs_super.h"

/* What a resumable step reports to its caller and ultimately to the reactor. */
typedef enum {
    WFS_R_DONE = 0, /* the step completed */
    WFS_R_WAIT = 1, /* the step yielded; a block request is outstanding */
    WFS_R_ERR = 2,  /* the step failed; the code is on the op that owns the buffer */
} wfs_r_t;

/* A mounted volume. The superblock as the reader parsed it, plus what mount
 * learned from the group descriptor table.
 *
 * Group descriptors are NOT held here. A volume of many groups has a descriptor
 * table larger than a driver's linear memory should carry, so a descriptor is
 * read from its block when a group is touched. Only the table's location is
 * kept, which is in the superblock already.
 */
typedef struct {
    wfs_super_t super;
    uint8_t mounted;
} wfs_volume_t;

/* Reading one group descriptor: a sub-machine because it costs a block read. */
typedef struct {
    int cont;
    uint32_t group;            /* which descriptor is wanted */
    uint32_t block;            /* the block holding it; see the note below */
    struct wfs_group_desc out; /* the descriptor, once the step completes */
} wfs_group_ctx_t;

/* Reading one object record. */
typedef struct {
    int cont;
    uint32_t object_id;
    uint32_t block; /* the block holding the record; see the note below */
    struct wfs_object out;
} wfs_object_ctx_t;

/* Why `block` is a context field and not a C local.
 *
 * A yielding macro returns out of its function and the reactor later re-enters
 * at the resume label, so the C stack from before the yield is gone. The block
 * number is evaluated INSIDE the yield point — it is the argument to the read —
 * so on resume a local would be read uninitialised, and the staged-block cache
 * check would compare against garbage and reissue the read. Every block number
 * handed to WFS_CO_READ therefore lives in the context.
 *
 * Values computed strictly AFTER a yield may be locals; the steps recompute
 * those rather than carry them. */

/* The mount operation. */
typedef struct {
    int cont;
    wfs_group_ctx_t group;
    uint32_t checked_groups; /* cursor over the descriptor table */
} wfs_mount_ctx_t;

#endif /* FS_WFS_WFS_TYPES_H */
