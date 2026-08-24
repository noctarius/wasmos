/* Host unit test for the WFS driver operations: mount, group descriptors and
 * object records, run as tasks on the SYSTEM coroutine runtime against a volume
 * mkfs_wfs built.
 *
 * Nothing here reimplements scheduling. The suite links the real
 * coroutine_wasm.c and ipc_future_wasm.c and drives them with
 * wasmos_wasm_coroutine_run_budget, so what is exercised is the runtime's own
 * parking and resumption. Below the driver it supplies the wasm IPC hostcalls —
 * WASMOS_WASM_IMPORT expands to nothing off wasm, so these definitions link —
 * as a fake BLOCK server serving a RAM image. That means the request encoding
 * itself is under test: a wrong lba or sector count reaches the fake server as a
 * wrong read, not as a silently different function call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_format.h"
#include "wfs_mkfs.h"
#include "wfs_mount.h"
#include "wfs_super.h"

#include "wasmos_cast.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

static void expect_rc(wasmos_error_code_t got, wasmos_error_code_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %s (%d), want %s (%d)\n",
               what,
               wasmos_strerror(got),
               (int)got,
               wasmos_strerror(want),
               (int)want);
    }
}

/* ---- the fake block server ---------------------------------------------- */

#define REQ_LOG_MAX 64
#define FAKE_BUF_ID 7
#define BLOCK_ENDPOINT 11
#define REPLY_ENDPOINT 12

/* The one block client this fixture drives; the stubs above need it. */
static wfs_block_t g_blk;

static uint8_t* g_image;
static uint32_t g_image_blocks;
static uint32_t g_image_block_size;

static wasmos_ipc_message_t g_queued;
static int g_queued_ready;
static int g_send_status;

/* What the driver asked for, in order: the fake server records the FILESYSTEM
 * block each request resolves to, so a step that let a block number live on the
 * C stack across an await shows up as a request for the wrong block. */
static uint32_t g_req_blocks[REQ_LOG_MAX];
static uint32_t g_req_count;
static uint32_t g_reads;
static uint32_t g_last_sectors;
static int g_fail_next; /* make the next request answered with BLOCK_IPC_ERROR */

/* The block the pending reply is for, so buffer_copy serves the right one. */
static uint32_t g_serving_block;

int32_t wasmos_xfer_buffer_acquire(int32_t size) {
    return size > 0 ? FAKE_BUF_ID : -1;
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

/* Serves the staged block out of the RAM image, which is what a real block
 * server would have written into the transfer buffer.
 *
 * `dst` is a guest address, and the hostcall ABI carries it as int32_t because a
 * wasm32 pointer IS 32 bits. On this 64-bit host that truncates, so the
 * truncated value cannot be turned back into a pointer. It is still checked
 * against the truncation of the driver's own buffer — which is what verifies the
 * driver passed the buffer it was given rather than some other address — and the
 * copy then goes to the real pointer, of which this fixture has exactly one. */
int32_t wasmos_block_buffer_copy(int32_t phys, int32_t dst, int32_t len, int32_t offset) {
    if (phys != FAKE_BUF_ID || offset != 0 || len <= 0) {
        return -1;
    }
    if (dst != addr_cast(int32_t, g_blk.data)) {
        return -1;
    }
    if (g_serving_block >= g_image_blocks) {
        return -1;
    }
    memcpy(g_blk.data, g_image + (size_t)g_serving_block * g_image_block_size, (size_t)len);
    return 0;
}

/* The fake server: answers a block request immediately by arming the reply the
 * next drain will deliver. */
int32_t wasmos_ipc_send(int32_t destination, int32_t source, int32_t type, int32_t request_id,
                        int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    uint32_t sectors;
    uint32_t block;

    (void)arg3;
    if (g_send_status != 0) {
        return g_send_status;
    }
    if (type != BLOCK_IPC_READ_REQ && type != BLOCK_IPC_WRITE_REQ) {
        return 0; /* not ours; silently accepted and never answered */
    }
    if (arg0 != FAKE_BUF_ID) {
        return -1; /* the driver must name the buffer it was given */
    }

    sectors = (uint32_t)arg2;
    g_last_sectors = sectors;
    /* Undo the driver's block -> sector scaling, which is the encoding under
     * test: the request must name whole filesystem blocks. */
    block = sectors ? (uint32_t)arg1 / sectors : 0xFFFFFFFFu;
    if (g_req_count < REQ_LOG_MAX) {
        g_req_blocks[g_req_count] = block;
    }
    g_req_count++;
    g_reads++;
    g_serving_block = block;

    memset(&g_queued, 0, sizeof(g_queued));
    g_queued.request_id = request_id;
    g_queued.source = destination;
    g_queued.destination = source;
    if (g_fail_next || block >= g_image_blocks) {
        g_fail_next = 0;
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

/* No select set, so the loop never parks in wasmos_ipc_select_wait: the suite
 * drives poll() itself. */
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

/* ---- the volume --------------------------------------------------------- */

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0x30, 0x91, 0x4c, 0x02, 0xbb, 0x77, 0x41, 0x18, 0x8e, 0x5a, 0x22, 0xd9, 0x6f, 0x40, 0x13, 0xc7};
#define TEST_NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

static wfs_mkfs_layout_t g_layout;

static int mkfs_sink(void* ctx, uint32_t block, const void* data, uint32_t len) {
    (void)ctx;
    if (block >= g_image_blocks) {
        return -1;
    }
    memcpy(g_image + (size_t)block * len, data, len);
    return 0;
}

static int build_volume(uint64_t size, uint32_t block_size) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;

    memset(&params, 0, sizeof(params));
    params.size_bytes = size;
    params.block_size = block_size;
    params.now_ns = TEST_NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);

    if (wfs_mkfs_plan(&params, &g_layout) != WASMOS_ERR_NONE) {
        return -1;
    }
    free(g_image);
    g_image = (uint8_t*)calloc(g_layout.total_blocks, g_layout.block_size);
    if (!g_image) {
        return -1;
    }
    g_image_blocks = g_layout.total_blocks;
    g_image_block_size = g_layout.block_size;

    sink.ctx = NULL;
    sink.write_block = mkfs_sink;
    if (wfs_mkfs_format(&params, &sink, &g_layout) != WASMOS_ERR_NONE) {
        return -1;
    }

    g_queued_ready = 0;
    g_send_status = 0;
    g_req_count = 0;
    g_reads = 0;
    g_fail_next = 0;
    return 0;
}

static void teardown(void) {
    free(g_image);
    g_image = NULL;
}

/* ---- driving the runtime ------------------------------------------------- */

static wasmos_wasm_runtime_t g_runtime;
static wasmos_sys_event_loop_t g_loop;

static void bring_up(void) {
    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, REPLY_ENDPOINT, 0x1000);
    wfs_block_configure(&g_blk, &g_loop, BLOCK_ENDPOINT, REPLY_ENDPOINT, FAKE_BUF_ID);
    wfs_ops_bind(&g_runtime, &g_blk);
}

/* Run the runtime and the event loop until `task` is finished, exactly as the
 * driver's own loop will: resume ready tasks, then deliver replies, which wakes
 * whatever those tasks parked on. Returns the task's completion status. */
static int32_t run_until_done(wasmos_wasm_coroutine_t* task, int32_t* out_status) {
    uint32_t spins = 0;
    int32_t result = 0;

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
    result = wasmos_wasm_coroutine_join(task, out_status);
    return result;
}

static int32_t g_task_status;

/* The task record is zeroed as well as the context. wasmos_async_start accepts
 * only a NEW or DEAD record — a caller-owned record is NEW exactly when it is
 * zero — and rejects anything else, because a queued, running or waiting record
 * would corrupt the ready list through its reused link fields. An uninitialised
 * stack record is therefore refused, silently, whenever the stack happens not to
 * hold zero. */
static int32_t start_and_run_mount(wfs_mount_ctx_t* ctx, wfs_volume_t* vol,
                                   wasmos_wasm_coroutine_t* task) {
    memset(ctx, 0, sizeof(*ctx));
    memset(vol, 0, sizeof(*vol));
    memset(task, 0, sizeof(*task));
    ctx->vol = vol;
    if (!wasmos_async_start(&g_runtime, task, wfs_mount_task, ctx)) {
        return -1;
    }
    return run_until_done(task, &g_task_status);
}

static int32_t start_and_run_object(wfs_object_ctx_t* ctx, const wfs_volume_t* vol,
                                    uint32_t object_id, wasmos_wasm_coroutine_t* task) {
    memset(ctx, 0, sizeof(*ctx));
    memset(task, 0, sizeof(*task));
    ctx->vol = vol;
    ctx->object_id = object_id;
    if (!wasmos_async_start(&g_runtime, task, wfs_object_task, ctx)) {
        return -1;
    }
    return run_until_done(task, &g_task_status);
}

static int32_t start_and_run_group(wfs_group_ctx_t* ctx, const wfs_volume_t* vol, uint32_t group,
                                   wasmos_wasm_coroutine_t* task) {
    memset(ctx, 0, sizeof(*ctx));
    memset(task, 0, sizeof(*task));
    ctx->vol = vol;
    ctx->group = group;
    if (!wasmos_async_start(&g_runtime, task, wfs_group_task, ctx)) {
        return -1;
    }
    return run_until_done(task, &g_task_status);
}

/* ---- mount --------------------------------------------------------------- */

static void test_mount_reads_a_volume_mkfs_wrote(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "the mount task completes");
    expect(vol.mounted == 1u, "the volume is mounted");
    expect(g_reads > 0u, "it read from the device");

    expect(vol.super.block_size == 4096u, "block size");
    expect(vol.super.total_blocks == g_layout.total_blocks, "block count");
    expect(vol.super.group_count == g_layout.group_count, "group count");
    expect(vol.super.root_object_id == WFS_OBJECT_ROOT, "root object id");
    expect(vol.super.needs_replay == 0u, "a fresh volume needs no replay");
    expect(vol.super.read_only == 0u, "and mounts writable");
    expect(memcmp(vol.super.uuid, k_uuid, WFS_UUID_LEN) == 0, "uuid");

    /* The task parked on futures and the runtime resumed it: it is DEAD, and it
     * got there across at least one reply delivery. */
    expect(task.state == WASMOS_WASM_COROUTINE_DEAD, "the task ended on the runtime");

    teardown();
}

/* Regression: 2026-08-24-wfs-yield-local — the block number a step was about to
 * read was held in a C LOCAL. The runtime preserves no stack across a resume, so
 * on the resume path that local was read uninitialised and the request went out
 * for whatever the stack held. The visible cost is a read of the wrong block, so
 * this pins the SEQUENCE of blocks mount asks for rather than only the count. It
 * also covers the request encoding: the fake server divides the lba back out by
 * the sector count, so a wrong scaling shows up here too.
 */
static void test_mount_requests_exactly_the_blocks_it_should(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    expect(g_layout.group_count == 1u, "a 16 MiB volume is one group");
    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount");

    /* Block 0 for the superblock, then the one descriptor-table block. Mount
     * reads neither the object table nor the bitmaps. */
    expect(g_req_count == 2u, "mount reads exactly two blocks");
    if (g_req_count >= 2u) {
        expect(g_req_blocks[0] == 0u, "the first read is block 0, for the superblock");
        expect(g_req_blocks[1] == g_layout.group_table_start,
               "the second read is the group descriptor table");
    }
    expect(g_last_sectors == 4096u / WFS_SECTOR_BYTES,
           "a filesystem block is requested as its whole run of sectors");

    teardown();
}

/* The staged block is a one-block cache, so a read of the block already staged
 * must not reach the device. */
static void test_the_staged_block_is_a_cache(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t o;
    wasmos_wasm_coroutine_t otask;
    uint32_t after_first;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount");

    expect(start_and_run_object(&o, &vol, WFS_OBJECT_ROOT, &otask) == 0, "read the root object");
    after_first = g_reads;
    expect(start_and_run_object(&o, &vol, WFS_OBJECT_ROOT, &otask) == 0, "read it again");
    expect(g_reads == after_first, "a second read of a staged block costs no request");

    teardown();
}

/* ---- object records ------------------------------------------------------ */

static void test_the_root_object_reads_back_as_mkfs_wrote_it(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t o;
    wasmos_wasm_coroutine_t otask;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount");
    expect(start_and_run_object(&o, &vol, WFS_OBJECT_ROOT, &otask) == 0,
           "the root record reads and verifies");

    expect(o.out.object_id == WFS_OBJECT_ROOT, "object id");
    expect(o.out.type == WFS_TYPE_DIR, "the root is a directory");
    expect(o.out.link_count == 2u, "link count is 2");
    expect(o.out.size == 4096u, "size is one directory block");
    expect(o.out.extent_count == 1u, "one extent");
    expect(o.out.extents[0].physical_block == g_layout.root_data_block,
           "the extent points at the root's data block");
    expect(o.out.extents[0].length == 1u, "the extent is one block");
    expect(o.out.extent_tree_block == 0u, "an inline extent needs no tree");
    expect(o.out.mtime == TEST_NOW_NS, "timestamps round-trip");

    teardown();
}

static void test_an_unallocated_object_is_refused(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t o;
    wasmos_wasm_coroutine_t otask;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount");

    /* A task that fails rejects its completion future, so the join reports the
     * packed code directly — no separate status channel. */
    expect_rc((wasmos_error_code_t)start_and_run_object(&o, &vol, WFS_OBJECT_INVALID, &otask),
              WASMOS_ERR_FS_NOT_FOUND,
              "object 0 is refused");
    expect_rc((wasmos_error_code_t)start_and_run_object(&o, &vol, vol.super.total_objects, &otask),
              WASMOS_ERR_FS_NOT_FOUND,
              "an id past the table is refused");
    /* An id inside the table but never allocated has a zeroed record, whose
     * checksum cannot match: corrupt, not merely absent. Telling the two apart
     * is the bitmap's job, not the record reader's. */
    expect_rc((wasmos_error_code_t)start_and_run_object(&o, &vol, WFS_OBJECT_FIRST, &otask),
              WASMOS_ERR_FS_CHECKSUM,
              "an unallocated record does not verify");

    teardown();
}

/* A record is checksummed under its object id, so one moved to another slot must
 * not verify there (§13). */
static void test_an_object_record_is_bound_to_its_slot(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t o;
    wasmos_wasm_coroutine_t otask;
    uint8_t* table;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    table = g_image + (size_t)g_layout.object_table_start * g_layout.block_size;
    memcpy(table + (size_t)2u * WFS_OBJECT_SIZE, table + WFS_OBJECT_SIZE, WFS_OBJECT_SIZE);

    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount");
    expect_rc((wasmos_error_code_t)start_and_run_object(&o, &vol, 2u, &otask),
              WASMOS_ERR_FS_CHECKSUM,
              "a transplanted record is refused");

    teardown();
}

/* ---- group descriptors --------------------------------------------------- */

static void test_group_descriptors_read_back(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    wfs_group_ctx_t g;
    wasmos_wasm_coroutine_t gtask;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount");

    expect(start_and_run_group(&g, &vol, 0u, &gtask) == 0, "group 0's descriptor verifies");
    expect(g.out.block_bitmap == g_layout.bitmap_start, "it names its block bitmap");
    expect(g.out.object_bitmap == g_layout.bitmap_start + 1u, "it names its object bitmap");
    expect(g.out.object_table == g_layout.object_table_start, "it names its object table slice");
    expect(g.out.free_blocks == g_layout.free_blocks, "one group's free count is the volume's");

    expect_rc((wasmos_error_code_t)start_and_run_group(&g, &vol, vol.super.group_count, &gtask),
              WASMOS_ERR_FS_CORRUPT,
              "a group past the table is refused");

    teardown();
}

/* Mount verifies every descriptor before declaring the volume usable, and the
 * child task's failure reaches it through the join. */
static void test_mount_refuses_a_volume_with_a_bad_descriptor(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    uint8_t* table;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    table = g_image + (size_t)g_layout.group_table_start * g_layout.block_size;
    table[4] = (uint8_t)(table[4] ^ 0x10u);

    bring_up();
    expect_rc((wasmos_error_code_t)start_and_run_mount(&ctx, &vol, &task),
              WASMOS_ERR_FS_CHECKSUM,
              "a corrupted group descriptor fails the mount");
    expect(vol.mounted == 0u, "and the volume is not marked mounted");

    teardown();
}

/* ---- mount failure paths ------------------------------------------------- */

static void test_mount_refuses_a_device_that_holds_no_volume(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    memset(g_image, 0, g_layout.block_size);

    bring_up();
    expect_rc((wasmos_error_code_t)start_and_run_mount(&ctx, &vol, &task),
              WASMOS_ERR_FS_BAD_MAGIC,
              "an unformatted device");
    expect(vol.mounted == 0u, "nothing is mounted");

    teardown();
}

/* A device error must reach the awaiting task. The future bridge rejects on
 * BLOCK_IPC_ERROR, so the failure arrives at the await rather than being
 * swallowed into a silent retry. */
static void test_a_device_error_fails_the_mount(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    g_fail_next = 1;

    expect_rc((wasmos_error_code_t)start_and_run_mount(&ctx, &vol, &task),
              WASMOS_ERR_FS_IO,
              "a failed transfer fails the mount");
    /* And it must not leave the device's leftover bytes looking cached. */
    expect(g_blk.staged_block == WFS_BLOCK_NONE, "the cache tag is cleared on failure");
    expect(vol.mounted == 0u, "nothing is mounted");

    teardown();
}

/* A send that cannot be issued must fail the same way: the bridge returns an
 * already-rejected future rather than NULL, so the await path is identical. */
static void test_a_failed_send_fails_the_mount(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    bring_up();
    g_send_status = -1;

    expect(start_and_run_mount(&ctx, &vol, &task) != 0, "an unsendable request fails the mount");
    expect(vol.mounted == 0u, "nothing is mounted");
    g_send_status = 0;

    teardown();
}

/* Until replay exists a volume that was not unmounted cleanly mounts read-only
 * rather than serving metadata the log has superseded. */
static void test_a_dirty_volume_mounts_read_only(void) {
    wfs_mount_ctx_t ctx;
    wfs_volume_t vol;
    wasmos_wasm_coroutine_t task;
    uint8_t* sb;
    uint8_t* csum;
    uint32_t c;

    if (build_volume(VOL_16M, 4096u) != 0) {
        expect(0, "build a volume");
        return;
    }
    sb = g_image + WFS_SUPER_OFFSET;
    sb[offsetof(struct wfs_superblock, state)] = (uint8_t)WFS_STATE_DIRTY;

    /* Reseal: the state is inside the checksummed image, so without this the
     * case would exercise the checksum path instead of the dirty-mount one. */
    csum = sb + offsetof(struct wfs_superblock, checksum);
    csum[0] = 0;
    csum[1] = 0;
    csum[2] = 0;
    csum[3] = 0;
    c = wfs_checksum_struct(
        k_uuid, 0u, sb, WFS_SUPER_SIZE, (uint32_t)offsetof(struct wfs_superblock, checksum));
    csum[0] = (uint8_t)(c & 0xFFu);
    csum[1] = (uint8_t)((c >> 8) & 0xFFu);
    csum[2] = (uint8_t)((c >> 16) & 0xFFu);
    csum[3] = (uint8_t)((c >> 24) & 0xFFu);

    bring_up();
    expect(start_and_run_mount(&ctx, &vol, &task) == 0, "a dirty volume still mounts");
    expect(vol.super.needs_replay == 1u, "and reports that replay is owed");
    expect(vol.super.read_only == 1u, "and is read-only until it happens");

    teardown();
}

/* Every permitted block size must mount: the block layer adopts the volume's
 * size after reading block 0 at the default. */
static void test_every_block_size_mounts(void) {
    static const uint32_t sizes[3] = {4096u, 8192u, 16384u};
    uint32_t i;

    for (i = 0; i < 3u; ++i) {
        wfs_mount_ctx_t ctx;
        wfs_volume_t vol;
        wasmos_wasm_coroutine_t task;

        if (build_volume(64ull * 1024ull * 1024ull, sizes[i]) != 0) {
            expect(0, "build a volume");
            continue;
        }
        bring_up();
        expect(start_and_run_mount(&ctx, &vol, &task) == 0, "mount at a permitted block size");
        expect(vol.super.block_size == sizes[i], "the volume's size was adopted");
        expect(g_blk.block_size == sizes[i], "and whole blocks are transferred");
        expect(g_last_sectors == sizes[i] / WFS_SECTOR_BYTES,
               "the request names the block's whole run of sectors");
        teardown();
    }
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_mount_reads_a_volume_mkfs_wrote),
    WASMOS_TEST_CASE(test_mount_requests_exactly_the_blocks_it_should),
    WASMOS_TEST_CASE(test_the_staged_block_is_a_cache),
    WASMOS_TEST_CASE(test_the_root_object_reads_back_as_mkfs_wrote_it),
    WASMOS_TEST_CASE(test_an_unallocated_object_is_refused),
    WASMOS_TEST_CASE(test_an_object_record_is_bound_to_its_slot),
    WASMOS_TEST_CASE(test_group_descriptors_read_back),
    WASMOS_TEST_CASE(test_mount_refuses_a_volume_with_a_bad_descriptor),
    WASMOS_TEST_CASE(test_mount_refuses_a_device_that_holds_no_volume),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_mount),
    WASMOS_TEST_CASE(test_a_failed_send_fails_the_mount),
    WASMOS_TEST_CASE(test_a_dirty_volume_mounts_read_only),
    WASMOS_TEST_CASE(test_every_block_size_mounts),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_mount: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_mount: %d checks passed\n", g_checks);
    return 0;
}
