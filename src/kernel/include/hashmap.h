/* hashmap.h - Generic uint32-keyed hashmap with fixed-size inline values.
 *
 * Separate chaining with per-entry nodes, so a value pointer returned by
 * hashmap_get/hashmap_put stays valid until that key is removed (or the map is
 * destroyed) -- including across rehashes and across insert/remove of OTHER
 * keys.  This matters for callers that stash the value pointer elsewhere.
 *
 * The bucket array grows automatically (rehash) as the entry count rises, so
 * there is no fixed capacity: the map holds an arbitrary number of live keys,
 * bounded only by available memory.  Backed by the shared list allocator
 * (malloc with an early-boot arena fallback), same as list.c. */
#ifndef WASMOS_HASHMAP_H
#define WASMOS_HASHMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct hashmap_node hashmap_node_t;

/* Map handle.  Treat as opaque; use hashmap_* functions only. */
typedef struct {
    uint32_t        value_size;
    uint32_t        bucket_count;   /* always a power of two */
    uint32_t        count;          /* number of live keys */
    hashmap_node_t **buckets;
} hashmap_t;

/* Iterator.  Invalidated by any hashmap_put/hashmap_remove during traversal. */
typedef struct {
    hashmap_t      *map;
    uint32_t        bucket;
    hashmap_node_t *node;
} hashmap_iter_t;

/* Initialize a map storing value_size bytes per key.  initial_buckets is
 * rounded up to a power of two (minimum 8).  Returns 0 on success, -1 on bad
 * args or out of memory. */
int hashmap_init(hashmap_t *map, uint32_t value_size, uint32_t initial_buckets);

/* Free all entries and the bucket array. */
void hashmap_destroy(hashmap_t *map);

/* Return a pointer to the value for key, or NULL if the key is absent. */
void *hashmap_get(hashmap_t *map, uint32_t key);

/* Return the value for key, creating a new zeroed entry if absent (grows the
 * table as needed).  Returns NULL only on allocation failure. */
void *hashmap_put(hashmap_t *map, uint32_t key);

/* Remove key.  Returns 0 if removed, -1 if the key was absent. */
int hashmap_remove(hashmap_t *map, uint32_t key);

/* Number of live keys. */
uint32_t hashmap_count(const hashmap_t *map);

/* Begin iteration; returns the first value (and its key via out_key) or NULL
 * if empty.  Iteration order is unspecified. */
void *hashmap_first(hashmap_t *map, hashmap_iter_t *iter, uint32_t *out_key);

/* Advance; returns the next value (and its key) or NULL at end. */
void *hashmap_next(hashmap_iter_t *iter, uint32_t *out_key);

#endif
