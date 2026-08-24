/* wfs_mount.h - mount, group-descriptor and object-record tasks.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15, §11, §7, §13.
 *
 * Each of these is a wasmos_wasm_task_resume_fn for the system coroutine
 * runtime. A caller starts one with wasmos_async_start and drives the runtime;
 * the task awaits block futures and the runtime parks and resumes it. A task
 * returns WASMOS_WASM_TASK_COMPLETE on success, or a negative packed
 * WASMOS_ERR_FS_* code, which rejects its completion future — so a caller that
 * joins it observes the failure without a separate status channel.
 *
 * The block client these tasks share is reached through wfs_ops_bind: the
 * runtime hands a task only its `user` pointer, and every one of these needs
 * the same wfs_block_t.
 */
#ifndef FS_WFS_WFS_MOUNT_H
#define FS_WFS_WFS_MOUNT_H

#include "wasmos/coroutine_wasm.h"
#include "wfs_block.h"
#include "wfs_types.h"

/* Bind the block client and runtime these tasks operate on. Called once, before
 * any task is started. */
void wfs_ops_bind(wasmos_wasm_runtime_t* runtime, wfs_block_t* block);

/* The bound block client, or NULL before wfs_ops_bind. Exposed so a caller can
 * read a failure's cause off the same client the tasks used. */
wfs_block_t* wfs_ops_block(void);

/* Read and verify group descriptor `ctx->group` into `ctx->out`.
 *
 * The descriptor is checksummed under its GROUP INDEX, not under the block that
 * holds it: it is one record among many in a shared block, so the index that
 * addresses it is what binds it to its slot (§13). A descriptor moved to
 * another slot therefore fails here. Set `ctx->vol` and `ctx->group` before
 * starting the task. */
int32_t wfs_group_task(void* user, uintptr_t* out_value);

/* Read and verify object record `ctx->object_id` into `ctx->out`. Objects are
 * fixed-size and the table is contiguous, so the record's location is a
 * division rather than a walk. The record is checksummed under its object_id. */
int32_t wfs_object_task(void* user, uintptr_t* out_value);

/* Mount `ctx->vol`.
 *
 * Reads block 0, parses the superblock at its fixed byte offset, adopts the
 * block size, then verifies every group descriptor before declaring the volume
 * mounted — one that fails names a bitmap block nothing vouches for, and every
 * allocation in its group would address it. The sweep runs the group task as a
 * child and joins it, which is the runtime's own composition rather than a
 * private sub-machine convention. */
int32_t wfs_mount_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_MOUNT_H */
