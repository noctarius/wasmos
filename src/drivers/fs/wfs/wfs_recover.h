/* wfs_recover.h - crash recovery: replaying the metadata journal (§21).
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15, §21.
 *
 * A volume whose superblock does not say WFS_STATE_CLEAN was mounted for writing
 * and never unmounted, so the log may hold a transaction whose metadata never
 * reached its blocks. Replay applies it before the volume is handed out; a clean
 * volume skips this entirely, which is what keeps a normal mount from paying a
 * full journal read (§15).
 *
 * Replay is IDEMPOTENT. Every image is a whole-block overwrite, so a crash
 * during recovery repeats it from the same tail with the same result.
 */
#ifndef FS_WFS_WFS_RECOVER_H
#define FS_WFS_WFS_RECOVER_H

#include "wfs_types.h"

/* Replay the log into `ctx->vol`, then advance the tail past what was applied.
 *
 * Loads the journal superblock when the volume does not already carry it, so a
 * caller need not have run wfs_journal_load_task. On success `ctx->applied` is the number of block
 * images written: zero is the ordinary result for a volume that crashed between transactions and
 * means the log held nothing committed.
 *
 * Failures:
 *
 *   FS_JOURNAL  the log is not one this driver can interpret -- see
 *               wfs_journal_load_task, or a tail record that is neither a
 *               descriptor nor stale content, or a transaction naming more
 *               blocks than a transaction here carries
 *   FS_CORRUPT  a descriptor names a target outside the volume, block 0, or a
 *               block inside the journal region itself
 *   FS_REPLAY   a committed image did not match the checksum its descriptor
 *               recorded, so applying the transaction would write a partial one
 *   FS_IO       the device failed
 *
 * A failure leaves the volume for fsck (§24): the caller mounts read-only rather
 * than serving metadata the log has superseded.
 *
 * Context: wfs_replay_ctx_t.
 */
int32_t wfs_journal_replay_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_RECOVER_H */
