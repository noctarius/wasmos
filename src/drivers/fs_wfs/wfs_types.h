/* wfs_types.h - the driver's operation contexts.
 *
 * Operations are tasks on the SYSTEM coroutine runtime
 * (src/libsys/wasm/coroutine_wasm.h, docs/architecture/32): each is a
 * wasmos_wasm_task_resume_fn that awaits a future and returns
 * WASMOS_WASM_TASK_YIELDED, and the runtime parks and resumes it. The runtime
 * owns the ready list, the waiter lists, and the settlement of every future;
 * this driver owns only the per-operation state below.
 *
 * The runtime preserves no stack across a resume, so a task records its own
 * resume point. That is what `pc` is, and it is why any value that must survive
 * an await lives in the context struct rather than in a C local — a local is
 * read uninitialised on the resume path, and a block number read that way sends
 * a request for whatever the stack happened to hold. Values computed strictly
 * after an await may be locals.
 */
#ifndef FS_WFS_WFS_TYPES_H
#define FS_WFS_WFS_TYPES_H

#include <stdint.h>

#include "wasmos/coroutine_wasm.h"
#include "wasmos_status.h"
#include "wfs_format.h"
#include "wfs_super.h"

/* A mounted volume: the superblock as the reader parsed it.
 *
 * Group descriptors are not held here. A volume of many groups has a descriptor
 * table larger than a driver's linear memory should carry, so a descriptor is
 * read from its block when its group is touched; the table's location is in the
 * superblock already. */
typedef struct {
    wfs_super_t super;
    uint8_t mounted;
} wfs_volume_t;

/* Reading one group descriptor. */
typedef enum {
    WFS_GROUP_PC_START = 0,
    WFS_GROUP_PC_BLOCK_READY,
} wfs_group_pc_t;

typedef struct {
    wfs_group_pc_t pc;
    const wfs_volume_t* vol;
    uint32_t group; /* which descriptor is wanted */
    uint32_t block; /* the block holding it; must survive the await */
    wasmos_error_code_t err;
    struct wfs_group_desc out;
} wfs_group_ctx_t;

/* Reading one object record. */
typedef enum {
    WFS_OBJECT_PC_START = 0,
    WFS_OBJECT_PC_BLOCK_READY,
} wfs_object_pc_t;

typedef struct {
    wfs_object_pc_t pc;
    const wfs_volume_t* vol;
    uint32_t object_id;
    uint32_t block; /* must survive the await, for the same reason */
    wasmos_error_code_t err;
    struct wfs_object out;
} wfs_object_ctx_t;

/* Mounting a volume. */
typedef enum {
    WFS_MOUNT_PC_START = 0,
    WFS_MOUNT_PC_SUPER_READY,
    WFS_MOUNT_PC_GROUP_JOINED,
} wfs_mount_pc_t;

typedef struct {
    wfs_mount_pc_t pc;
    wfs_volume_t* vol;
    wasmos_error_code_t err;

    /* The group-descriptor sweep. The child task's record and context are held
     * here because the runtime requires both to outlive the task, and a task
     * cannot keep them on a stack that does not survive its own await. */
    uint32_t next_group;
    uint8_t group_started; /* a child task is outstanding and owes a join */
    wasmos_wasm_coroutine_t group_task;
    wfs_group_ctx_t group;
} wfs_mount_ctx_t;

#endif /* FS_WFS_WFS_TYPES_H */
