/* idtable.h - id-addressed, owner-scoped table of kernel objects.
 *
 * The shape ipc.c had written out longhand twice, and that keeps recurring: a
 * table of kernel objects that
 *
 *   - grows on demand out of kmem rather than being a fixed array,
 *   - hands out ids that never collide with a live object, even after the id
 *     counter wraps,
 *   - is looked up by id from a caller that holds nothing but that id,
 *   - is bounded per owning context, so one context cannot starve every other,
 *   - and is released wholesale when a context dies.
 *
 * Each of those has already cost a real bug: a wrapped id colliding with a live
 * endpoint gave it a second, ambiguous owner, and an unbounded table let one
 * context take every slot. Writing them out per table means getting them right
 * per table.
 *
 * See docs/architecture/35-kernel-object-tables.md for when to reach for this
 * and what it deliberately does not do.
 *
 * USAGE. The element type embeds idtable_header_t as its FIRST member:
 *
 *     typedef struct {
 *         idtable_header_t header;
 *         ... whatever the object is ...
 *     } my_object_t;
 *
 * LOCKING IS THE CALLER'S. This holds no lock of its own, deliberately: its two
 * intended callers have different lock orders (the endpoint table takes the
 * table lock and then the per-endpoint lock, so that a lookup cannot be raced by
 * a release), and a lock inside here would either duplicate or invert them. Every
 * function below must be called with the caller's table lock held.
 */
#ifndef WASMOS_IDTABLE_H
#define WASMOS_IDTABLE_H

#include <stdint.h>

#include "list.h"
#include "wasmos_status.h"

/* Reserved: never handed out, so a caller can use it as "no object". Matches
 * IPC_ENDPOINT_NONE, which is what the endpoint table already reserves. */
#define IDTABLE_ID_NONE 0xFFFFFFFFu

/* Embedded as the FIRST member of every element. */
typedef struct {
    uint32_t id; /* 0 while free; never IDTABLE_ID_NONE */
    uint32_t owner_context_id;
    uint8_t in_use;
} idtable_header_t;

typedef struct {
    list_t store;
    uint32_t next_id;
    uint8_t id_wrapped; /* once set, every id is checked against live objects */
    uint32_t per_owner_max;
    uint32_t elem_size;
} idtable_t;

/**
 * Prepare a table of `elem_size` elements, `chunk_capacity` per kmem chunk.
 *
 * `per_owner_max` of 0 means unbounded -- a table with no starvation concern
 * does not have to invent a number. Returns WASMOS_INVAL if the element is too
 * small to hold the header, or WASMOS_NOMEM if the backing store cannot be
 * created.
 */
int idtable_init(idtable_t* table, uint32_t elem_size, uint32_t chunk_capacity,
                 uint32_t per_owner_max);

void idtable_destroy(idtable_t* table);

/**
 * Allocate a zeroed element owned by `owner_context_id`, with its header filled
 * in. Returns the element, or NULL with *out_status set: WASMOS_FULL when the
 * owner is at its quota or the store cannot grow, WASMOS_INVAL for a bad table.
 *
 * The quota is checked BEFORE the store is asked to grow, so an owner at its
 * ceiling cannot take the memory another owner would have used.
 */
void* idtable_alloc(idtable_t* table, uint32_t owner_context_id, int* out_status);

/** The live element with this id, or NULL. Reserved ids are never live. */
void* idtable_get(idtable_t* table, uint32_t id);

/** Release one element by id. WASMOS_NOENT if no live element has that id. */
int idtable_free(idtable_t* table, uint32_t id);

/** How many live elements `owner_context_id` holds. */
uint32_t idtable_count_for_owner(const idtable_t* table, uint32_t owner_context_id);

/**
 * Release every element owned by `owner_context_id`, returning how many went.
 *
 * `on_release` runs for each element while it is still intact, which is where a
 * caller undoes what the object owns (waiters to wake, a poll registration to
 * drop). NULL if there is nothing to undo.
 */
uint32_t idtable_release_owner(idtable_t* table, uint32_t owner_context_id,
                               void (*on_release)(void* elem, void* user), void* user);

#ifdef WASMOS_IDTABLE_TEST_SEAMS
/* Drive the id counter to a chosen point, so a test can reach the wrap without
 * allocating four billion objects. */
void idtable_test_set_next_id(idtable_t* table, uint32_t next_id, int wrapped);
#endif

#endif
