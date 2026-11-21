/* Host unit test for fs-fat's block layer (src/drivers/fs_fat/fat_block.c):
 * what it sends a block server, and what it does when that server ends.
 *
 * The IPC hostcalls the layer reaches the server through are defined here --
 * WASMOS_WASM_IMPORT expands to nothing off wasm, so these definitions link. A
 * send is recorded rather than answered; a case answers it, or ends the server,
 * by queueing what the kernel and the registry would deliver on the layer's
 * endpoints.
 */
#include <stdio.h>
#include <string.h>

#include "fat_block.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"
#include "wasmos_status.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

/* --- the stubbed kernel ---------------------------------------------------- */

#define REPLY_EP 20
#define FOLLOW_EP 21
#define PROC_EP 5
#define SERVER_EP 30
#define SUCCESSOR_EP 31
#define TARGET 0x1234u
#define QUEUE_MAX 8

typedef struct {
    int32_t endpoint;
    wasmos_ipc_message_t msg;
} queued_t;

static queued_t g_queue[QUEUE_MAX];
static int g_queued;
static wasmos_ipc_message_t g_last;

/* Every send, in order. */
static wasmos_ipc_message_t g_sent[QUEUE_MAX];
static int g_sent_count;

/* The endpoint a live server owns; -1 when its owner has ended. */
static int32_t g_live_server = SERVER_EP;
/* Grants of the request buffer, by grantee endpoint. */
static int32_t g_grants[QUEUE_MAX];
static int g_grant_count;

static void queue(int32_t endpoint, int32_t type, int32_t request_id, int32_t source, int32_t arg0,
                  int32_t arg1, int32_t arg2) {
    wasmos_ipc_message_t m = {0};
    m.type = (uint32_t)type;
    m.request_id = (uint32_t)request_id;
    m.source = (uint32_t)source;
    m.destination = (uint32_t)endpoint;
    m.arg0 = (uint32_t)arg0;
    m.arg1 = (uint32_t)arg1;
    m.arg2 = (uint32_t)arg2;
    g_queue[g_queued].endpoint = endpoint;
    g_queue[g_queued].msg = m;
    g_queued++;
}

static int take(int32_t endpoint) {
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].endpoint != endpoint) {
            continue;
        }
        g_last = g_queue[i].msg;
        memmove(&g_queue[i], &g_queue[i + 1], (size_t)(g_queued - i - 1) * sizeof(g_queue[0]));
        g_queued--;
        return 1;
    }
    return 0;
}

int32_t wasmos_ipc_select_one(int32_t endpoint) {
    return take(endpoint) ? endpoint : WASMOS_NOENT;
}

int32_t wasmos_ipc_try_recv(int32_t endpoint) {
    return take(endpoint) ? 1 : 0;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    switch (field) {
    case WASMOS_IPC_FIELD_TYPE:
        return (int32_t)g_last.type;
    case WASMOS_IPC_FIELD_REQUEST_ID:
        return (int32_t)g_last.request_id;
    case WASMOS_IPC_FIELD_ARG0:
        return (int32_t)g_last.arg0;
    case WASMOS_IPC_FIELD_ARG1:
        return (int32_t)g_last.arg1;
    case WASMOS_IPC_FIELD_SOURCE:
        return (int32_t)g_last.source;
    case WASMOS_IPC_FIELD_DESTINATION:
        return (int32_t)g_last.destination;
    case WASMOS_IPC_FIELD_ARG2:
        return (int32_t)g_last.arg2;
    case WASMOS_IPC_FIELD_ARG3:
        return (int32_t)g_last.arg3;
    default:
        return 0;
    }
}

int32_t wasmos_ipc_send(int32_t destination, int32_t source, int32_t type, int32_t request_id,
                        int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    wasmos_ipc_message_t m = {0};
    m.type = (uint32_t)type;
    m.request_id = (uint32_t)request_id;
    m.source = (uint32_t)source;
    m.destination = (uint32_t)destination;
    m.arg0 = (uint32_t)arg0;
    m.arg1 = (uint32_t)arg1;
    m.arg2 = (uint32_t)arg2;
    m.arg3 = (uint32_t)arg3;
    if (g_sent_count < QUEUE_MAX) {
        g_sent[g_sent_count++] = m;
    }
    return 0;
}

int32_t wasmos_ipc_peer_await(int32_t waiter, int32_t awaited) {
    (void)waiter;
    return awaited == g_live_server ? 0 : WASMOS_NOENT;
}

int32_t wasmos_block_buffer_phys(void) {
    return 0x1000;
}

int32_t wasmos_block_buffer_write(int32_t phys, int32_t src, int32_t len, int32_t offset) {
    (void)phys, (void)src, (void)len, (void)offset;
    return 0;
}

int32_t wasmos_block_buffer_copy(int32_t phys, int32_t dst, int32_t len, int32_t offset) {
    (void)phys, (void)dst, (void)len, (void)offset;
    return 0;
}

int32_t wasmos_xfer_buffer_acquire(int32_t size) {
    (void)size;
    return 7;
}

int32_t wasmos_xfer_buffer_borrow(int32_t endpoint, int32_t buffer, int32_t flags) {
    (void)buffer, (void)flags;
    if (g_grant_count < QUEUE_MAX) {
        g_grants[g_grant_count++] = endpoint;
    }
    return 100 + g_grant_count;
}

int32_t wasmos_xfer_buffer_unborrow(int32_t borrow) {
    (void)borrow;
    return 0;
}

int32_t wasmos_xfer_buffer_release(int32_t buffer) {
    (void)buffer;
    return 0;
}

int32_t wasmos_xfer_buffer_write(int32_t buffer, const void* ptr, int32_t len, int32_t offset) {
    (void)buffer, (void)ptr, (void)len, (void)offset;
    return 0;
}

/* --- fixture --------------------------------------------------------------- */

static fat_block_t g_blk;
static fat_op_ctx_t g_op;

static void fixture(void) {
    memset(&g_blk, 0, sizeof(g_blk));
    memset(&g_op, 0, sizeof(g_op));
    g_queued = 0;
    g_sent_count = 0;
    g_grant_count = 0;
    g_live_server = SERVER_EP;
    fat_block_configure(&g_blk, SERVER_EP, REPLY_EP, TARGET);
    expect(fat_block_setup(&g_blk) == 0, "the block layer sets up");
    fat_block_set_owner(&g_blk, &g_op);
}

/* --- cases ----------------------------------------------------------------- */

static void test_a_read_is_sent_to_the_server_and_completes(void) {
    int ok = 0;
    fixture();
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT, "a read is submitted");
    expect(g_sent_count == 1 && (int32_t)g_sent[0].destination == SERVER_EP,
           "the read goes to the server");
    queue(REPLY_EP, BLOCK_IPC_READ_RESP, (int32_t)g_sent[0].request_id, SERVER_EP, 0, 1, 0);
    expect(fat_block_complete(&g_blk, &ok) == &g_op && ok == 1, "its reply completes it");
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_DONE, "the sector is cached");
}

/* Regression: 2026-11-21-fat-hangs-on-a-dead-block-driver -- fs-fat on a whole
 * disk looked its disk's driver up once, at mount. A driver that ended holding
 * a read never answered it, and the kernel's hangup for it was dropped as an
 * unmatched reply, so the operation waiting on the read never finished: fs-fat
 * stopped serving, and fs-manager's synchronous forward stalled behind it --
 * the path a supervised driver's restart reads its own module through. */
static void test_a_read_in_flight_when_its_server_ends_fails_gone(void) {
    int ok = 1;
    fixture();
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT, "a read is submitted");
    g_live_server = -1;
    queue(REPLY_EP, WASMOS_IPC_HANGUP, 0, 0, SERVER_EP, 0, 0);
    expect(fat_block_complete(&g_blk, &ok) == &g_op, "the hangup completes the waiting op");
    expect(ok == 0, "...as a failure");
    expect(g_op.err == WASMOS_ERR_BLOCK_DEV_GONE, "...that names the server's end");
    expect(fat_block_idle(&g_blk), "nothing is left outstanding");
}

/* Regression: 2026-11-21-fat-hangs-on-a-dead-block-driver -- after its driver
 * ended, fs-fat went on sending to the endpoint id the driver had owned, which
 * the next process to take that id would receive. */
static void test_a_read_after_its_server_ended_is_not_sent(void) {
    fixture();
    g_live_server = -1;
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_ERR, "the read fails at once");
    expect(g_op.err == WASMOS_ERR_BLOCK_DEV_GONE, "...naming the server's end");
    expect(g_sent_count == 0, "...and nothing is sent to the ended server's endpoint");
}

/* Regression: 2026-11-21-fat-write-latch-survives-a-failed-write -- a write
 * that failed left fat_block_write's yield-once latch set, because releasing
 * the op cleared only the direct-read latches. The next op's first write then
 * reported completion without being sent, and a FAT or directory sector the
 * filesystem believed written never reached the disk. */
static void test_a_failed_write_leaves_the_next_write_to_be_sent(void) {
    int ok = 1;
    fat_op_ctx_t next;
    fixture();
    expect(fat_block_write(&g_blk, 9u) == FAT_R_WAIT, "a write is submitted");
    queue(REPLY_EP,
          BLOCK_IPC_ERROR,
          (int32_t)g_sent[0].request_id,
          SERVER_EP,
          WASMOS_ERR_BLOCK_DEV_WRITE_FAILED,
          0,
          0);
    expect(fat_block_complete(&g_blk, &ok) == &g_op && ok == 0, "the write fails");
    fat_block_release(&g_blk, &g_op);

    memset(&next, 0, sizeof(next));
    fat_block_set_owner(&g_blk, &next);
    expect(fat_block_write(&g_blk, 10u) == FAT_R_WAIT, "the next op's write is submitted");
    expect(g_sent_count == 2, "...and sent");
}

/* The fixture, following its server. */
static void following_fixture(void) {
    int ok = 0;
    fixture();
    expect(fat_block_follow(&g_blk, PROC_EP, FOLLOW_EP) == 0, "the subscription is sent");
    expect(g_sent_count == 1 && (int32_t)g_sent[0].destination == PROC_EP &&
               g_sent[0].type == SVC_IPC_SUBSCRIBE_CLASS_REQ &&
               (int32_t)g_sent[0].arg0 == FOLLOW_EP,
           "...to the process manager, naming the follow endpoint for its events");
    queue(FOLLOW_EP, SVC_IPC_SUBSCRIBE_CLASS_RESP, 1, PROC_EP, 0, 0, 0);
    expect(fat_block_event(&g_blk, &ok) == 0, "its answer fails nothing");
    g_sent_count = 0;
}

static void server_ends(int with_hangup) {
    g_live_server = -1;
    if (with_hangup) {
        queue(REPLY_EP, WASMOS_IPC_HANGUP, 0, 0, SERVER_EP, 0, 0);
    }
    queue(FOLLOW_EP,
          SVC_IPC_CLASS_EVENT,
          0,
          PROC_EP,
          SVC_CLASS_EVENT_REMOVE,
          (int32_t)TARGET,
          SERVER_EP);
}

static void successor_registers(void) {
    g_live_server = SUCCESSOR_EP;
    queue(FOLLOW_EP,
          SVC_IPC_CLASS_EVENT,
          0,
          PROC_EP,
          SVC_CLASS_EVENT_ADD,
          (int32_t)TARGET,
          SUCCESSOR_EP);
}

static void test_the_successor_is_lent_the_request_buffer_and_served(void) {
    int ok = 1;
    following_fixture();
    server_ends(0);
    expect(fat_block_event(&g_blk, &ok) == 0, "a REMOVE with nothing in flight fails nothing");
    successor_registers();
    expect(fat_block_event(&g_blk, &ok) == 0, "the successor's ADD fails nothing");
    expect(g_grant_count == 2 && g_grants[1] == SUCCESSOR_EP,
           "the successor is lent the request buffer");
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT, "a read is submitted");
    expect(g_sent_count == 1 && (int32_t)g_sent[0].destination == SUCCESSOR_EP,
           "...to the successor");
}

static void test_a_read_drains_queued_events_before_failing(void) {
    following_fixture();
    server_ends(0);
    successor_registers();
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT,
           "a read issued with the successor's ADD still queued is submitted");
    expect(g_sent_count == 1 && (int32_t)g_sent[0].destination == SUCCESSOR_EP,
           "...to the successor");
}

static void test_a_read_with_no_successor_yet_fails_gone(void) {
    following_fixture();
    server_ends(0);
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_ERR, "a read with no live server fails");
    expect(g_op.err == WASMOS_ERR_BLOCK_DEV_GONE, "...naming the server's end");
    expect(g_sent_count == 0, "...and is not sent");
}

static void test_a_remove_fails_a_read_in_flight(void) {
    int ok = 1;
    following_fixture();
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT, "a read is submitted");
    server_ends(0);
    expect(fat_block_event(&g_blk, &ok) == &g_op && ok == 0, "the REMOVE fails the read");
    expect(g_op.err == WASMOS_ERR_BLOCK_DEV_GONE, "...naming the server's end");
    expect(fat_block_idle(&g_blk), "nothing is left outstanding");
}

static void test_the_hangup_and_the_remove_fail_a_read_once(void) {
    int ok = 1;
    following_fixture();
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT, "a read is submitted");
    server_ends(1);
    expect(fat_block_complete(&g_blk, &ok) == &g_op, "the hangup fails the read");
    expect(fat_block_event(&g_blk, &ok) == 0, "the REMOVE behind it fails nothing more");
}

static void test_a_stale_reply_after_a_rebind_is_dropped(void) {
    int ok = 1;
    following_fixture();
    expect(fat_need_sector(&g_blk, 5u) == FAT_R_WAIT, "a read is submitted");
    int32_t first = (int32_t)g_sent[0].request_id;
    successor_registers();
    expect(fat_block_event(&g_blk, &ok) == &g_op && ok == 0,
           "an ADD on another endpoint fails what the old instance held");
    queue(REPLY_EP, BLOCK_IPC_READ_RESP, first, SERVER_EP, 0, 1, 0);
    expect(fat_block_complete(&g_blk, &ok) == 0, "a late reply to it completes nothing");
}

int main(void) {
    test_a_read_is_sent_to_the_server_and_completes();
    test_a_read_in_flight_when_its_server_ends_fails_gone();
    test_a_read_after_its_server_ended_is_not_sent();
    test_a_failed_write_leaves_the_next_write_to_be_sent();
    test_the_successor_is_lent_the_request_buffer_and_served();
    test_a_read_drains_queued_events_before_failing();
    test_a_read_with_no_successor_yet_fails_gone();
    test_a_remove_fails_a_read_in_flight();
    test_the_hangup_and_the_remove_fail_a_read_once();
    test_a_stale_reply_after_a_rebind_is_dropped();
    printf("test_fat_block: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
