/* wfs_sync.h - recording a volume's mount state on disk (§4).
 *
 * `state` says whether the volume was unmounted cleanly. A volume mounted for
 * WRITING must say WFS_STATE_DIRTY on disk before the first metadata write
 * lands, because that flag is what makes the next mount read the log at all
 * (§15): wfs_mount_task turns a non-clean state into needs_replay and replays.
 *
 * The flag is deliberately NOT journaled. A mount that has not read it does not
 * know whether to consult the log, so a value only a replay could apply would be
 * useless to the decision it exists to inform. wfs_txn_open is what sets it, once
 * per mount, before the transaction it opens writes anything.
 */
#ifndef FS_WFS_WFS_SYNC_H
#define FS_WFS_WFS_SYNC_H

#include "wfs_types.h"

/* Set the volume's on-disk state to WFS_STATE_DIRTY and record that it is set, so
 * a caller may start this before every transaction and pay for it once per mount.
 *
 * Completes immediately when vol->dirty_marked is already set. Fails with
 * WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes -- marking one
 * dirty would make a read-only mount look like an interrupted write.
 */
int32_t wfs_mark_dirty_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_SYNC_H */
