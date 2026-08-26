/* wfs_sync.h - recording a volume's mount state on disk (§4).
 *
 * `state` says whether the volume was unmounted cleanly. A volume mounted for
 * WRITING must say WFS_STATE_DIRTY on disk before the first write lands, because
 * that flag is the only thing that tells the next mount its metadata may be
 * mid-update: wfs_mount_task turns a non-clean state into needs_replay and mounts
 * read-only, so a crash leaves a volume that refuses writes rather than one that
 * reads back plausible-looking garbage.
 *
 * Until the journal exists (phase 3) this is the whole of WFS's crash safety, and
 * it is worth having early precisely because phase 2 writes are not crash-safe.
 */
#ifndef FS_WFS_WFS_SYNC_H
#define FS_WFS_WFS_SYNC_H

#include "wfs_types.h"

/* Set the volume's on-disk state to WFS_STATE_DIRTY and record that it is set, so
 * a caller may start this before every write and pay for it once per mount.
 *
 * Completes immediately when vol->dirty_marked is already set. Fails with
 * WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes -- marking one
 * dirty would make a read-only mount look like an interrupted write.
 */
int32_t wfs_mark_dirty_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_SYNC_H */
