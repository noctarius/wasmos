/* list.h - Generic polymorphic list with pluggable storage backends.
 *
 * Two implementations share one API: LIST_IMPL_LINKED (slab-allocated linked
 * list, O(1) alloc/remove) and LIST_IMPL_ARRAY_CHUNK (contiguous chunk array,
 * better cache locality for small fixed-size elements).  Choose based on
 * whether random removal or iteration performance matters more. */
#ifndef WASMOS_LIST_H
#define WASMOS_LIST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LIST_IMPL_LINKED = 0,      /* slab-backed doubly-linked list */
    LIST_IMPL_ARRAY_CHUNK = 1  /* array of fixed-size chunks */
} list_impl_t;

typedef struct list_ops list_ops_t;

/* List handle.  Treat as opaque; use list_* functions only.
 * config_value is passed to the implementation at init (e.g. chunk capacity). */
typedef struct {
    uint32_t elem_size;
    list_impl_t impl;
    uint32_t config_value;
    const list_ops_t *ops;
    void *impl_state;
} list_t;

/* Iterator state for list_first / list_next traversal.
 * Invalidated by any list_alloc or list_remove during iteration. */
typedef struct {
    list_t *list;
    void *state0;
    void *state1;
    uint32_t index;
} list_iter_t;

/* Initialize a list for elements of elem_size bytes.
 * array_chunk_capacity is used only for LIST_IMPL_ARRAY_CHUNK. */
int list_init(list_t *list, uint32_t elem_size, list_impl_t impl, uint32_t array_chunk_capacity);

void list_destroy(list_t *list);

/* Allocate and append a new zeroed element; returns a pointer to it. */
void *list_alloc(list_t *list);

/* Remove elem (must be a pointer previously returned by list_alloc) from the list. */
int list_remove(list_t *list, void *elem);

/* Begin iteration; returns the first element or NULL if empty. */
void *list_first(list_t *list, list_iter_t *iter);

/* Advance to the next element; returns NULL at end. */
void *list_next(list_iter_t *iter);

#endif
