/* stubs_wfs_block_server.h - a RAM-backed BLOCK server for the WFS host tests.
 *
 * Supplies the wasm IPC hostcalls the driver reaches the device through —
 * WASMOS_WASM_IMPORT expands to nothing off wasm, so these definitions link —
 * and answers BLOCK_IPC read/write requests out of an in-memory volume that
 * mkfs_wfs formatted.
 *
 * The request encoding is therefore under test alongside the driver: the fake
 * server divides the lba back out by the sector count it was handed, so a wrong
 * block-to-sector scaling shows up as a read of the wrong block rather than as a
 * silently different call.
 *
 * Nothing here reimplements scheduling. Tests link the real coroutine_wasm.c and
 * ipc_future_wasm.c and drive them through wfs_stub_run_task.
 */
#ifndef WASMOS_TEST_STUBS_WFS_BLOCK_SERVER_H
#define WASMOS_TEST_STUBS_WFS_BLOCK_SERVER_H

#include <stdint.h>

#include "wasmos/coroutine_wasm.h"
#include "wasmos/libsys.h"
#include "wfs_block.h"
#include "wfs_mkfs.h"

#define WFS_STUB_REQ_LOG_MAX 64

/* The volume, in memory. Writable so a case can corrupt a block before mounting. */
extern uint8_t* wfs_stub_image;
extern uint32_t wfs_stub_blocks;
extern uint32_t wfs_stub_block_size;

/* Requests the driver issued, in order, as FILESYSTEM block numbers. A step that
 * let a block number live on the C stack across an await reads garbage on the
 * resume path, and the only visible symptom is a request for the wrong block —
 * so the sequence is recorded, not merely counted. */
extern uint32_t wfs_stub_req_blocks[WFS_STUB_REQ_LOG_MAX];
extern uint32_t wfs_stub_req_count;
extern uint32_t wfs_stub_reads;
extern uint32_t wfs_stub_last_sectors;

/* Answer the next request with BLOCK_IPC_ERROR. Cleared once it fires. */
extern int wfs_stub_fail_next;
/* Non-zero makes wasmos_ipc_send report that failure instead of sending. */
extern int wfs_stub_send_status;
/* Fail the next staging copy into the server's block buffer, so a write cannot
 * be sent. Cleared once it fires. */
extern int wfs_stub_fail_stage;

wfs_block_t* wfs_stub_block(void);

/* The driver -> server direction of the block buffer. Declared here because the
 * suites reach it through this fixture rather than through a hostcall header off
 * wasm. */
int32_t wasmos_block_buffer_write(int32_t phys, int32_t src, int32_t len, int32_t offset);

/* The sink that writes into the fixture's image. Exposed so a test can re-format
 * the volume in place — with a tree, say — after wfs_stub_build_volume has
 * allocated it. */
int wfs_stub_sink_write(void* ctx, uint32_t block, const void* data, uint32_t len);

/* Format a volume with mkfs_wfs into a fresh in-memory image, then bind a
 * runtime, an event loop and a block client over it. Returns 0, or -1 when the
 * geometry is unusable or the allocation fails. */
int wfs_stub_build_volume(uint64_t size_bytes, uint32_t block_size,
                          const uint8_t uuid[WFS_UUID_LEN], uint64_t now_ns,
                          wfs_mkfs_layout_t* out_layout);

void wfs_stub_teardown(void);
void wfs_stub_reset_counters(void);

/* Zero the task record, start `fn` on it, and drive the runtime and the event
 * loop until it is done — which is what the driver's own loop will do: resume
 * ready tasks, then deliver replies, which wakes whatever those tasks parked on.
 *
 * The record is zeroed because wasmos_async_start accepts only a NEW or DEAD
 * record and a caller-owned record is NEW exactly when it is zero; an
 * uninitialised stack record is refused whenever the stack does not hold zero.
 *
 * Returns the task's completion status: 0, or the negative packed code it failed
 * with.
 */
int32_t wfs_stub_run_task(wasmos_wasm_coroutine_t* task, wasmos_wasm_task_resume_fn fn, void* user);

#endif /* WASMOS_TEST_STUBS_WFS_BLOCK_SERVER_H */
