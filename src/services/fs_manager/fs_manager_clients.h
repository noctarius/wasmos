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

/* Release the state held for `context_id`, whose process has ended: its working
 * directory and its fd mappings. Releasing a context with no state is a no-op. */
void fsmgr_client_release(fs_client_table_t* table, int32_t context_id);

/* Forget every open file in `state` without closing anything on a backend. */
void fsmgr_client_reset_fds(fs_client_state_t* state);

#endif
