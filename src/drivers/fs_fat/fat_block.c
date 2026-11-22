/* fat_block.c - single-buffer block-I/O layer.  See fat_block.h.  State is
 * caller-owned (fat_block_t); no globals.  The driver issues no PIO or DMA
 * itself (that is the block server's job).  Staged transfers are one 512-byte
 * sector each and blk->sector is a 1-sector cache tagged by loaded_lba; only
 * fat_block_read_direct moves more than a sector, and it lands in the client's
 * buffer without touching the cache. */
#include "fat_block.h"

#include <string.h>

#include "wasmos/api.h"
#include "wasmos/libsys.h" /* wasmos_sys_ipc_report_discard */
#include "wasmos_driver_abi.h"

void fat_block_configure(fat_block_t* blk, int32_t block_endpoint, int32_t reply_endpoint,
                         uint32_t target) {
    blk->sector_bytes = FAT_SECTOR_SIZE;
    /* The FSInfo accounting starts unknown: a caller that did not zero the
     * struct would otherwise present a garbage free count as valid and write it
     * back to the volume on the first allocation. */
    blk->free_count = 0;
    blk->free_count_valid = 0;
    blk->block_endpoint = block_endpoint;
    blk->reply_endpoint = reply_endpoint;
    blk->follow_endpoint = -1;
    blk->follow_bid = -1;
    blk->req_borrow = -1;
    blk->target = target;
    blk->req_bid = -1;
    blk->buf_phys = -1;
    blk->next_req_id = 1;
    blk->cur_req_id = 0;
    blk->wait_lba = 0;
    blk->wait_resp_type = 0;
    blk->copy_into_sector = 0;
    blk->direct_read = 0;
    blk->direct_pending = 0;
    blk->direct_sectors = 0;
    blk->write_pending = 0;
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    blk->cur_req_type = 0;
    memset(&blk->cur_req, 0, sizeof(blk->cur_req));
    blk->held = 0u;
    blk->hold_logged = 0u;
    blk->gone = 0u;
    blk->hold_deadline = 0u;
    blk->owner = 0;
}

int fat_block_setup(fat_block_t* blk) {
    blk->buf_phys = wasmos_block_buffer_phys();
    if (blk->buf_phys < 0) {
        return -1;
    }
    /* Acquire and lend the request buffer ONCE. Doing it per transfer would be
     * the expensive shape people assume a descriptor forces; it is not the shape
     * the descriptor asks for, and a re-grant would fail ALREADY_BORROWED
     * anyway. */
    blk->req_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(wasmos_block_request_t));
    if (blk->req_bid < 0) {
        return -1;
    }
    blk->req_borrow =
        wasmos_xfer_buffer_borrow(blk->block_endpoint, blk->req_bid, WASMOS_BUFFER_GRANT_READ);
    if (blk->req_borrow < 0) {
        blk->req_borrow = -1;
        (void)wasmos_xfer_buffer_release(blk->req_bid);
        blk->req_bid = -1;
        return -1;
    }
    return 0;
}

/* fat_block_server_ready's answer when no server is there but the hold window
 * is open: the transfer is staged and held rather than sent or failed. */
#define FAT_BLOCK_HOLD 1

/* Ticks in the hold window, rounded up as the kernel rounds a millisecond
 * window, so the hold is never shorter than WASMOS_BLOCK_HOLD_MS. */
static uint32_t fat_block_hold_ticks(void) {
    return (uint32_t)(((uint64_t)WASMOS_BLOCK_HOLD_MS * WASMOS_SCHED_TICK_HZ + 999u) / 1000u);
}

/* 1 once the hold window of the current outage has closed. Compared as a
 * signed difference, so it holds across the tick counter's wrap. */
static int fat_block_window_closed(const fat_block_t* blk) {
    return (int32_t)((uint32_t)wasmos_sched_ticks() - blk->hold_deadline) >= 0;
}

static int32_t fat_block_next_id(fat_block_t* blk) {
    int32_t id = blk->next_req_id++;
    if (blk->next_req_id < 1) {
        blk->next_req_id = 1;
    }
    return id;
}

/* End the outstanding transfer as failed with `err`. Returns its op. */
static fat_op_ctx_t* fat_block_fail_current(fat_block_t* blk, int32_t err) {
    blk->cur_req_id = 0;
    blk->held = 0u;
    blk->direct_read = 0u;
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    fat_block_set_err(blk, err);
    return blk->owner;
}

/* Park the outstanding transfer until a server is there. It takes a fresh id:
 * the instance that ended may still have answered the old one, and that reply
 * must not complete the resend. */
static void fat_block_hold(fat_block_t* blk) {
    blk->cur_req_id = fat_block_next_id(blk);
    blk->held = 1u;
    if (!blk->hold_logged) {
        blk->hold_logged = 1u;
        (void)printf("[fat] holding a block transfer until the disk driver returns\n");
    }
}

/* Stage cur_req and hand its buffer to the server. Returns 0, or -1 when the
 * request could not be written or sent. */
static int fat_block_submit(fat_block_t* blk) {
    if (blk->req_bid < 0 ||
        wasmos_xfer_buffer_write(blk->req_bid, &blk->cur_req, (int32_t)sizeof(blk->cur_req), 0) !=
            0) {
        return -1;
    }
    return wasmos_ipc_send(blk->block_endpoint,
                           blk->reply_endpoint,
                           blk->cur_req_type,
                           blk->cur_req_id,
                           blk->req_bid,
                           0,
                           (int32_t)sizeof(blk->cur_req),
                           0);
}

/* Send the held transfer to the server just bound. A direct read names a grant
 * of the client's buffer, and the ended instance's grant ended with it, so the
 * buffer is reborrowed to the successor first, and the op keeps the new grant
 * for its later reads. Returns the op to fail when the resend could not be
 * made, else NULL. */
static fat_op_ctx_t* fat_block_resend(fat_block_t* blk) {
    if (!blk->held || blk->block_endpoint < 0) {
        return 0;
    }
    if (blk->direct_read) {
        fat_op_ctx_t* op = blk->owner;
        int32_t borrow;
        if (!op || op->arg3 <= 0) {
            return fat_block_fail_current(blk, WASMOS_ERR_FS_BUFFER);
        }
        if (op->zc_borrow > 0) {
            (void)wasmos_xfer_buffer_unborrow(op->zc_borrow);
        }
        borrow =
            wasmos_xfer_buffer_reborrow(blk->block_endpoint, op->arg3, WASMOS_BUFFER_GRANT_WRITE);
        if (borrow <= 0) {
            op->zc_borrow = -1;
            return fat_block_fail_current(blk, WASMOS_ERR_FS_BUFFER);
        }
        op->zc_borrow = borrow;
        blk->cur_req.dst_borrow_id = borrow;
    }
    blk->held = 0u;
    if (fat_block_submit(blk) != 0) {
        return fat_block_fail_current(blk, WASMOS_ERR_FS_IO);
    }
    return 0;
}

/* The server ended: nothing is sent to its endpoint again, and the hold window
 * opens. A layer that follows its server holds a transfer the server never
 * answered and returns NULL; one that does not fails it, returning its op with
 * err set. NULL also when nothing was outstanding. */
static fat_op_ctx_t* fat_block_server_gone(fat_block_t* blk) {
    if (blk->block_endpoint < 0) {
        return 0;
    }
    blk->block_endpoint = -1;
    blk->hold_deadline = (uint32_t)wasmos_sched_ticks() + fat_block_hold_ticks();
    if (blk->cur_req_id == 0 || blk->held) {
        return 0;
    }
    /* A read the server never answered may have half-landed in the block
     * buffer; nothing it holds is known good. */
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    if (blk->follow_endpoint < 0) {
        return fat_block_fail_current(blk, WASMOS_ERR_BLOCK_DEV_GONE);
    }
    fat_block_hold(blk);
    return 0;
}

/* A new instance serves the disk at `endpoint`: lend it the request buffer. The
 * previous grant is revoked first; a buffer is lent to one borrower at a
 * time. */
static void fat_block_rebind(fat_block_t* blk, int32_t endpoint) {
    if (blk->req_borrow >= 0) {
        (void)wasmos_xfer_buffer_unborrow(blk->req_borrow);
        blk->req_borrow = -1;
    }
    blk->block_endpoint = -1;
    blk->req_borrow = wasmos_xfer_buffer_borrow(endpoint, blk->req_bid, WASMOS_BUFFER_GRANT_READ);
    if (blk->req_borrow < 0) {
        blk->req_borrow = -1;
        return;
    }
    blk->block_endpoint = endpoint;
    blk->gone = 0u;
    blk->hold_logged = 0u;
}

/* Act on the follow-endpoint message just received. SVC_IPC_CLASS_EVENT for
 * `block`: arg0 the kind, arg1 the instance, arg2 the provider's endpoint. One
 * instance has one live provider, so an ADD naming another endpoint is a new
 * instance of the server, and what the layer holds is resent to it. Returns
 * the op of a transfer that could not be resent, or NULL. */
static fat_op_ctx_t* fat_block_on_follow(fat_block_t* blk) {
    int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t kind = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    uint32_t instance = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    int32_t provider = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    fat_op_ctx_t* failed = 0;

    if (type == SVC_IPC_SUBSCRIBE_CLASS_RESP || type == SVC_IPC_ERROR) {
        if (blk->follow_bid >= 0) {
            (void)wasmos_xfer_buffer_release(blk->follow_bid);
            blk->follow_bid = -1;
        }
        if (type == SVC_IPC_ERROR) {
            (void)printf("[fat] block class subscription refused; a restarted disk driver "
                         "is not followed\n");
        }
        return 0;
    }
    if (type != SVC_IPC_CLASS_EVENT || instance != blk->target) {
        return 0;
    }
    if (kind == SVC_CLASS_EVENT_REMOVE) {
        if (provider == blk->block_endpoint) {
            failed = fat_block_server_gone(blk);
        }
    } else if (kind == SVC_CLASS_EVENT_ADD && provider >= 0 && provider != blk->block_endpoint) {
        failed = fat_block_server_gone(blk);
        fat_block_rebind(blk, provider);
        if (!failed) {
            failed = fat_block_resend(blk);
        }
    }
    return failed;
}

/* 0 when a live instance serves the disk; FAT_BLOCK_HOLD when none does but
 * the layer follows its server and the hold window is open; otherwise
 * WASMOS_ERR_BLOCK_DEV_GONE, which also marks the disk gone once the window has
 * closed.
 *
 * The await registered here also makes the server's end reach reply_endpoint
 * if it never answers. Its refusal with NOENT says the endpoint has no live
 * owner already -- an end whose hangup is still queued -- so nothing is sent to
 * an endpoint id anyone may own next. With no live server, the follow events
 * queued so far are read first, since they may name its successor. Called with
 * nothing outstanding, so no op can fail here. */
static int32_t fat_block_server_ready(fat_block_t* blk) {
    if (blk->block_endpoint >= 0 &&
        wasmos_ipc_peer_await(blk->reply_endpoint, blk->block_endpoint) == WASMOS_NOENT) {
        (void)fat_block_server_gone(blk);
    }
    if (blk->block_endpoint < 0 && blk->follow_endpoint >= 0) {
        while (wasmos_ipc_try_recv(blk->follow_endpoint) > 0) {
            (void)fat_block_on_follow(blk);
        }
    }
    if (blk->block_endpoint >= 0) {
        return 0;
    }
    if (blk->follow_endpoint < 0 || blk->gone) {
        return WASMOS_ERR_BLOCK_DEV_GONE;
    }
    if (fat_block_window_closed(blk)) {
        blk->gone = 1u;
        return WASMOS_ERR_BLOCK_DEV_GONE;
    }
    return FAT_BLOCK_HOLD;
}

/* Send the request just staged in cur_req, or hold it when `ready` is
 * FAT_BLOCK_HOLD. Returns 0, or WASMOS_ERR_FS_IO when it could not be sent. */
static int32_t fat_block_dispatch(fat_block_t* blk, int32_t ready) {
    if (ready == FAT_BLOCK_HOLD) {
        fat_block_hold(blk);
        return 0;
    }
    if (fat_block_submit(blk) != 0) {
        blk->cur_req_id = 0;
        return WASMOS_ERR_FS_IO;
    }
    return 0;
}

int32_t fat_block_follow(fat_block_t* blk, int32_t proc_endpoint, int32_t follow_endpoint) {
    static const char k_class[] = "block";
    int32_t bid = wasmos_xfer_stage(k_class, (int32_t)sizeof(k_class));
    if (bid < 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_ipc_send(proc_endpoint,
                        follow_endpoint,
                        SVC_IPC_SUBSCRIBE_CLASS_REQ,
                        1,
                        follow_endpoint,
                        bid,
                        0,
                        0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_FS_IO;
    }
    blk->follow_bid = bid;
    blk->follow_endpoint = follow_endpoint;
    return 0;
}

fat_op_ctx_t* fat_block_event(fat_block_t* blk, int* out_ok) {
    if (out_ok) {
        *out_ok = 0;
    }
    if (blk->follow_endpoint < 0 || wasmos_ipc_try_recv(blk->follow_endpoint) <= 0) {
        return 0;
    }
    return fat_block_on_follow(blk);
}

int fat_block_holding(const fat_block_t* blk) {
    return blk->held;
}

fat_op_ctx_t* fat_block_expire(fat_block_t* blk, int* out_ok) {
    if (out_ok) {
        *out_ok = 0;
    }
    if (!blk->held || blk->block_endpoint >= 0 || !fat_block_window_closed(blk)) {
        return 0;
    }
    blk->gone = 1u;
    return fat_block_fail_current(blk, WASMOS_ERR_BLOCK_DEV_GONE);
}

uint8_t* fat_block_sector(fat_block_t* blk) {
    return blk->sector;
}

int fat_block_idle(const fat_block_t* blk) {
    return blk->cur_req_id == 0;
}

void fat_block_set_owner(fat_block_t* blk, fat_op_ctx_t* ctx) {
    blk->owner = ctx;
}

void fat_block_set_err(fat_block_t* blk, int32_t err) {
    if (blk->owner) {
        blk->owner->err = err;
    }
}

void fat_block_invalidate(fat_block_t* blk) {
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
}

void fat_block_set_free_count(fat_block_t* blk, uint32_t count) {
    if (!blk) {
        return;
    }
    blk->free_count = count;
    blk->free_count_valid = 1;
}

int fat_block_set_sector_bytes(fat_block_t* blk, uint32_t bytes) {
    if (!blk || bytes < FAT_SECTOR_SIZE || bytes > FAT_MAX_SECTOR_BYTES ||
        (bytes % FAT_SECTOR_SIZE) != 0) {
        return -1;
    }
    blk->sector_bytes = bytes;
    return 0;
}

/* Submit one block transfer of blk->sector_bytes (count = 1), or hold it while
 * the disk's driver restarts.  Returns 0; WASMOS_ERR_BLOCK_DEV_GONE when no
 * instance serves the disk and none is waited for; or WASMOS_ERR_FS_IO when the
 * layer is unconfigured, when staging the write into the block buffer failed,
 * or when the request could not be sent. */
static int32_t fat_block_start(fat_block_t* blk, uint32_t lba, int rw) {
    int32_t rc;

    if (blk->reply_endpoint < 0 || blk->buf_phys < 0) {
        return WASMOS_ERR_FS_IO;
    }
    rc = fat_block_server_ready(blk);
    if (rc < 0) {
        return rc;
    }
    if (rw == 1 /* write */ &&
        wasmos_block_buffer_write(
            blk->buf_phys, addr_cast(int32_t, blk->sector), (int32_t)blk->sector_bytes, 0) != 0) {
        return WASMOS_ERR_FS_IO;
    }

    blk->cur_req_id = fat_block_next_id(blk);
    blk->wait_lba = lba;
    blk->wait_resp_type = rw ? BLOCK_IPC_WRITE_RESP : BLOCK_IPC_READ_RESP;
    blk->copy_into_sector = rw ? 0u : 1u;
    blk->cur_req_type = rw ? BLOCK_IPC_WRITE_REQ : BLOCK_IPC_READ_REQ;

    memset(&blk->cur_req, 0, sizeof(blk->cur_req));
    blk->cur_req.version = BLOCK_REQUEST_VERSION;
    blk->cur_req.target = blk->target;
    blk->cur_req.lba = lba;
    blk->cur_req.sector_count = 1u;
    blk->cur_req.dst_kind = BLOCK_DST_BLOCK_BUFFER;
    blk->cur_req.dst_phys = (uint32_t)blk->buf_phys;
    return fat_block_dispatch(blk, rc);
}

uint32_t fat_block_direct_sectors(const fat_block_t* blk) {
    return blk->direct_sectors;
}

int32_t fat_block_server_endpoint(const fat_block_t* blk) {
    return blk->block_endpoint;
}

fat_r_t fat_block_read_direct(fat_block_t* blk, uint32_t lba, uint32_t count, int32_t buffer_id,
                              int32_t borrow_id, uint32_t dst_offset) {
    /* The coroutine macros re-invoke this on resume (the case label sits before
     * the call), so completion must be reported on the second entry — the same
     * yield-once contract fat_block_write uses. fat_need_sector gets away
     * without a flag only because its sector cache answers the repeat call. */
    if (blk->direct_pending) {
        blk->direct_pending = 0;
        return FAT_R_DONE;
    }
    if (blk->reply_endpoint < 0 || buffer_id < 0 || count == 0 ||
        count > WASMOS_BLOCK_ZC_MAX_SECTORS) {
        fat_block_set_err(blk, WASMOS_ERR_FS_BAD_ARGS);
        return FAT_R_ERR;
    }
    int32_t rc = fat_block_server_ready(blk);
    if (rc < 0) {
        fat_block_set_err(blk, rc);
        return FAT_R_ERR;
    }
    blk->cur_req_id = fat_block_next_id(blk);
    blk->wait_lba = lba;
    blk->wait_resp_type = BLOCK_IPC_READ_RESP;
    /* Nothing lands in the staged sector, so do not pull it on completion and do
     * not retag the cache: whatever it held before is still what it holds. */
    blk->copy_into_sector = 0u;
    blk->direct_read = 1u;

    /* Same opcode as a staged read; only the destination differs. The borrow is
     * carried so the server can map the destination for device DMA, and a server
     * that only copies ignores it. */
    blk->cur_req_type = BLOCK_IPC_READ_REQ;
    memset(&blk->cur_req, 0, sizeof(blk->cur_req));
    blk->cur_req.version = BLOCK_REQUEST_VERSION;
    blk->cur_req.target = blk->target;
    blk->cur_req.lba = lba;
    blk->cur_req.sector_count = count;
    blk->cur_req.dst_kind = BLOCK_DST_XFER_BUFFER;
    blk->cur_req.dst_buffer_id = buffer_id;
    blk->cur_req.dst_borrow_id = borrow_id;
    blk->cur_req.dst_offset = dst_offset;
    if (fat_block_dispatch(blk, rc) != 0) {
        blk->direct_read = 0u;
        fat_block_set_err(blk, WASMOS_ERR_FS_IO);
        return FAT_R_ERR;
    }
    blk->direct_pending = 1u;
    return FAT_R_WAIT;
}

fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba) {
    if (blk->loaded_lba == lba) {
        return FAT_R_DONE; /* cache hit */
    }
    int32_t rc = fat_block_start(blk, lba, 0);
    if (rc != 0) {
        fat_block_set_err(blk, rc);
        return FAT_R_ERR;
    }
    return FAT_R_WAIT;
}

fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba) {
    if (blk->write_pending) {
        blk->write_pending = 0; /* resumed: the write completed */
        return FAT_R_DONE;
    }
    int32_t rc = fat_block_start(blk, lba, 1);
    if (rc != 0) {
        fat_block_set_err(blk, rc);
        return FAT_R_ERR;
    }
    blk->write_pending = 1;
    return FAT_R_WAIT;
}

void fat_block_release(fat_block_t* blk, fat_op_ctx_t* ctx) {
    if (blk->owner == ctx) {
        blk->owner = 0;
        /* An op torn down mid-flight (a failed completion) must not leave a
         * yield-once latch armed, or the next op's first direct read or write
         * would report completion without ever submitting one. */
        blk->direct_pending = 0u;
        blk->direct_read = 0u;
        blk->write_pending = 0u;
    }
}

fat_op_ctx_t* fat_block_complete(fat_block_t* blk, int* out_ok) {
    fat_op_ctx_t* owner = blk->owner;
    int32_t rtype, rreq, rstatus;

    if (out_ok) {
        *out_ok = 0;
    }
    if (wasmos_ipc_select_one(blk->reply_endpoint) < 0) {
        return 0; /* spurious wake */
    }
    rtype = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    rreq = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    rstatus = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);

    /* WASMOS_IPC_HANGUP: arg0 names the endpoint whose owner ended. Owed for a
     * server this layer has sent to, and for the one it last awaited. */
    if (rtype == WASMOS_IPC_HANGUP) {
        return rstatus == blk->block_endpoint ? fat_block_server_gone(blk) : 0;
    }

    if (blk->cur_req_id == 0 || rreq != blk->cur_req_id) {
        /* Consumed and dropped.  On this path that is a block reply nobody will
         * ever see again, and the operation waiting for it stalls the whole FS
         * chain behind it -- so it is reported rather than silently returned. */
        wasmos_ipc_message_t dropped;
        wasmos_ipc_message_read_last(&dropped);
        wasmos_sys_ipc_report_discard(
            "fs-fat/block", blk->reply_endpoint, blk->cur_req_id, &dropped);
        return 0; /* stale/unmatched reply */
    }
    blk->cur_req_id = 0;

    /* A direct read never touched the staged sector, so its outcome says nothing
     * about the cache either way — leave loaded_lba exactly as it was. */
    if (blk->direct_read) {
        int32_t rsectors = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        uint8_t ok = (rtype == blk->wait_resp_type && rstatus == 0 && rsectors > 0) ? 1u : 0u;
        blk->direct_sectors = ok ? (uint32_t)rsectors : 0u;
        blk->direct_read = 0u;
        if (out_ok) {
            *out_ok = ok;
        }
        return owner;
    }

    if (rtype == BLOCK_IPC_ERROR || rstatus != 0 || rtype != blk->wait_resp_type) {
        blk->loaded_lba = FAT_BLOCK_NO_LBA;
        return owner; /* out_ok stays 0 */
    }
    if (blk->copy_into_sector &&
        wasmos_block_buffer_copy(
            blk->buf_phys, addr_cast(int32_t, blk->sector), (int32_t)blk->sector_bytes, 0) != 0) {
        blk->loaded_lba = FAT_BLOCK_NO_LBA;
        return owner;
    }
    blk->loaded_lba = blk->wait_lba; /* sector now holds this lba (read or write) */
    if (out_ok) {
        *out_ok = 1;
    }
    return owner;
}
