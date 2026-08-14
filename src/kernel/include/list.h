/* list.h - Generic polymorphic list with pluggable storage backends.
 *
 * Two implementations share one API and one iterator type, so callers never see
 * which is in use.  Both are backed by kmem and neither carries a lock: a list
 * shared across CPUs needs the caller's own.
 *
 * ELEMENT OWNERSHIP.  The list owns the storage.  list_alloc hands out an
 * interior pointer into a node (LINKED) or a chunk slot (ARRAY_CHUNK); the
 * caller never frees it and must return exactly that pointer to list_remove.
 * The pointer stays valid until that element is removed or the list is
 * destroyed -- allocating or removing OTHER elements never moves it, in either
 * backend.
 *
 * ITERATOR INVALIDATION.  An iterator is bound to the list it was started on
 * and holds the position it last returned.  Any list_remove during a walk
 * invalidates it: under LINKED the node is freed, so the next list_next reads
 * freed memory; under ARRAY_CHUNK the slot is cleared but the walk's own cursor
 * survives, which is a difference callers must not rely on.  Restart the walk
 * after any mutation, or collect victims first and remove them afterwards (the
 * restart-after-each-removal loop in idtable_release_owner is the in-tree
 * pattern).  list_alloc during a walk is not fatal but is not ordered either:
 * the new element may land in a slot the iterator has already passed and go
 * unseen.  Iteration order is unspecified in both backends and differs between
 * them; do not depend on it.
 *
 * CHOOSING A BACKEND.  Pick ARRAY_CHUNK for many small elements of a fixed
 * size that are allocated and freed repeatedly (its slots are reused, and one
 * kmem allocation covers a whole chunk); pick LINKED when elements are few or
 * long-lived, since its slots are never reused and its removal is a scan. */
#ifndef WASMOS_LIST_H
#define WASMOS_LIST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* One kmem allocation per element, pushed at the HEAD of a singly-linked
     * chain: list_alloc is O(1) but iteration yields elements in reverse
     * allocation order, and list_remove is an O(n) scan for the matching
     * payload address that frees the node outright.  config_value is unused, so
     * the list is bounded only by kmem. */
    LIST_IMPL_LINKED = 0,
    /* Elements packed into chunks of config_value slots, one kmem allocation
     * per chunk.  list_alloc reuses the first free slot and is therefore
     * O(total slots), allocating a new chunk only when every existing one is
     * full; list_remove just clears the slot's in-use flag, and chunks are
     * released only by list_destroy -- never when they drain.  Chunks are
     * linked at the head, so iteration visits the newest chunk first. */
    LIST_IMPL_ARRAY_CHUNK = 1
} list_impl_t;

typedef struct list_ops list_ops_t;

/* List handle.  Treat as opaque; use list_* functions only.
 * config_value is passed to the implementation at init (e.g. chunk capacity).
 * ops is NULL before a successful list_init and again after list_destroy, which
 * is what makes every call below degrade to NULL/-1 instead of faulting. */
typedef struct {
    uint32_t elem_size;
    list_impl_t impl;
    uint32_t config_value;
    const list_ops_t* ops;
    void* impl_state;
} list_t;

/* Iterator state for list_first / list_next traversal.
 * Invalidated by any list_alloc or list_remove during iteration. */
typedef struct {
    list_t* list;
    void* state0;
    void* state1;
    uint32_t index;
} list_iter_t;

/* Initialize a list for elements of elem_size bytes.
 * array_chunk_capacity is used only for LIST_IMPL_ARRAY_CHUNK.
 * Returns 0 on success, -1 for a NULL list, elem_size 0, an unknown impl, a
 * zero array_chunk_capacity under LIST_IMPL_ARRAY_CHUNK, or a failed kmem
 * allocation.  On failure the handle is left zeroed, so calling the rest of the
 * API on it is safe (everything returns NULL/-1). */
int list_init(list_t* list, uint32_t elem_size, list_impl_t impl, uint32_t array_chunk_capacity);

/* Free all backing storage, invalidating every element pointer and iterator.
 * Idempotent: the handle is left with ops == NULL. */
void list_destroy(list_t* list);

/* Allocate a new zeroed element and return the pointer the caller uses; NULL if
 * the list was never initialised or kmem is exhausted.  The element is added,
 * but not appended in any observable order -- see the backend notes above. */
void* list_alloc(list_t* list);

/* Remove elem (must be a pointer previously returned by list_alloc) from the
 * list.  Returns 0 if it was removed, -1 if the list is uninitialised or the
 * pointer names no live element (including a double remove).  Invalidates any
 * live iterator; under LIST_IMPL_LINKED it also frees the memory elem points
 * into. */
int list_remove(list_t* list, void* elem);

/* Begin iteration; returns the first element or NULL if empty.  Rebinds *iter
 * to this list, so one iterator can be reused across lists sequentially. */
void* list_first(list_t* list, list_iter_t* iter);

/* Advance to the next element; returns NULL at end.  Undefined if the list was
 * mutated since the iterator's last call -- see ITERATOR INVALIDATION above. */
void* list_next(list_iter_t* iter);

#endif
