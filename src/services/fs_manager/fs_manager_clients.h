/* fs_manager_clients.h - the FS manager's per-client state table
 *
 * One fs_client_state_t per client CONTEXT: its working directory and its open
 * files. Split out of fs_manager.c, which cannot be linked without IPC and
 * transfer-buffer stubs, so the table's lifecycle is testable on its own.
 * tests/unit/test_fs_manager_clients.c links this translation unit alone.
 */
#ifndef WASMOS_FS_MANAGER_CLIENTS_H
#define WASMOS_FS_MANAGER_CLIENTS_H

#include <stdint.h>

#include "fs_manager_types.h"

typedef struct fs_client_chunk {
    struct fs_client_chunk* next;
    /* Slots ever claimed in this chunk; slots[used..] have never held a client. */
    uint32_t used;
    fs_client_state_t slots[FS_CLIENT_CHUNK_CAP];
} fs_client_chunk_t;

/* The table is a list of fixed-size chunks that only grows: a chunk is never
 * freed, because fs-manager's heap is a bump allocator. */
typedef struct {
    fs_client_chunk_t* chunks;
} fs_client_table_t;

/* Memory for one chunk, or NULL when the heap is exhausted. Provided by the
 * linking unit: fs_manager.c's bump heap in the service, malloc in the host test.
 * The table zeroes what it receives. */
void* fsmgr_clients_alloc_chunk(uint32_t size);

/* The state held for `context_id`, or NULL when it has none. */
fs_client_state_t* fsmgr_client_lookup(fs_client_table_t* table, int32_t context_id);

/* The state held for `context_id`, created at the VFS root with no open files
 * when it has none. Returns NULL only when a new chunk was needed and the heap
 * is exhausted. */
fs_client_state_t* fsmgr_client_get(fs_client_table_t* table, int32_t context_id);

/* Release the state held for `context_id`, whose process has ended. Releasing a
 * context with no state is a no-op.
 *
 * Its working directory is dropped at once. A client with no open files is freed
 * at once; one with open files keeps its slot, marked `releasing`, until every
 * file is confirmed closed on its backend (fsmgr_client_orphan_closed) or its
 * backend is gone -- those files are still open there, and the slot is where the
 * record of them lives until then. */
void fsmgr_client_release(fs_client_table_t* table, int32_t context_id);

/* The next file of a released client that needs a close sent: open, and not
 * already awaiting one. NULL when there is none. Released slots left with no
 * open files are freed on the way.
 *
 * The caller sends the close and reports it with fsmgr_client_orphan_sent. A
 * close that could not be sent is simply not reported: the file stays eligible
 * and is returned again by a later call. */
fsmgr_client_fd_t* fsmgr_client_next_orphan(fs_client_table_t* table);

/* Record that the close for `fd` went out with `request_id`. */
void fsmgr_client_orphan_sent(fsmgr_client_fd_t* fd, int32_t request_id);

/* The reply to the close sent with `request_id` arrived. With `retry` set (the
 * backend was transiently unable to serve it) the file becomes eligible again;
 * otherwise its record is dropped and a released slot left empty is freed.
 * Returns 1 when `request_id` named an outstanding close, 0 for any other
 * message -- including a hangup, which carries none. */
int fsmgr_client_orphan_closed(fs_client_table_t* table, int32_t request_id, int retry);

/* Closes sent and not yet answered. Derived from the records, not counted
 * separately, so a backend that dies with closes outstanding stops counting
 * against the cap as soon as its files are forgotten. */
uint32_t fsmgr_client_orphans_in_flight(const fs_client_table_t* table);

/* Forget every open file in `state` without closing anything on a backend. */
void fsmgr_client_reset_fds(fs_client_state_t* state);

#endif
