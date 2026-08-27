/* wfs_namespace.h - creating, removing and renaming directory entries (§10).
 *
 * The five operations O_CREAT, MKDIR, UNLINK, RMDIR and RENAME were all blocked
 * on the same two pieces, which now exist: object allocation (wfs_alloc.h) and
 * record surgery inside a directory block (wfs_dirent.h). This composes them.
 *
 * Written as plain sequences over wfs_ops_run rather than as coroutine state
 * machines. Each operation is several tasks in order — resolve, allocate, insert,
 * seal — and a state machine would have to carry every intermediate across an
 * await for no benefit: nothing else can run during one of these anyway, because
 * the block layer stages a single block. It also keeps them host-testable, since
 * the pump needs only the bound runtime and block client.
 *
 * ORDER, throughout: whichever sequence leaves a LEAK when it is interrupted.
 * Creating writes the object before the directory record, so a crash leaves an
 * object nothing names; removing takes the record off disk before freeing
 * anything, so a crash leaves space nothing names. The opposite of either leaves
 * a directory entry pointing at an id that is unallocated or already reused —
 * which is corruption no later pass can repair, where a leak is just space fsck
 * reclaims.
 */
#ifndef FS_WFS_WFS_NAMESPACE_H
#define FS_WFS_WFS_NAMESPACE_H

#include "wfs_types.h"

/* Create an empty file or directory named by `path`, relative to `cwd_object`.
 *
 * `type` is WFS_TYPE_FILE or WFS_TYPE_DIR. A directory is created with its own
 * first block laid out and `.` and `..` in place, and the parent's link count is
 * raised to account for the new `..`.
 *
 * *out_object_id names the new object on success.
 *
 * Fails with WASMOS_ERR_FS_EXISTS when the name is taken, WASMOS_ERR_FS_NOT_DIR
 * when the parent is not a directory, WASMOS_ERR_FS_NAME for a path naming no
 * component, WASMOS_ERR_FS_NO_SPACE when the parent directory block is full (see
 * the TODO in wfs_namespace.c: growing a directory is not implemented) and
 * WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes.
 */
wasmos_error_code_t wfs_ns_create(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                  uint32_t path_len, uint16_t type, uint32_t mode, uint64_t now_ns,
                                  uint32_t* out_object_id);

/* Remove the FILE named by `path`, releasing its blocks and its record.
 *
 * Fails with WASMOS_ERR_FS_IS_DIR on a directory — rmdir is a separate operation
 * because it has a different precondition — and WASMOS_ERR_FS_NOT_FOUND when the
 * name is absent.
 */
wasmos_error_code_t wfs_ns_unlink(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                  uint32_t path_len, uint64_t now_ns);

/* Remove the empty DIRECTORY named by `path`.
 *
 * Fails with WASMOS_ERR_FS_NOT_DIR on a file, WASMOS_ERR_FS_NOT_EMPTY when it
 * still holds an entry other than `.` and `..`, and WASMOS_ERR_FS_NOT_FOUND when
 * the name is absent. The parent's link count drops with the removed `..`.
 */
wasmos_error_code_t wfs_ns_rmdir(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                 uint32_t path_len, uint64_t now_ns);

/* Move the entry `from` names to `to`, within or between directories.
 *
 * The INSERT happens before the removal, which is the opposite of every other
 * removal here and deliberate: interrupted, it leaves the object reachable under
 * BOTH names, where the other order would leave it reachable under neither.
 *
 * Fails with WASMOS_ERR_FS_EXISTS when the destination is taken — this does not
 * replace — and WASMOS_ERR_FS_NOT_FOUND when the source is absent.
 */
wasmos_error_code_t wfs_ns_rename(wfs_volume_t* vol, uint32_t cwd_object, const char* from,
                                  uint32_t from_len, const char* to, uint32_t to_len,
                                  uint64_t now_ns);

#endif /* FS_WFS_WFS_NAMESPACE_H */
