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

/* Record that an inconsistency was detected, so the NEXT mount refuses to write
 * (§4's WFS_STATE_ERROR).
 *
 * Written by wfs_mount_task when a replay cannot complete, through
 * wfs_super_write_task with `force` set -- the volume is already read-only by
 * then, and this is the write that says why.
 *
 * TODO: a checksum that fails during an ORDINARY operation -- an object record,
 * a directory tail, an extent node -- is an inconsistency by the same definition
 * and is not recorded. It reaches the caller as WASMOS_ERR_FS_CHECKSUM and the
 * volume stays writable, so the next mount sees nothing wrong. Recording it
 * needs a write from paths that are otherwise read-only, and a policy decision
 * this does not make: whether one bad record should cost the whole volume its
 * writability, as ext4's errors=remount-ro does.
 */

/* Write the volume superblock, advancing `generation` (§4).
 *
 * The ONE writer of block 0. Everything that records something in the superblock
 * goes through it, so `generation` cannot be advanced by some paths and not
 * others -- and it must advance on every write, because §5's backup scan takes
 * the valid copy carrying the highest one. A generation that never moved left a
 * backup indistinguishable from a current primary.
 *
 * `ctx->state` is recorded always; `ctx->set_counters` also writes the volume's
 * free counters; `ctx->refresh_backups` propagates the state and generation to
 * the backup copies §5's scan can reach. A backup that cannot be written is
 * passed over rather than failing the write the primary already took: the primary
 * is what a mount reads first.
 *
 * Refuses a read-only volume -- writing one would compound whatever made it
 * read-only.
 *
 * Context: wfs_sb_ctx_t.
 */
int32_t wfs_super_write_task(void* user, uintptr_t* out_value);

/* Reconcile the superblock with the volume and record how it is being left (§4).
 *
 * Writes the free counters -- which is what makes them true on disk at all,
 * since they are adjusted in memory as blocks and objects move and are NEVER
 * written per transaction. §4 calls them derived and advisory: the bitmaps are
 * authoritative and land inside the transaction that moves them, and fsck
 * recomputes the counters from those. A sync is what saves fsck the work; a
 * crash simply leaves them trailing.
 *
 * `ctx->state` is what the volume is left saying. WFS_STATE_DIRTY keeps it
 * mounted for writing; WFS_STATE_CLEAN is the unmount, and is the only thing
 * that tells the next mount its log holds nothing to replay (§15). A transition
 * carries the backup copies with it.
 *
 * Completes without a write when there is nothing to reconcile and no transition
 * to record. Fails with WASMOS_ERR_FS_READ_ONLY on a volume that does not permit
 * writes.
 *
 * Context: wfs_sync_ctx_t.
 */
int32_t wfs_sync_task(void* user, uintptr_t* out_value);

#endif /* FS_WFS_WFS_SYNC_H */
