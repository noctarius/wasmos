/* wfs_block.h - block-I/O layer for the WFS reactor.
 *
 * fs_wfs is a block CLIENT: it stages one filesystem block in a dedicated
 * buffer and asks a block server to fill it. Whether that server uses PIO or
 * DMA is its decision and opaque here.
 *
 * The reactor drives one ACTIVE op at a time, so at most one block request is
 * outstanding. All of that singleton state — the request in flight, the staged
 * block and its cache tag, and the op to resume — lives in the caller-owned
 * wfs_block_t. There are no module globals. The reactor sets b->owner to the
 * active op before stepping it, so a completion knows whom to resume and a
 * failure knows which op to record its code on.
 *
 * The cache tag is what makes WFS_CO_READ cheap: a step may assume the staged
 * block is the one IT last asked for across a yield of its own, but must assume
 * nothing on entry, because the previous op left whatever it left there. A
 * failed completion clears the tag, so an I/O error never leaves stale bytes
 * looking cached.
 *
 * A filesystem block is 4096..16384 bytes while the block device speaks
 * 512-byte sectors, so one filesystem block is several device sectors. That
 * conversion lives here and nowhere else: every layer above addresses
 * filesystem blocks.
 *
 * TESTABILITY. wfs_block_submit_read / _write are the only functions that touch
 * IPC. A host test supplies its own, serving a RAM image synchronously, and
 * links everything above unchanged — which is how the reactor is exercised
 * without an emulator. See tests/unit/test_wfs_mount.c.
 */
#ifndef FS_WFS_WFS_BLOCK_H
#define FS_WFS_WFS_BLOCK_H

#include <stdint.h>

#include "wfs_format.h"
#include "wfs_types.h"

#define WFS_BLOCK_NONE 0xFFFFFFFFu /* staged_block sentinel: nothing staged */

/* Device sector size the block protocol speaks. A filesystem block is a whole
 * number of these. */
#define WFS_SECTOR_BYTES 512u

typedef struct wfs_op wfs_op_t;

typedef struct {
    int32_t block_endpoint; /* the block server */
    int32_t reply_endpoint;
    int32_t buf_phys; /* dedicated block-buffer transfer handle */
    int32_t next_req_id;

    uint32_t block_size; /* filesystem block size, from the superblock */

    /* Single outstanding request. */
    int32_t cur_req_id;    /* outstanding request id, 0 when idle */
    uint32_t wait_block;   /* the block that request is for */
    uint8_t wait_is_write; /* the outstanding request is a write */

    /* The staged block, tagged by the block number it holds. */
    uint32_t staged_block; /* WFS_BLOCK_NONE when nothing is staged */
    uint8_t data[WFS_BLOCK_SIZE_MAX];

    /* A completion reported a device error, and the step that was waiting has
     * not yet been told.
     *
     * This flag is what makes a failure reach the step. A failed completion
     * clears the cache tag, so without it the resumed step would find nothing
     * staged, submit the SAME read again, and carry on if the retry happened to
     * succeed — turning a device error into a silent retry that no caller
     * asked for and no caller can bound. The next wfs_block_need or
     * wfs_block_write consumes the flag and reports WFS_R_ERR. */
    uint8_t io_failed;

    /* Where a failing step records its packed code, and which op a completion
     * resumes. */
    wasmos_error_code_t err;
    wfs_op_t* owner;
} wfs_block_t;

/* The staged block. Valid only until the next yield. */
static inline uint8_t* wfs_block_data(wfs_block_t* b) {
    return b->data;
}

void wfs_block_configure(wfs_block_t* b, int32_t block_endpoint, int32_t reply_endpoint);

/* Adopt the volume's block size once the superblock has been read. Refuses
 * anything outside the three permitted sizes, leaving the previous value.
 * Returns WASMOS_ERR_NONE or a packed code.
 *
 * Until this is called the layer stages WFS_SUPER_SIZE-aligned reads of the
 * default block size, which is what the mount path needs to reach a superblock
 * whose block_size it does not yet know. */
wasmos_error_code_t wfs_block_set_block_size(wfs_block_t* b, uint32_t block_size);

/* Discard the staged block. Called when a write elsewhere may have invalidated
 * it, and on any failed completion. */
void wfs_block_invalidate(wfs_block_t* b);

/* Record a failure against the active op. */
void wfs_block_set_err(wfs_block_t* b, wasmos_error_code_t code);

/* Ensure `block` is staged. Returns WFS_R_DONE when it already is (a cache
 * hit costs nothing), WFS_R_WAIT having submitted a read, or WFS_R_ERR. */
wfs_r_t wfs_block_need(wfs_block_t* b, uint32_t block);

/* Push the staged buffer to `block`. Returns WFS_R_WAIT having submitted the
 * write, or WFS_R_ERR. */
wfs_r_t wfs_block_write(wfs_block_t* b, uint32_t block);

/* Called by the reactor when the outstanding request completes. `ok` is zero on
 * a device error. Clears the cache tag on failure so stale bytes never look
 * cached. */
void wfs_block_complete(wfs_block_t* b, int ok);

/* ---- the IPC seam --------------------------------------------------------
 *
 * These two are the whole of this layer's contact with the outside world. The
 * driver defines them over BLOCK_IPC; a host test defines them over a RAM
 * image. Both submit a request and return WFS_R_WAIT, or WFS_R_ERR if the
 * request could not be sent; a test that serves synchronously calls
 * wfs_block_complete itself and still returns WFS_R_WAIT, so the reactor's
 * resume path is the one exercised.
 */
wfs_r_t wfs_block_submit_read(wfs_block_t* b, uint32_t block);
wfs_r_t wfs_block_submit_write(wfs_block_t* b, uint32_t block);

#endif /* FS_WFS_WFS_BLOCK_H */
