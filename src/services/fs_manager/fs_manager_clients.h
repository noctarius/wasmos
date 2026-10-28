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
 * at once; one with open files keeps its slot, marked `releasing`, until
 * fsmgr_client_next_orphan_fd has handed out every file for closing -- those
 * files are still open on their backends, and the slot is where the record of
 * them lives until then. */
void fsmgr_client_release(fs_client_table_t* table, int32_t context_id);

/* Hand out one open file of a released client, so the caller can close it on
 * the backend that holds it. Returns 1 with the backend's endpoint and fd, which
 * the table forgets; 0 when no released client has an open file left. A
 * released slot whose last file is handed out is freed for reuse. */
int fsmgr_client_next_orphan_fd(fs_client_table_t* table, int32_t* out_backend_endpoint,
                                int32_t* out_backend_fd);

/* Forget every open file in `state` without closing anything on a backend. */
void fsmgr_client_reset_fds(fs_client_state_t* state);

#endif
