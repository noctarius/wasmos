/* fs_manager_clients.c - the FS manager's per-client state table.
 * See fs_manager_clients.h for the contract. */
#include "fs_manager_clients.h"

#include <string.h>

/* Reset a slot to the VFS root. A client with no working directory would
 * otherwise have every relative name routed by guesswork; the root is the one
 * directory that needs no backend to name. */
static void client_slot_claim(fs_client_state_t* slot, int32_t context_id) {
    slot->in_use = 1;
    slot->releasing = 0;
    slot->context_id = context_id;
    slot->mount = FS_MOUNT_ROOT;
    slot->backend_endpoint = -1;
    slot->cwd[0] = '/';
    slot->cwd[1] = '\0';
    fsmgr_client_reset_fds(slot);
}

static void client_slot_free(fs_client_state_t* slot) {
    fsmgr_client_reset_fds(slot);
    slot->in_use = 0;
    slot->releasing = 0;
    slot->context_id = -1;
    slot->backend_endpoint = -1;
}

static int client_slot_has_open_fds(const fs_client_state_t* slot) {
    for (uint32_t f = 0; f < FSMGR_CLIENT_FD_CAP; ++f) {
        if (slot->fds[f].in_use) {
            return 1;
        }
    }
    return 0;
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
            fs_client_state_t* slot = &chunk->slots[i];
            if (slot->in_use && !slot->releasing && slot->context_id == context_id) {
                return slot;
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
    if (!client_slot_has_open_fds(state)) {
        client_slot_free(state);
        return;
    }
    state->releasing = 1;
    state->context_id = -1;
    state->mount = FS_MOUNT_ROOT;
    state->backend_endpoint = -1;
    state->cwd[0] = '\0';
}

int fsmgr_client_next_orphan_fd(fs_client_table_t* table, int32_t* out_backend_endpoint,
                                int32_t* out_backend_fd) {
    for (fs_client_chunk_t* chunk = table->chunks; chunk; chunk = chunk->next) {
        for (uint32_t i = 0; i < chunk->used; ++i) {
            fs_client_state_t* slot = &chunk->slots[i];
            if (!slot->in_use || !slot->releasing) {
                continue;
            }
            for (uint32_t f = 0; f < FSMGR_CLIENT_FD_CAP; ++f) {
                fsmgr_client_fd_t* fd = &slot->fds[f];
                if (!fd->in_use) {
                    continue;
                }
                *out_backend_endpoint = fd->backend_endpoint;
                *out_backend_fd = fd->backend_fd;
                fd->in_use = 0;
                fd->backend_endpoint = -1;
                fd->backend_fd = -1;
                if (!client_slot_has_open_fds(slot)) {
                    client_slot_free(slot);
                }
                return 1;
            }
            /* Nothing left to close: a backend that went away took its files
             * with it (fsmgr_forget_backend_in_clients). */
            client_slot_free(slot);
        }
    }
    return 0;
}
