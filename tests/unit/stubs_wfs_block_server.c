/* stubs_wfs_block_server.c - see the header. */
#include "stubs_wfs_block_server.h"

#include <stdlib.h>
#include <string.h>

#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"
#include "wfs_ops.h"

#define WFS_STUB_BUF_ID 7
#define WFS_STUB_BLOCK_ENDPOINT 11
#define WFS_STUB_REPLY_ENDPOINT 12

uint8_t* wfs_stub_image;
uint32_t wfs_stub_blocks;
uint32_t wfs_stub_block_size;
uint32_t wfs_stub_req_blocks[WFS_STUB_REQ_LOG_MAX];
uint32_t wfs_stub_req_count;
uint32_t wfs_stub_reads;
uint32_t wfs_stub_last_sectors;
int wfs_stub_fail_next;
int wfs_stub_send_status;

static wasmos_wasm_runtime_t g_runtime;
static wasmos_sys_event_loop_t g_loop;
static wfs_block_t g_blk;

/* The one reply in flight, and the block it is for. */
static wasmos_ipc_message_t g_queued;
static int g_queued_ready;
static uint32_t g_serving_block;

wfs_block_t* wfs_stub_block(void) {
    return &g_blk;
}

/* ---- transfer buffers --------------------------------------------------- */

int32_t wasmos_xfer_buffer_acquire(int32_t size) {
    return size > 0 ? WFS_STUB_BUF_ID : -1;
}
int32_t wasmos_xfer_buffer_borrow(int32_t endpoint, int32_t buffer, int32_t flags) {
    (void)endpoint;
    (void)buffer;
    (void)flags;
    return 1;
}
int32_t wasmos_xfer_buffer_release(int32_t buffer) {
    (void)buffer;
    return 0;
}
int32_t wasmos_xfer_buffer_write(int32_t buffer, const void* ptr, int32_t len, int32_t offset) {
    (void)buffer;
    (void)ptr;
    (void)len;
    (void)offset;
    return 0;
}
int32_t wasmos_xfer_buffer_read(int32_t buffer, void* ptr, int32_t len, int32_t offset) {
    (void)buffer;
    (void)ptr;
    (void)len;
    (void)offset;
    return 0;
}

/* Serves the requested block out of the image, which is what a real block server
 * would have written into the transfer buffer.
 *
 * `dst` is a guest address and the hostcall ABI carries it as int32_t because a
 * wasm32 pointer IS 32 bits. On a 64-bit host that truncates, so the value
 * cannot be turned back into a pointer. It is still checked against the
 * truncation of the driver's own buffer — which is what verifies the driver
 * passed the buffer it was given — and the copy then goes to the real pointer,
 * of which this fixture has exactly one. */
int32_t wasmos_block_buffer_copy(int32_t phys, int32_t dst, int32_t len, int32_t offset) {
    if (phys != WFS_STUB_BUF_ID || offset != 0 || len <= 0) {
        return -1;
    }
    if (dst != addr_cast(int32_t, g_blk.data)) {
        return -1;
    }
    if (g_serving_block >= wfs_stub_blocks) {
        return -1;
    }
    memcpy(g_blk.data, wfs_stub_image + (size_t)g_serving_block * wfs_stub_block_size, (size_t)len);
    return 0;
}

/* ---- the fake server ---------------------------------------------------- */

int32_t wasmos_ipc_send(int32_t destination, int32_t source, int32_t type, int32_t request_id,
                        int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    uint32_t sectors;
    uint32_t block;

    (void)arg3;
    if (wfs_stub_send_status != 0) {
        return wfs_stub_send_status;
    }
    if (type != BLOCK_IPC_READ_REQ && type != BLOCK_IPC_WRITE_REQ) {
        return 0; /* not ours: accepted and never answered */
    }
    if (arg0 != WFS_STUB_BUF_ID) {
        return -1; /* the driver must name the buffer it was given */
    }

    sectors = (uint32_t)arg2;
    wfs_stub_last_sectors = sectors;
    /* Undo the driver's block -> sector scaling. This is the encoding under
     * test: the request must name a whole filesystem block. */
    block = sectors ? (uint32_t)arg1 / sectors : 0xFFFFFFFFu;
    if (wfs_stub_req_count < WFS_STUB_REQ_LOG_MAX) {
        wfs_stub_req_blocks[wfs_stub_req_count] = block;
    }
    wfs_stub_req_count++;
    wfs_stub_reads++;
    g_serving_block = block;

    memset(&g_queued, 0, sizeof(g_queued));
    g_queued.request_id = request_id;
    g_queued.source = destination;
    g_queued.destination = source;
    if (wfs_stub_fail_next || block >= wfs_stub_blocks) {
        wfs_stub_fail_next = 0;
        g_queued.type = BLOCK_IPC_ERROR;
        g_queued.arg0 = WASMOS_ERR_FS_IO;
    } else {
        g_queued.type = (type == BLOCK_IPC_WRITE_REQ) ? BLOCK_IPC_WRITE_RESP : BLOCK_IPC_READ_RESP;
        g_queued.arg1 = (int32_t)sectors;
    }
    g_queued_ready = 1;
    return 0;
}

int32_t wasmos_ipc_drain(int32_t endpoint) {
    (void)endpoint;
    if (!g_queued_ready) {
        return 0;
    }
    g_queued_ready = 0;
    return 1;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    switch (field) {
    case 0:
        return g_queued.type;
    case 1:
        return g_queued.request_id;
    case 2:
        return g_queued.arg0;
    case 3:
        return g_queued.arg1;
    case 4:
        return g_queued.source;
    case 5:
        return g_queued.destination;
    case 6:
        return g_queued.arg2;
    case 7:
        return g_queued.arg3;
    default:
        return 0;
    }
}

/* No select set, so the loop never parks in wasmos_ipc_select_wait: the tests
 * drive poll() themselves. */
int32_t wasmos_ipc_select_create(void) {
    return -1;
}
int32_t wasmos_ipc_select_add(int32_t select_id, int32_t endpoint) {
    (void)select_id;
    (void)endpoint;
    return -1;
}
int32_t wasmos_ipc_select_wait(int32_t select_id) {
    (void)select_id;
    return -1;
}
int32_t wasmos_ipc_select_destroy(int32_t select_id) {
    (void)select_id;
    return -1;
}

/* ---- fixture ------------------------------------------------------------ */

int wfs_stub_sink_write(void* ctx, uint32_t block, const void* data, uint32_t len) {
    (void)ctx;
    if (block >= wfs_stub_blocks) {
        return -1;
    }
    memcpy(wfs_stub_image + (size_t)block * len, data, len);
    return 0;
}

void wfs_stub_reset_counters(void) {
    wfs_stub_req_count = 0;
    wfs_stub_reads = 0;
    wfs_stub_last_sectors = 0;
    wfs_stub_fail_next = 0;
    wfs_stub_send_status = 0;
    g_queued_ready = 0;
}

int wfs_stub_build_volume(uint64_t size_bytes, uint32_t block_size,
                          const uint8_t uuid[WFS_UUID_LEN], uint64_t now_ns,
                          wfs_mkfs_layout_t* out_layout) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;

    memset(&params, 0, sizeof(params));
    params.size_bytes = size_bytes;
    params.block_size = block_size;
    params.now_ns = now_ns;
    memcpy(params.uuid, uuid, WFS_UUID_LEN);

    if (wfs_mkfs_plan(&params, out_layout) != WASMOS_ERR_NONE) {
        return -1;
    }
    free(wfs_stub_image);
    wfs_stub_image = (uint8_t*)calloc(out_layout->total_blocks, out_layout->block_size);
    if (!wfs_stub_image) {
        return -1;
    }
    wfs_stub_blocks = out_layout->total_blocks;
    wfs_stub_block_size = out_layout->block_size;

    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;
    if (wfs_mkfs_format(&params, &sink, out_layout) != WASMOS_ERR_NONE) {
        return -1;
    }

    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, WFS_STUB_REPLY_ENDPOINT, 0x1000);
    wfs_block_configure(
        &g_blk, &g_loop, WFS_STUB_BLOCK_ENDPOINT, WFS_STUB_REPLY_ENDPOINT, WFS_STUB_BUF_ID);
    wfs_ops_bind(&g_runtime, &g_blk);
    wfs_stub_reset_counters();
    return 0;
}

void wfs_stub_teardown(void) {
    free(wfs_stub_image);
    wfs_stub_image = NULL;
    wfs_stub_blocks = 0;
    wfs_stub_block_size = 0;
}

int32_t wfs_stub_run_task(wasmos_wasm_coroutine_t* task, wasmos_wasm_task_resume_fn fn,
                          void* user) {
    uint32_t spins = 0;
    int32_t value = 0;

    memset(task, 0, sizeof(*task));
    if (!wasmos_async_start(&g_runtime, task, fn, user)) {
        return -1;
    }
    for (;;) {
        (void)wasmos_wasm_coroutine_run_budget(&g_runtime, 64u);
        if (task->state == WASMOS_WASM_COROUTINE_DEAD) {
            break;
        }
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        if (++spins > 100000u) {
            return -1; /* a task that never progresses must not hang the suite */
        }
    }
    return wasmos_wasm_coroutine_join(task, &value);
}
