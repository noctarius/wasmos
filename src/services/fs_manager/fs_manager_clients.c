/* fs_manager_clients.c - the FS manager's per-client state table.
 * See fs_manager_clients.h for the contract. */
#include "fs_manager_clients.h"

#include <string.h>

/* Reset a slot to the VFS root. A client with no working directory would
 * otherwise have every relative name routed by guesswork; the root is the one
 * directory that needs no backend to name. */
static void client_slot_claim(fs_client_state_t* slot, int32_t context_id) {
    slot->in_use = 1;
    slot->context_id = context_id;
    slot->mount = FS_MOUNT_ROOT;
    slot->backend_endpoint = -1;
    slot->cwd[0] = '/';
    slot->cwd[1] = '\0';
    fsmgr_client_reset_fds(slot);
}

void fsmgr_client_reset_fds(fs_client_state_t* state) {
    if (!state) {
        return;
    }
    memset(state->fds, 0, sizeof(state->fds));
}

fs_client_state_t* fsmgr_client_lookup(fs_client_table_t* table, int32_t context_id) {
    fs_client_chunk_t* chunk = table->chunks;
    while (chunk) {
        for (uint32_t i = 0; i < chunk->used; ++i) {
            if (chunk->slots[i].in_use && chunk->slots[i].context_id == context_id) {
                return &chunk->slots[i];
            }
        }
        chunk = chunk->next;
    }
    return 0;
}

fs_client_state_t* fsmgr_client_get(fs_client_table_t* table, int32_t context_id) {
    fs_client_state_t* state = fsmgr_client_lookup(table, context_id);
    fs_client_chunk_t* chunk = table->chunks;
    fs_client_chunk_t* last = 0;
    if (state) {
        return state;
    }
    /* A released slot first: claiming past the high-water mark while one is free
     * would cost a slot of the bump heap per process that ever touched the
     * filesystem, since nothing on that heap is ever freed. */
    for (chunk = table->chunks; chunk; chunk = chunk->next) {
        for (uint32_t i = 0; i < chunk->used; ++i) {
            if (!chunk->slots[i].in_use) {
                client_slot_claim(&chunk->slots[i], context_id);
                return &chunk->slots[i];
            }
        }
    }
    chunk = table->chunks;
    while (chunk) {
        if (chunk->used < FS_CLIENT_CHUNK_CAP) {
            fs_client_state_t* slot = &chunk->slots[chunk->used++];
            client_slot_claim(slot, context_id);
            return slot;
        }
        last = chunk;
        chunk = chunk->next;
    }
    chunk = (fs_client_chunk_t*)fsmgr_clients_alloc_chunk((uint32_t)sizeof(fs_client_chunk_t));
    if (!chunk) {
        return 0;
    }
    memset(chunk, 0, sizeof(*chunk));
    if (last) {
        last->next = chunk;
    } else {
        table->chunks = chunk;
    }
    chunk->used = 1;
    client_slot_claim(&chunk->slots[0], context_id);
    return &chunk->slots[0];
}

void fsmgr_client_release(fs_client_table_t* table, int32_t context_id) {
    fs_client_state_t* state = fsmgr_client_lookup(table, context_id);
    if (!state) {
        return;
    }
    fsmgr_client_reset_fds(state);
    state->in_use = 0;
    state->context_id = -1;
    state->backend_endpoint = -1;
}
