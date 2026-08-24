/* wfs_block.c - staging and cache-tag bookkeeping over the IPC-future bridge. */
#include "wfs_block.h"

#include "wasmos/api.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

/* Accept a block reply, reject anything else. The bridge calls this when a
 * reply lands and settles the future accordingly, which is how BLOCK_IPC_ERROR
 * becomes a rejected await instead of a status the caller must remember to
 * check. */
static int32_t block_reply_status(void* user, const wasmos_ipc_message_t* reply) {
    const wfs_block_t* b = (const wfs_block_t*)user;
    int32_t want;

    if (!b || !reply) {
        return WASMOS_ERR_FS_IO;
    }
    want = b->op.reply.type == BLOCK_IPC_WRITE_RESP ? BLOCK_IPC_WRITE_RESP : BLOCK_IPC_READ_RESP;
    if (reply->type == BLOCK_IPC_ERROR) {
        return reply->arg0 ? reply->arg0 : WASMOS_ERR_FS_IO;
    }
    if (reply->type != want) {
        return WASMOS_ERR_FS_IO;
    }
    return 0;
}

void wfs_block_configure(wfs_block_t* b, wasmos_sys_event_loop_t* loop, int32_t block_endpoint,
                         int32_t reply_endpoint, int32_t buf_id) {
    b->loop = loop;
    b->block_endpoint = block_endpoint;
    b->reply_endpoint = reply_endpoint;
    b->buf_id = buf_id;
    b->block_size = WFS_BLOCK_SIZE_MIN;
    b->staged_block = WFS_BLOCK_NONE;
    b->pending_block = WFS_BLOCK_NONE;
    b->in_flight = 0u;
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

/* Send one block request and return its future. */
static wasmos_future_t* submit(wfs_block_t* b, uint32_t block, int write) {
    uint32_t sectors = b->block_size / WFS_SECTOR_BYTES;

    wasmos_sys_wasm_ipc_future_init(&b->op, block_reply_status, b);
    b->pending_block = block;
    b->in_flight = 1u;

    /* arg0 = the server's buffer handle, arg1 = starting sector, arg2 = sector
     * count. A filesystem block is `sectors` device sectors, so the lba is
     * scaled here rather than by every caller. */
    return wasmos_sys_wasm_ipc_future_send(b->loop,
                                           &b->op,
                                           b->block_endpoint,
                                           b->reply_endpoint,
                                           write ? BLOCK_IPC_WRITE_REQ : BLOCK_IPC_READ_REQ,
                                           b->buf_id,
                                           (int32_t)(block * sectors),
                                           (int32_t)sectors,
                                           0,
                                           0);
}

wasmos_future_t* wfs_block_read_begin(wfs_block_t* b, uint32_t block) {
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

wasmos_future_t* wfs_block_write_begin(wfs_block_t* b, uint32_t block) {
    if (b->in_flight) {
        return 0;
    }
    return submit(b, block, 1);
}

wasmos_error_code_t wfs_block_take(wfs_block_t* b) {
    const wasmos_ipc_message_t* reply;

    if (!b->in_flight) {
        return WASMOS_ERR_NONE; /* the caller took the cache-hit path */
    }
    b->in_flight = 0u;

    if (b->op.future.state != WASMOS_FUTURE_READY) {
        wfs_block_invalidate(b);
        return b->op.future.status ? (wasmos_error_code_t)b->op.future.status : WASMOS_ERR_FS_IO;
    }
    reply = wasmos_sys_wasm_ipc_future_reply(&b->op);
    if (!reply) {
        wfs_block_invalidate(b);
        return WASMOS_ERR_FS_IO;
    }

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
