/* wfs_block.h - block client for the WFS driver.
 *
 * fs_wfs stages one filesystem block and asks a block server to fill it. A
 * request is a loop intent whose reply settles a future the client owns, and
 * the awaiting task is parked and resumed by the runtime (docs/architecture/32).
 * Nothing here polls or schedules — that is all the runtime's. A device error
 * reaches the caller as a rejection at its await.
 *
 * The client follows a restarted server. The disk is the same disk after its
 * driver restarts, so the volume, its open files and this process carry over:
 * the kernel's hangup or the registry's REMOVE unbinds the client, and the
 * registry's ADD of the disk's instance on a new endpoint binds it there. The
 * client keeps its own promise, because the IPC-future bridge would wait on the
 * dead instance's reply forever.
 *
 * A transfer the dead instance never answered, and one begun while no instance
 * serves the disk, is HELD and resent to the successor unchanged -- a transfer
 * is idempotent per LBA -- so a restart is invisible above this layer. The hold
 * lasts WASMOS_BLOCK_HOLD_MS from the server's end (abi/constants.yaml,
 * block_hold); a server not back by then is taken to be gone, and every
 * transfer fails with WASMOS_ERR_BLOCK_DEV_GONE until one is. Holding does not
 * stall the restart: the successor's module is read through fs-manager, which
 * never waits on a backend (docs/architecture/18-filesystem-stack.md, "Deferred
 * Replies"). Until wfs_block_follow has been sent no successor can be learned
 * of, so nothing is held and such a transfer fails at once.
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
    int32_t block_endpoint; /* -1 while no instance of the server is known */
    int32_t reply_endpoint;
    int32_t buf_id; /* this process's block buffer, by PHYSICAL address */

    /* The disk this client addresses: its `block` class instance, carried in
     * every request. A backend serves several devices on one endpoint, so the
     * endpoint alone no longer says which disk a transfer means. */
    uint32_t target;
    /* Transfer buffer holding the request descriptor, acquired once and
     * borrowed READ to the block endpoint for the client's lifetime. One slot
     * suffices because this layer keeps exactly one request in flight; a slot
     * may not be reused until its reply lands. */
    int32_t req_bid;

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
     * is refused rather than queued. Its descriptor stays in req_bid until it
     * settles, which is what lets a held request be resent unchanged. */
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_ipc_message_t reply;
    int32_t intent_id; /* the request on the wire; 0 when none */
    int32_t req_type;  /* BLOCK_IPC_*_REQ of the in-flight request */
    /* The request buffer's grant to block_endpoint, revoked when the client
     * moves to a new server; -1 when there is none. */
    int32_t req_borrow;
    uint8_t following;   /* wfs_block_follow was sent: a successor is announced */
    uint8_t held;        /* the in-flight request waits for a server; none is sent */
    uint8_t hold_logged; /* the hold of this outage has been reported */
    /* The hold window of the current outage closed with no server back: the
     * disk is taken to be gone until a server registers. */
    uint8_t gone;
    uint32_t hold_deadline; /* sched tick the window closes at, while no server runs */
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

/* Bind the client to a block endpoint and a disk. `buf_id` is this process's
 * block buffer by physical address; `target` is the disk's `block` class
 * instance. Acquires and borrows the request-descriptor buffer, so it may fail:
 * returns 0, or a packed code when that buffer could not be obtained.
 *
 * Takes the loop's WASMOS_IPC_HANGUP and SVC_IPC_CLASS_EVENT handlers, which
 * follow the server across a restart, and its timeout hook (on_timeout,
 * timeout_user, poll_timeout_ms), which ages a held transfer; the loop's owner
 * must not install its own. */
wasmos_error_code_t wfs_block_configure(wfs_block_t* b, wasmos_sys_event_loop_t* loop,
                                        int32_t block_endpoint, int32_t reply_endpoint,
                                        int32_t buf_id, uint32_t target);

/* Subscribe the reply endpoint to `block` class events through the process
 * manager at `proc_endpoint`, as a loop intent, so a new instance of the
 * server is seen when it registers. Returns 0 once the request is on its way,
 * or a packed code. From then on a transfer is held across a restart; before
 * it, a restart is noticed only through the kernel's hangup and every transfer
 * fails with WASMOS_ERR_BLOCK_DEV_GONE. */
wasmos_error_code_t wfs_block_follow(wfs_block_t* b, int32_t proc_endpoint);

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

/* Begin a flush: commit everything already written to durable media.
 *
 * What turns the journal's request ORDERING into a barrier (§14, steps 2, 4 and
 * 6). Awaiting each write's reply before issuing the next only guarantees the
 * device saw them in that order; a volatile write cache can still lose the
 * earlier ones, which is exactly what a crash between the COMMIT block and the
 * checkpoint would expose.
 *
 * Returns the future to await, or NULL when the request could not be sent, which
 * wfs_block_take reports. The staged block is untouched: a flush transfers
 * nothing, so it neither fills nor invalidates the buffer. */
wasmos_future_t* wfs_block_flush_begin(wfs_block_t* b);

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
