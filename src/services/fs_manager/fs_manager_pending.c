/* fs_manager_pending.c - the FS manager's table of requests in flight to backends.
 * See fs_manager_pending.h for the contract. */
#include "fs_manager_pending.h"

#include <string.h>

static int id_in_flight(const fsmgr_pending_table_t* table, int32_t id) {
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        if (table->slots[i].in_use && table->slots[i].id == id) {
            return 1;
        }
    }
    return 0;
}

/* Positive, and held by no record in flight. The table is far smaller than the
 * id space, so the search ends within FSMGR_PENDING_CAP + 1 candidates. */
static int32_t next_free_id(fsmgr_pending_table_t* table) {
    for (;;) {
        int32_t id = table->next_id;
        table->next_id = (id <= 0 || id == INT32_MAX) ? 1 : id + 1;
        if (id > 0 && !id_in_flight(table, id)) {
            return id;
        }
    }
}

fsmgr_pending_t* fsmgr_pending_alloc(fsmgr_pending_table_t* table, fsmgr_step_t step,
                                     int32_t backend) {
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        fsmgr_pending_t* entry = &table->slots[i];
        if (entry->in_use) {
            continue;
        }
        memset(entry, 0, sizeof(*entry));
        entry->id = next_free_id(table);
        entry->in_use = 1;
        entry->step = (uint8_t)step;
        entry->backend = backend;
        entry->client = -1;
        entry->context = -1;
        entry->borrow = -1;
        entry->buffer = -1;
        entry->client_fd = -1;
        entry->client_buffer = -1;
        entry->fd = -1;
        return entry;
    }
    return 0;
}

int32_t fsmgr_pending_renew(fsmgr_pending_table_t* table, fsmgr_pending_t* entry) {
    entry->id = next_free_id(table);
    return entry->id;
}

fsmgr_pending_t* fsmgr_pending_find(fsmgr_pending_table_t* table, int32_t id) {
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        if (table->slots[i].in_use && table->slots[i].id == id) {
            return &table->slots[i];
        }
    }
    return 0;
}

fsmgr_pending_t* fsmgr_pending_next_for_backend(fsmgr_pending_table_t* table, int32_t backend) {
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        if (table->slots[i].in_use && table->slots[i].backend == backend) {
            return &table->slots[i];
        }
    }
    return 0;
}

uint32_t fsmgr_pending_orphan_client(fsmgr_pending_table_t* table, int32_t endpoint) {
    uint32_t n = 0;
    if (endpoint < 0) {
        return 0;
    }
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        fsmgr_pending_t* entry = &table->slots[i];
        if (entry->in_use && entry->client == endpoint) {
            entry->client = -1;
            entry->context = -1;
            n++;
        }
    }
    return n;
}

uint32_t fsmgr_pending_count(const fsmgr_pending_table_t* table) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        if (table->slots[i].in_use) {
            n++;
        }
    }
    return n;
}

void fsmgr_pending_free(fsmgr_pending_t* entry) {
    if (entry) {
        entry->in_use = 0;
    }
}
