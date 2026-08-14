/* idtable.c - id-addressed, owner-scoped table of kernel objects.
 *
 * See idtable.h for the contract and docs/architecture/35-kernel-object-tables.md
 * for the pattern. Every function here assumes the caller's table lock is held.
 */

#include "idtable.h"

static idtable_header_t* idtable_header(void* elem) {
    return (idtable_header_t*)elem;
}

/* True while `id` belongs to a live element. Only consulted once the counter
 * has wrapped, because before that every id it produces is new by construction. */
static int idtable_id_taken(idtable_t* table, uint32_t id) {
    list_iter_t it;
    void* elem = list_first(&table->store, &it);
    while (elem) {
        idtable_header_t* header = idtable_header(elem);
        if (header->in_use && header->id == id) {
            return 1;
        }
        elem = list_next(&it);
    }
    return 0;
}

/*
 * The next id, skipping reserved values and -- once wrapped -- any id a live
 * element still holds.
 *
 * The skip is the whole point. A counter that wraps onto a live object would
 * give that object a second, ambiguous owner: traffic for the new one lands on
 * the old.
 */
static uint32_t idtable_alloc_id(idtable_t* table) {
    for (;;) {
        uint32_t id = table->next_id++;
        if (table->next_id == IDTABLE_ID_NONE) {
            table->next_id = 1u;
            table->id_wrapped = 1u;
        }
        if (id == 0u || id == IDTABLE_ID_NONE) {
            continue;
        }
        if (!table->id_wrapped || !idtable_id_taken(table, id)) {
            return id;
        }
    }
}

int idtable_init(idtable_t* table, uint32_t elem_size, uint32_t chunk_capacity,
                 uint32_t per_owner_max) {
    if (!table || elem_size < sizeof(idtable_header_t)) {
        return WASMOS_INVAL;
    }
    table->next_id = 1u;
    table->id_wrapped = 0u;
    table->per_owner_max = per_owner_max;
    table->elem_size = elem_size;
    if (list_init(&table->store, elem_size, LIST_IMPL_ARRAY_CHUNK, chunk_capacity) != 0) {
        return WASMOS_NOMEM;
    }
    return WASMOS_OK;
}

void idtable_destroy(idtable_t* table) {
    if (!table) {
        return;
    }
    list_destroy(&table->store);
    table->next_id = 1u;
    table->id_wrapped = 0u;
}

uint32_t idtable_count_for_owner(const idtable_t* table, uint32_t owner_context_id) {
    list_iter_t it;
    uint32_t used = 0;
    if (!table) {
        return 0;
    }
    /* The store is walked rather than a per-owner counter kept: allocation and
     * teardown are rare, and a counter is one more thing to keep true across
     * every release path. */
    void* elem = list_first((list_t*)&table->store, &it);
    while (elem) {
        idtable_header_t* header = idtable_header(elem);
        if (header->in_use && header->owner_context_id == owner_context_id) {
            used++;
        }
        elem = list_next(&it);
    }
    return used;
}

/* The returned element has only its idtable_header_t filled in; the payload is
 * zeroed by the store and is the caller's to populate.  The array-chunk store
 * never relocates a slot, so the pointer stays valid until the element is freed
 * or the table destroyed — but the id, not the pointer, is the handle to keep.
 *
 * *out_status is set on every path when non-NULL: WASMOS_OK, WASMOS_INVAL for a
 * NULL table, or WASMOS_FULL both for an owner at per_owner_max and for a store
 * that cannot grow. */
void* idtable_alloc(idtable_t* table, uint32_t owner_context_id, int* out_status) {
    int status = WASMOS_OK;
    void* elem = 0;

    if (!table) {
        if (out_status) {
            *out_status = WASMOS_INVAL;
        }
        return 0;
    }
    /* Checked BEFORE the store is asked to grow, so an owner at its ceiling
     * cannot take the memory another owner would have used. */
    if (table->per_owner_max != 0u &&
        idtable_count_for_owner(table, owner_context_id) >= table->per_owner_max) {
        status = WASMOS_FULL;
    } else {
        elem = list_alloc(&table->store);
        if (!elem) {
            status = WASMOS_FULL;
        }
    }
    if (!elem) {
        if (out_status) {
            *out_status = status;
        }
        return 0;
    }

    idtable_header_t* header = idtable_header(elem);
    header->id = idtable_alloc_id(table);
    header->owner_context_id = owner_context_id;
    header->in_use = 1u;
    if (out_status) {
        *out_status = WASMOS_OK;
    }
    return elem;
}

void* idtable_get(idtable_t* table, uint32_t id) {
    list_iter_t it;
    if (!table || id == 0u || id == IDTABLE_ID_NONE) {
        return 0;
    }
    void* elem = list_first(&table->store, &it);
    while (elem) {
        idtable_header_t* header = idtable_header(elem);
        if (header->in_use && header->id == id) {
            return elem;
        }
        elem = list_next(&it);
    }
    return 0;
}

int idtable_free(idtable_t* table, uint32_t id) {
    if (!table) {
        return WASMOS_INVAL;
    }
    void* elem = idtable_get(table, id);
    if (!elem) {
        return WASMOS_NOENT;
    }
    idtable_header(elem)->in_use = 0u;
    idtable_header(elem)->id = 0u;
    list_remove(&table->store, elem);
    return WASMOS_OK;
}

uint32_t idtable_release_owner(idtable_t* table, uint32_t owner_context_id,
                               void (*on_release)(void* elem, void* user), void* user) {
    uint32_t released = 0;
    if (!table) {
        return 0;
    }
    /* Restarted after each removal rather than iterated once: list_remove
     * invalidates the iterator, and a stale one would walk freed storage. */
    for (;;) {
        list_iter_t it;
        void* victim = 0;
        void* elem = list_first(&table->store, &it);
        while (elem) {
            idtable_header_t* header = idtable_header(elem);
            if (header->in_use && header->owner_context_id == owner_context_id) {
                victim = elem;
                break;
            }
            elem = list_next(&it);
        }
        if (!victim) {
            return released;
        }
        /* The callback runs while the element is still intact, which is where
         * the caller undoes what the object owns. */
        if (on_release) {
            on_release(victim, user);
        }
        idtable_header(victim)->in_use = 0u;
        idtable_header(victim)->id = 0u;
        list_remove(&table->store, victim);
        released++;
    }
}

#ifdef WASMOS_IDTABLE_TEST_SEAMS
/* Test seam, compiled only under WASMOS_IDTABLE_TEST_SEAMS.  Forces the id
 * counter and the wrapped flag so a unit test can reach the post-wrap
 * collision-avoidance path without allocating 2^32 ids.  No validation: setting
 * next_id to 0 or IDTABLE_ID_NONE is allowed and simply skipped by
 * idtable_alloc_id. */
void idtable_test_set_next_id(idtable_t* table, uint32_t next_id, int wrapped) {
    if (!table) {
        return;
    }
    table->next_id = next_id;
    table->id_wrapped = wrapped ? 1u : 0u;
}
#endif
