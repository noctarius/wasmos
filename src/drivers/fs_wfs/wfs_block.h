/* wfs_block.h - block client for the WFS driver.
 *
 * fs_wfs stages one filesystem block and asks a block server to fill it.
 * Requests ride the SYSTEM IPC-future bridge (wasmos_sys_wasm_ipc_future_*,
 * docs/architecture/32): a request becomes a loop intent, its reply settles a
 * future, and the awaiting task is parked and resumed by the runtime. Nothing
 * here polls, retries, or schedules — that is all the runtime's.
 *
 * Using the bridge rather than a private request/reply protocol is what makes a
 * device error reach the caller: a failed send returns an already-rejected
 * future and a failed transfer rejects on reply, so the awaiting task observes
 * the failure at its await. A hand-rolled layer has to invent a flag for that
 * and gets it wrong the first time.
 *
 * A filesystem block is 4096..16384 bytes while the block protocol speaks
 * 512-byte sectors, so one filesystem block is several sectors. That conversion
 * lives here and nowhere else; every layer above addresses filesystem blocks.
 */
#ifndef FS_WFS_WFS_BLOCK_H
#define FS_WFS_WFS_BLOCK_H

#include <stdint.h>

#include "wasmos/libsys.h"
#include "wfs_status.h"
#include "wfs_format.h"

#define WFS_BLOCK_NONE 0xFFFFFFFFu /* staged_block sentinel: nothing staged */
#define WFS_SECTOR_BYTES 512u

/* Where a read of a filesystem block must actually go.
 *
 * An open metadata transaction holds the new content of its blocks in the
 * journal, not yet at their targets (§14): the images are written to the log and
 * only checkpointed at commit. A reader inside that transaction must see what
 * the transaction wrote, so its reads are mapped target -> journal block here
 * rather than at every call site.
 *
 * Returns the block to read, which is `block` itself when no mapping applies.
 * Reads only: a metadata WRITE inside a transaction goes through the journal by
 * name, and a data write goes straight to its block, so neither is redirected. */
typedef uint32_t (*wfs_block_redirect_fn)(void* user, uint32_t block);

typedef struct {
    wasmos_sys_event_loop_t* loop;
    int32_t block_endpoint;
    int32_t reply_endpoint;
    int32_t buf_id; /* transfer buffer the block server fills */

    uint32_t block_size;

    /* The staged block, tagged by the block number it holds. A step may assume
     * the staged block is the one IT last asked for across its own await, but
     * must assume nothing on entry: another task's step may have run in
     * between. A pointer into wfs_block_data() is valid only until the next
     * await. */
    uint32_t staged_block;
    uint8_t data[WFS_BLOCK_SIZE_MAX];

    /* The one in-flight request. The reactor drives one block operation at a
     * time, so a single record suffices; a second begin while one is in flight
     * is refused rather than queued. */
    wasmos_sys_wasm_ipc_future_t op;
    uint32_t pending_block;
    uint8_t in_flight;
    /* A write whose staging into the server's buffer failed, so no request was
     * sent. Recorded rather than returned, because a NULL future from a begin
     * means "nothing to await" and would otherwise let a write that never
     * happened read back as a success at the take. */
    uint8_t stage_failed;

    /* The read redirect, or NULL when reads address blocks directly. */
    wfs_block_redirect_fn redirect;
    void* redirect_user;
} wfs_block_t;

/* The staged block. Valid only until the next await. */
static inline uint8_t* wfs_block_data(wfs_block_t* b) {
    return b->data;
}

void wfs_block_configure(wfs_block_t* b, wasmos_sys_event_loop_t* loop, int32_t block_endpoint,
                         int32_t reply_endpoint, int32_t buf_id);

/* Adopt the volume's block size once the superblock has been read. Refuses
 * anything outside the three permitted sizes, leaving the previous value.
 *
 * Until this is called the layer transfers WFS_BLOCK_SIZE_MIN bytes, which is
 * what the mount path needs to reach a superblock whose block_size it does not
 * yet know: block 0 begins at byte 0 and the superblock lies wholly inside the
 * first 4096 bytes at every permitted size. */
wasmos_error_code_t wfs_block_set_block_size(wfs_block_t* b, uint32_t block_size);

/* Install the read redirect, or clear it by passing NULL.
 *
 * Installing or clearing one INVALIDATES the staged block: the tag names the
 * block the bytes were read from, and the same tag would answer a differently
 * mapped request afterwards. */
void wfs_block_set_redirect(wfs_block_t* b, wfs_block_redirect_fn fn, void* user);

/* Discard the staged block. */
void wfs_block_invalidate(wfs_block_t* b);

/* Begin staging `block`, which the read redirect may map elsewhere first.
 *
 * Returns NULL when the block is ALREADY STAGED: a cache hit costs no request
 * and the caller proceeds without awaiting. Otherwise returns the future to
 * await, which may already be rejected if the request could not be sent.
 * wfs_block_take() must be called once it settles.
 */
wasmos_future_t* wfs_block_read_begin(wfs_block_t* b, uint32_t block);

/* Begin writing the staged buffer to `block`.
 *
 * Returns the future to await, or NULL when the block could not be staged into
 * the server's buffer and no request was sent. A NULL means the caller has
 * nothing to await, NOT that the write succeeded: wfs_block_take() reports the
 * failure, so the normal await-then-take sequence is correct either way. */
wasmos_future_t* wfs_block_write_begin(wfs_block_t* b, uint32_t block);

/* Consume the settled request. On success the staged buffer holds `block` and
 * the tag names it; on failure the tag is cleared, so an error never leaves the
 * device's leftover bytes looking cached.
 *
 * Returns WASMOS_ERR_NONE or a packed code. Calling this with nothing in flight
 * returns WASMOS_ERR_NONE, so a step that took the cache-hit path may call it
 * unconditionally -- except after a write whose staging failed, which is
 * reported here precisely so it cannot pass for a cache hit.
 */
wasmos_error_code_t wfs_block_take(wfs_block_t* b);

#endif /* FS_WFS_WFS_BLOCK_H */
