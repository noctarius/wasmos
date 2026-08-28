/* wfs_journal.h - the write-ahead metadata journal (§14).
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §14, §17, §18.
 *
 * A metadata operation runs inside a TRANSACTION. Every block the operation
 * changes is written to the log first and only reaches its real address once the
 * transaction commits, so a crash leaves either all of the operation's metadata
 * or none of it. File DATA is not journaled: after a crash the metadata is
 * consistent, but a block newly allocated to a file may hold what it held
 * before (§17).
 *
 * The sequence a transaction runs is §14's, in order:
 *
 *   1 write the descriptor block and the block images
 *   2 barrier
 *   3 write the COMMIT block
 *   4 barrier
 *   5 checkpoint: write each image to its target block
 *   6 barrier
 *   7 advance the log tail past the transaction
 *
 * A crash before step 3 discards the transaction; a crash between 3 and 7
 * replays it (wfs_recover.h). Steps 1 and 3 are inverted here only in the order
 * blocks are PRODUCED: an image is written to the log as the caller stages it,
 * and the descriptor naming all of them is written at commit, still before the
 * COMMIT block and its barrier. What step 1 requires is that both are durable
 * before step 3, not which of them is written first.
 *
 * THE BARRIER IS REQUEST ORDERING, NOT A CACHE FLUSH. Every step here awaits its
 * block reply before issuing the next request, so the device sees the writes in
 * order; nothing makes a device with a volatile write cache commit them to
 * media.
 * TODO: the block ABI has no flush (abi/opcodes.yaml, BLOCK_IPC_*). Until
 * BLOCK_IPC_FLUSH_REQ exists, a device that reorders across its own cache can
 * defeat steps 2, 4 and 6.
 *
 * Reads inside an open transaction see the transaction's own writes: the block
 * layer's redirect (wfs_block_set_redirect) maps a journaled target to the log
 * block holding its image, which is installed at begin and cleared at commit.
 */
#ifndef FS_WFS_WFS_JOURNAL_H
#define FS_WFS_WFS_JOURNAL_H

#include "wfs_types.h"

/* Log blocks a transaction of `targets` targets occupies: the descriptor, one
 * image per target, an optional revoke record, and the commit. The journal
 * superblock is block 0 of the region and is not part of the log. */
#define WFS_TXN_LOG_BLOCKS(targets, revokes) (1u + (targets) + ((revokes) ? 1u : 0u) + 1u)

/* The log block a transaction's descriptor occupies, journal-relative.
 *
 * It is a constant rather than a cursor because this driver retires each
 * transaction before the next begins, so the tail never moves off the first log
 * block. A journal superblock naming any other `first_block` was written by
 * something else, and wfs_journal_load_task refuses it rather than replaying
 * from an offset it cannot interpret. */
#define WFS_TXN_DESCRIPTOR_BLOCK 1u

/* Read and validate the journal superblock into vol->journal (§14).
 *
 * Fails with WASMOS_ERR_FS_JOURNAL when the region does not hold a log this
 * driver can transact in: wrong magic or version, a checksum that does not
 * verify, a block size disagreeing with the volume's, or a log too short for one
 * full-sized transaction. A damaged log costs the volume its WRITABILITY, not
 * its readability, which is why this is distinct from WASMOS_ERR_FS_CORRUPT.
 *
 * Context: wfs_jload_ctx_t.
 */
int32_t wfs_journal_load_task(void* user, uintptr_t* out_value);

/* Open a transaction on `vol`. Pure: no block is touched until the first stage.
 *
 * Fails with WASMOS_ERR_FS_JOURNAL when the log has not been loaded,
 * WASMOS_ERR_FS_READ_ONLY on a volume that does not permit writes, and
 * WASMOS_ERR_FS_BUSY when a transaction is already open — this driver runs one
 * at a time (wfs_journal_t).
 */
wasmos_error_code_t wfs_txn_begin(wfs_volume_t* vol);

/* Record that `block` stops being metadata in this transaction (§18).
 *
 * Mandatory whenever a metadata block is freed, whether or not it is reallocated
 * in the same transaction. The log records block NUMBERS rather than what a
 * block holds, so an older committed image of a freed block stays replayable
 * once the block has been handed to a file, and replaying it would overwrite
 * live file data with stale metadata.
 *
 * Pure. Fails with WASMOS_ERR_FS_TXN_FULL past WFS_TXN_MAX_REVOKES.
 */
wasmos_error_code_t wfs_txn_revoke(wfs_volume_t* vol, uint32_t block);

/* Journal the STAGED block as the new content of `target`, and take the result.
 *
 * The begin/take pair of wfs_block_write_begin/wfs_block_take, and deliberately
 * the same shape: converting a metadata write to a journaled one is then the
 * swap of one call for another, with the awaits and resume points already in
 * place. A DATA write keeps using the block layer directly, because file data is
 * not journaled (§17).
 *
 * The caller has just built the block's new content in wfs_block_data(); the
 * begin writes it into the log and records the target, its log block and the
 * image's checksum for the descriptor. The block itself is not touched until the
 * transaction checkpoints.
 *
 * Staging the same target twice REPLACES its image rather than adding a second
 * target, so a read-modify-write repeated inside one transaction costs one log
 * block and leaves one image for recovery to apply. That is what keeps a
 * transaction's target count proportional to the BLOCKS it touches rather than
 * to the number of times it touches them.
 *
 * The begin returns NULL when there is nothing to await, which covers a refusal
 * as well as a failed stage; wfs_txn_stage_take reports either, so the ordinary
 * await-then-take sequence is correct without the caller testing the future. Any
 * failure ABORTS the transaction, because a caller that continued would commit a
 * transaction missing one of its blocks.
 *
 * Refuses a volume that is not yet marked WFS_STATE_DIRTY on disk. That flag is
 * what makes the next mount replay at all, so a log written before it lands
 * would be skipped by a mount reading a CLEAN volume -- and a transaction whose
 * checkpoint had half run would then never be finished.
 */
wasmos_future_t* wfs_txn_stage_begin(wfs_volume_t* vol, uint32_t target);
wasmos_error_code_t wfs_txn_stage_take(wfs_volume_t* vol);

/* Commit and retire the open transaction: §14 steps 1 (descriptor) through 7.
 *
 * On return the transaction is closed, every image is at its target block, and
 * the log tail names the next transaction. A failure at any step aborts the
 * transaction; a failure before the COMMIT block lands leaves nothing
 * replayable, and one after it leaves the transaction for recovery to finish.
 *
 * Context: wfs_txcommit_ctx_t.
 */
int32_t wfs_txn_commit_task(void* user, uintptr_t* out_value);

/* Open a transaction around one operation, and close it.
 *
 * The pair every metadata operation in this driver is written between. `open`
 * marks the volume WFS_STATE_DIRTY on disk before the transaction begins --
 * §14's log is only consulted by a mount that sees that flag -- and `close`
 * commits and checkpoints it. Both drive their tasks through wfs_ops_run, so
 * they are for the plain-sequence callers the operations are written as; a
 * failure from either leaves no transaction open.
 *
 * Failing to close is not the same as aborting: see wfs_txn_commit_task for what
 * a failure after the COMMIT block lands means for the volume.
 */
wasmos_error_code_t wfs_txn_open(wfs_volume_t* vol);
wasmos_error_code_t wfs_txn_close(wfs_volume_t* vol);

/* Abandon the open transaction without committing.
 *
 * The log keeps whatever was written into it; with no COMMIT block carrying the
 * sequence, recovery discards it (§14). Safe to call with no transaction open.
 */
void wfs_txn_abort(wfs_volume_t* vol);

/* Whether a transaction is open on `vol`. */
int wfs_txn_is_open(const wfs_volume_t* vol);

/* Build the journal superblock into `image`, naming `first_sequence` as the log
 * tail, and seal it. `image` is a whole block and is zeroed first: the log
 * superblock occupies its region alone, so nothing else in the block is defined.
 *
 * Shared with wfs_recover.c, which advances the same tail after a replay, so
 * there is one definition of the record rather than two that can drift.
 */
void wfs_journal_build_super(const wfs_volume_t* vol, uint8_t* image, uint64_t first_sequence);

/* Seal a journal block: zero its checksum field, checksum the WHOLE block,
 * store the result.
 *
 * The checksum covers the whole block rather than the fixed header because a
 * descriptor's targets and a revoke's block list follow that header and are the
 * part recovery acts on; a header-only checksum would leave them unprotected.
 * Seeded with the block's own number (§13), so a log block that lands at the
 * wrong offset fails to verify instead of reading as a neighbour.
 *
 * Exposed for wfs_recover.c, which verifies with the same rule, and for the host
 * suites, which build log blocks to replay.
 */
void wfs_journal_seal(const uint8_t uuid[WFS_UUID_LEN], uint32_t block, uint8_t* image,
                      uint32_t block_size);

/* Whether `image` verifies as the journal block numbered `block`. */
int wfs_journal_verify(const uint8_t uuid[WFS_UUID_LEN], uint32_t block, const uint8_t* image,
                       uint32_t block_size);

#endif /* FS_WFS_WFS_JOURNAL_H */
