/* wfs_block.c - staging, cache-tag bookkeeping and the server binding. */
#include "wfs_block.h"

#include "wasmos/api.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

/* Accept a block reply, reject anything else, which is how BLOCK_IPC_ERROR
 * becomes a rejected await instead of a status the caller must remember to
 * check. */
static int32_t block_reply_status(const wasmos_ipc_message_t* reply) {
    if (!reply) {
        return WASMOS_ERR_FS_IO;
    }
    if (reply->type == BLOCK_IPC_ERROR) {
        return reply->arg0 < 0 ? reply->arg0 : WASMOS_ERR_FS_IO;
    }
    /* Which request this answers is settled by the bridge, which correlates on
     * the request id; the type only has to be a block reply of some kind. */
    if (reply->type != BLOCK_IPC_READ_RESP && reply->type != BLOCK_IPC_WRITE_RESP &&
        reply->type != BLOCK_IPC_FLUSH_RESP) {
        return WASMOS_ERR_FS_IO;
    }
    return 0;
}

/* How often the loop wakes to age a held transfer. The hold window is
 * WASMOS_BLOCK_HOLD_MS, so this bounds how far past it a held one is failed. */
#define WFS_BLOCK_HOLD_POLL_MS 500

/* Ticks in the hold window, rounded up as the kernel rounds a millisecond
 * window, so the hold is never shorter than WASMOS_BLOCK_HOLD_MS. */
static uint32_t wfs_block_hold_ticks(void) {
    return (uint32_t)(((uint64_t)WASMOS_BLOCK_HOLD_MS * WASMOS_SCHED_TICK_HZ + 999u) / 1000u);
}

/* 1 once the hold window of the current outage has closed. Compared as a
 * signed difference, so it holds across the tick counter's wrap. */
static int wfs_block_window_closed(const wfs_block_t* b) {
    return (int32_t)((uint32_t)wasmos_sched_ticks() - b->hold_deadline) >= 0;
}

/* Park the in-flight request until a server is there. The loop wakes on a
 * timer meanwhile, so the window is aged even when nothing arrives. */
static void wfs_block_hold(wfs_block_t* b) {
    b->intent_id = 0;
    b->held = 1u;
    b->loop->poll_timeout_ms = WFS_BLOCK_HOLD_POLL_MS;
    if (!b->hold_logged) {
        b->hold_logged = 1u;
        (void)printf("[fs-wfs] holding a block transfer until the disk driver returns\n");
    }
}

static void wfs_block_unhold(wfs_block_t* b) {
    b->held = 0u;
    b->loop->poll_timeout_ms = 0;
}

static void wfs_block_fail(wfs_block_t* b, int32_t code) {
    b->intent_id = 0;
    if (b->held) {
        wfs_block_unhold(b);
    }
    (void)wasmos_promise_reject(&b->promise, code);
}

static void on_reply(void* user, const wasmos_ipc_message_t* reply) {
    wfs_block_t* b = (wfs_block_t*)user;
    int32_t status = block_reply_status(reply);

    b->intent_id = 0;
    b->reply = *reply;
    if (status == 0) {
        (void)wasmos_promise_resolve(&b->promise, (uintptr_t)&b->reply);
    } else {
        (void)wasmos_promise_reject(&b->promise, status);
    }
}

/* Deliver what is already queued on the reply endpoint -- a hangup, class
 * events -- without waiting for more. While the client is idle nothing polls
 * the loop, so a server's death and its successor's registration wait there;
 * a request that is about to fail for want of a server reads them first. A
 * bounded park of 1 ms is the loop's non-blocking form. */
static void wfs_block_drain(wfs_block_t* b) {
    int32_t timeout = b->loop->poll_timeout_ms;
    b->loop->poll_timeout_ms = 1;
    (void)wasmos_sys_event_loop_poll(b->loop, 8);
    b->loop->poll_timeout_ms = timeout;
}

static void wfs_block_server_gone(wfs_block_t* b);

/* 1 when block_endpoint has a live owner. The await registered here also makes
 * the server's end reach the reply endpoint if it never answers; its refusal
 * with NOENT says the endpoint has no live owner already -- a death whose
 * hangup is still queued -- so nothing is sent to an endpoint id that anyone
 * may own next. Called with nothing on the wire. */
static int wfs_block_server_live(wfs_block_t* b) {
    if (b->block_endpoint >= 0 &&
        wasmos_ipc_peer_await(b->reply_endpoint, b->block_endpoint) == WASMOS_NOENT) {
        wfs_block_server_gone(b);
    }
    return b->block_endpoint >= 0;
}

/* Send the staged request to the server. With no live server, the queued
 * events are read first, since they may name its successor; with none still,
 * the request is held while the hold window is open, and fails with
 * WASMOS_ERR_BLOCK_DEV_GONE otherwise. */
static void wfs_block_transmit(wfs_block_t* b) {
    int32_t id = 0;

    if (!wfs_block_server_live(b)) {
        wfs_block_drain(b);
        if (!wfs_block_server_live(b)) {
            if (b->following && !b->gone && wfs_block_window_closed(b)) {
                b->gone = 1u;
            }
            if (b->following && !b->gone) {
                wfs_block_hold(b);
            } else {
                wfs_block_fail(b, WASMOS_ERR_BLOCK_DEV_GONE);
            }
            return;
        }
    }
    if (wasmos_sys_intent_send(b->loop,
                               b->block_endpoint,
                               b->reply_endpoint,
                               b->req_type,
                               b->req_bid,
                               0,
                               (int32_t)sizeof(wasmos_block_request_t),
                               0,
                               on_reply,
                               b,
                               &id) != 0) {
        wfs_block_fail(b, WASMOS_ERR_FS_IO);
        return;
    }
    b->intent_id = id;
}

/* The server ended: nothing is sent to its endpoint again, and the hold window
 * opens. A request it never answered is held when the client follows its
 * server, and fails otherwise. Its intent is cancelled either way, so a late
 * reply from the dead instance settles nothing. */
static void wfs_block_server_gone(wfs_block_t* b) {
    if (b->block_endpoint < 0) {
        return;
    }
    b->block_endpoint = -1;
    b->hold_deadline = (uint32_t)wasmos_sched_ticks() + wfs_block_hold_ticks();
    if (b->intent_id != 0) {
        wasmos_sys_intent_cancel(b->loop, b->intent_id);
        if (b->following) {
            wfs_block_hold(b);
        } else {
            wfs_block_fail(b, WASMOS_ERR_BLOCK_DEV_GONE);
        }
    }
}

/* The loop's timeout: a held request whose window has closed fails, and the
 * disk is taken to be gone until a server registers. */
static void on_hold_timeout(void* user) {
    wfs_block_t* b = (wfs_block_t*)user;
    if (b->held && b->block_endpoint < 0 && wfs_block_window_closed(b)) {
        b->gone = 1u;
        wfs_block_fail(b, WASMOS_ERR_BLOCK_DEV_GONE);
    }
}

/* A new instance serves the disk at `endpoint`: lend it the request buffer and
 * resend what is held. The previous grant is revoked first; a buffer is lent to
 * one borrower at a time. */
static void wfs_block_rebind(wfs_block_t* b, int32_t endpoint) {
    if (b->req_borrow >= 0) {
        (void)wasmos_xfer_buffer_unborrow(b->req_borrow);
        b->req_borrow = -1;
    }
    b->block_endpoint = -1;
    b->req_borrow = wasmos_xfer_buffer_borrow(endpoint, b->req_bid, WASMOS_BUFFER_GRANT_READ);
    if (b->req_borrow < 0) {
        b->req_borrow = -1;
        return;
    }
    b->block_endpoint = endpoint;
    b->gone = 0u;
    b->hold_logged = 0u;
    if (b->held) {
        wfs_block_unhold(b);
        wfs_block_transmit(b);
    }
}

/* WASMOS_IPC_HANGUP: arg0 names the endpoint that ended. */
static void on_hangup(void* user, const wasmos_ipc_message_t* msg) {
    wfs_block_t* b = (wfs_block_t*)user;
    if (msg->arg0 == b->block_endpoint) {
        wfs_block_server_gone(b);
    }
}

/* SVC_IPC_CLASS_EVENT for `block`: arg0 the kind, arg1 the instance, arg2 the
 * provider's endpoint. Only this disk's instance concerns the client. One
 * instance has one live provider, so an ADD naming another endpoint is a new
 * instance of the server. */
static void on_class_event(void* user, const wasmos_ipc_message_t* msg) {
    wfs_block_t* b = (wfs_block_t*)user;
    if ((uint32_t)msg->arg1 != b->target) {
        return;
    }
    if (msg->arg0 == SVC_CLASS_EVENT_REMOVE) {
        if (msg->arg2 == b->block_endpoint) {
            wfs_block_server_gone(b);
        }
    } else if (msg->arg0 == SVC_CLASS_EVENT_ADD && msg->arg2 >= 0 &&
               (b->block_endpoint < 0 || msg->arg2 != b->block_endpoint)) {
        wfs_block_rebind(b, msg->arg2);
    }
}

static void on_followed(void* user, const wasmos_ipc_message_t* reply) {
    (void)reply;
    (void)wasmos_xfer_buffer_release((int32_t)(intptr_t)user);
}

wasmos_error_code_t wfs_block_follow(wfs_block_t* b, int32_t proc_endpoint) {
    static const char k_class[] = "block";
    int32_t bid = wasmos_xfer_stage(k_class, (int32_t)sizeof(k_class));
    if (bid < 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_sys_intent_send(b->loop,
                               proc_endpoint,
                               b->reply_endpoint,
                               SVC_IPC_SUBSCRIBE_CLASS_REQ,
                               b->reply_endpoint,
                               bid,
                               0,
                               0,
                               on_followed,
                               (void*)(intptr_t)bid,
                               0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_FS_IO;
    }
    b->following = 1u;
    return WASMOS_ERR_NONE;
}

wasmos_error_code_t wfs_block_configure(wfs_block_t* b, wasmos_sys_event_loop_t* loop,
                                        int32_t block_endpoint, int32_t reply_endpoint,
                                        int32_t buf_id, uint32_t target) {
    b->loop = loop;
    b->block_endpoint = block_endpoint;
    b->reply_endpoint = reply_endpoint;
    b->buf_id = buf_id;
    b->target = target;
    b->req_bid = -1;
    b->block_size = WFS_BLOCK_SIZE_MIN;
    b->staged_block = WFS_BLOCK_NONE;
    b->pending_block = WFS_BLOCK_NONE;
    b->in_flight = 0u;
    b->redirect = 0;
    b->redirect_user = 0;
    b->intent_id = 0;
    b->req_borrow = -1;
    b->following = 0u;
    b->held = 0u;
    b->hold_logged = 0u;
    b->gone = 0u;
    b->hold_deadline = 0u;

    /* The descriptor buffer is the CLIENT's and is borrowed to the server, which
     * is the direction docs/architecture/12-dma-transfers.md requires: the
     * requester owns the object and the backend is a transient grantee. READ is
     * the whole grant it needs -- a backend reads the request and never writes
     * it. Acquired once here rather than per transfer, because a per-request
     * acquire is a per-request round trip for a buffer whose contents change but
     * whose identity does not. */
    b->req_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(wasmos_block_request_t));
    if (b->req_bid < 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    b->req_borrow =
        wasmos_xfer_buffer_borrow(b->block_endpoint, b->req_bid, WASMOS_BUFFER_GRANT_READ);
    if (b->req_borrow < 0) {
        b->req_borrow = -1;
        (void)wasmos_xfer_buffer_release(b->req_bid);
        b->req_bid = -1;
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_sys_event_register(loop, WASMOS_IPC_HANGUP, on_hangup, b) != 0 ||
        wasmos_sys_event_register(loop, SVC_IPC_CLASS_EVENT, on_class_event, b) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    loop->on_timeout = on_hold_timeout;
    loop->timeout_user = b;
    return WASMOS_ERR_NONE;
}

/* Stage `req` and send it. The request travels in the client's own buffer; the
 * arguments name only where to find it (arg0 = buffer id, arg1 = offset,
 * arg2 = size), which is what lets a transfer carry a 64-bit LBA and its own
 * target instead of a backend inferring one from the sender. */
static wasmos_future_t* send_request(wfs_block_t* b, int32_t type,
                                     const wasmos_block_request_t* req) {
    if (b->req_bid < 0 ||
        wasmos_xfer_buffer_write(b->req_bid, req, (int32_t)sizeof(*req), 0) != 0) {
        /* Recorded, not merely returned. A NULL future means "nothing to await",
         * and wfs_block_take reads that as the cache-hit path and reports
         * SUCCESS -- so a request that was never sent would come back as a write
         * that happened. This is the same trap the staging copy above sets the
         * flag for. */
        b->stage_failed = 1u;
        b->in_flight = 0u;
        wfs_block_invalidate(b);
        return 0;
    }
    b->req_type = type;
    wfs_block_transmit(b);
    return &b->future;
}

void wfs_block_set_redirect(wfs_block_t* b, wfs_block_redirect_fn fn, void* user) {
    b->redirect = fn;
    b->redirect_user = user;
    /* The tag names the block the staged bytes came from. Under a different
     * mapping the same tag would answer a request the bytes do not belong to. */
    wfs_block_invalidate(b);
}

wasmos_error_code_t wfs_block_set_block_size(wfs_block_t* b, uint32_t block_size) {
    if (block_size != 4096u && block_size != 8192u && block_size != 16384u) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    if (block_size != b->block_size) {
        /* The staged block was transferred at the old size, so its contents no
         * longer describe a whole block at the new one. */
        b->block_size = block_size;
        wfs_block_invalidate(b);
    }
    return WASMOS_ERR_NONE;
}

void wfs_block_invalidate(wfs_block_t* b) {
    b->staged_block = WFS_BLOCK_NONE;
}

/* Send one block transfer and return its future. */
static wasmos_future_t* submit(wfs_block_t* b, uint32_t block, int write) {
    uint32_t sectors = b->block_size / WFS_SECTOR_BYTES;
    wasmos_block_request_t req = {0};

    wasmos_future_init(&b->future, &b->promise);
    b->pending_block = block;
    b->in_flight = 1u;

    req.version = BLOCK_REQUEST_VERSION;
    req.target = b->target;
    /* A filesystem block is `sectors` device sectors, so the lba is scaled here
     * rather than by every caller. */
    req.lba = (uint64_t)block * sectors;
    req.sector_count = sectors;
    /* The payload still lands directly in this process's block buffer; the
     * descriptor says where it goes, never what goes there. */
    req.dst_kind = (uint32_t)BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = (uint32_t)b->buf_id;
    return send_request(b, write ? BLOCK_IPC_WRITE_REQ : BLOCK_IPC_READ_REQ, &req);
}

wasmos_future_t* wfs_block_read_begin(wfs_block_t* b, uint32_t block) {
    /* An open transaction holds its blocks in the log rather than at their
     * targets, so the read follows the image (§14). The tag is the block
     * actually read, which is what keeps a later read of the same target from
     * hitting a tag that no longer maps there. */
    if (b->redirect) {
        block = b->redirect(b->redirect_user, block);
    }
    if (b->staged_block == block) {
        return 0; /* cache hit: nothing to await */
    }
    if (b->in_flight) {
        /* One operation at a time. Reaching here means a step began a second
         * request without awaiting the first, which would lose the reply. */
        return 0;
    }
    return submit(b, block, 0);
}

wasmos_future_t* wfs_block_flush_begin(wfs_block_t* b) {
    if (b->in_flight) {
        return 0;
    }
    wasmos_block_request_t req = {0};

    wasmos_future_init(&b->future, &b->promise);
    /* The tag is left alone: a flush moves no data, so the staged block still
     * holds what it held and still names the block it came from. */
    b->pending_block = b->staged_block;
    b->in_flight = 1u;

    /* A flush carries a descriptor for one field, `target`. It moves no sectors
     * and names no destination -- the zeroed remainder says so -- but a backend
     * serving several disks has to be told which one to commit, and since the
     * client table went away nothing infers that from the sender. */
    req.version = BLOCK_REQUEST_VERSION;
    req.target = b->target;
    return send_request(b, BLOCK_IPC_FLUSH_REQ, &req);
}

wasmos_future_t* wfs_block_write_begin(wfs_block_t* b, uint32_t block) {
    if (b->in_flight) {
        return 0;
    }
    /* A write request names the SERVER's buffer, so the staged block has to be
     * copied into it first -- the reverse of the copy wfs_block_take does for a
     * read. Without this the server persists whatever that buffer already held.
     * Refusing here rather than submitting is what keeps a failed stage from
     * writing the wrong bytes to a real block. */
    if (wasmos_block_buffer_write(
            b->buf_id, addr_cast(int32_t, b->data), (int32_t)b->block_size, 0) != 0) {
        b->stage_failed = 1u;
        wfs_block_invalidate(b);
        return 0;
    }
    return submit(b, block, 1);
}

wasmos_error_code_t wfs_block_take(wfs_block_t* b) {
    const wasmos_ipc_message_t* reply;

    if (b->stage_failed) {
        b->stage_failed = 0u;
        return WASMOS_ERR_FS_BUFFER;
    }
    if (!b->in_flight) {
        return WASMOS_ERR_NONE; /* the caller took the cache-hit path */
    }
    b->in_flight = 0u;

    if (b->future.state != WASMOS_FUTURE_READY) {
        wfs_block_invalidate(b);
        return b->future.status ? (wasmos_error_code_t)b->future.status : WASMOS_ERR_FS_IO;
    }
    reply = &b->reply;

    /* A read lands in the server's buffer; pull it into the staged block. A
     * write left the buffer holding what was written, so the tag is valid for
     * the block written exactly as a read's is. */
    if (reply->type == BLOCK_IPC_READ_RESP &&
        wasmos_block_buffer_copy(
            b->buf_id, addr_cast(int32_t, b->data), (int32_t)b->block_size, 0) != 0) {
        wfs_block_invalidate(b);
        return WASMOS_ERR_FS_BUFFER;
    }

    b->staged_block = b->pending_block;
    return WASMOS_ERR_NONE;
}
