/* hashmap.c - Generic uint32-keyed hashmap (separate chaining, auto-growing).
 * See hashmap.h for the contract.  Node addresses are stable for a key's
 * lifetime: rehash only re-links existing nodes into a new bucket array, it
 * never moves or reallocates them, so stored value pointers stay valid. */
#include "hashmap.h"
#include "kmem.h"
#include "string.h"

struct hashmap_node {
    hashmap_node_t *next;
    uint32_t        key;
    /* value_size bytes of payload follow this header in the same allocation. */
};

#define HASHMAP_MIN_BUCKETS   8u
/* Grow when count would exceed 3/4 of the bucket count. */
#define HASHMAP_LOAD_NUM      3u
#define HASHMAP_LOAD_DEN      4u

static inline void *
node_value(hashmap_node_t *node)
{
    return (void *)((uint8_t *)node + sizeof(hashmap_node_t));
}

/* Fibonacci/Knuth multiplicative hash, masked to the (power-of-two) table. */
static inline uint32_t
hash_index(uint32_t key, uint32_t bucket_count)
{
    return (uint32_t)((key * 2654435761u)) & (bucket_count - 1u);
}

static uint32_t
round_up_pow2(uint32_t v)
{
    uint32_t p = HASHMAP_MIN_BUCKETS;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static hashmap_node_t **
alloc_buckets(uint32_t count)
{
    hashmap_node_t **b =
        (hashmap_node_t **)kmem_alloc((size_t)count * sizeof(hashmap_node_t *));
    if (b) {
        memset(b, 0, (size_t)count * sizeof(hashmap_node_t *));
    }
    return b;
}

int
hashmap_init(hashmap_t *map, uint32_t value_size, uint32_t initial_buckets)
{
    if (!map || value_size == 0) {
        return -1;
    }
    uint32_t count = round_up_pow2(initial_buckets ? initial_buckets : HASHMAP_MIN_BUCKETS);
    map->buckets = alloc_buckets(count);
    if (!map->buckets) {
        return -1;
    }
    map->value_size   = value_size;
    map->bucket_count = count;
    map->count        = 0;
    return 0;
}

void
hashmap_destroy(hashmap_t *map)
{
    if (!map || !map->buckets) {
        return;
    }
    for (uint32_t i = 0; i < map->bucket_count; ++i) {
        hashmap_node_t *node = map->buckets[i];
        while (node) {
            hashmap_node_t *next = node->next;
            kmem_free(node);
            node = next;
        }
    }
    kmem_free(map->buckets);
    map->buckets      = 0;
    map->bucket_count = 0;
    map->count        = 0;
}

static hashmap_node_t *
find_node(hashmap_t *map, uint32_t key)
{
    hashmap_node_t *node = map->buckets[hash_index(key, map->bucket_count)];
    while (node) {
        if (node->key == key) {
            return node;
        }
        node = node->next;
    }
    return 0;
}

void *
hashmap_get(hashmap_t *map, uint32_t key)
{
    if (!map || !map->buckets) {
        return 0;
    }
    hashmap_node_t *node = find_node(map, key);
    return node ? node_value(node) : 0;
}

/* Re-link every node into a fresh, larger bucket array.  Nodes are not moved,
 * so value pointers held by callers remain valid. */
static int
rehash(hashmap_t *map)
{
    uint32_t new_count = map->bucket_count << 1;
    if (new_count < map->bucket_count) {   /* overflow: keep current table */
        return 0;
    }
    hashmap_node_t **new_buckets = alloc_buckets(new_count);
    if (!new_buckets) {
        return -1;
    }
    for (uint32_t i = 0; i < map->bucket_count; ++i) {
        hashmap_node_t *node = map->buckets[i];
        while (node) {
            hashmap_node_t *next = node->next;
            uint32_t idx = hash_index(node->key, new_count);
            node->next = new_buckets[idx];
            new_buckets[idx] = node;
            node = next;
        }
    }
    kmem_free(map->buckets);
    map->buckets      = new_buckets;
    map->bucket_count = new_count;
    return 0;
}

void *
hashmap_put(hashmap_t *map, uint32_t key)
{
    if (!map || !map->buckets) {
        return 0;
    }
    hashmap_node_t *node = find_node(map, key);
    if (node) {
        return node_value(node);
    }
    /* Grow before inserting when the load factor would be exceeded. */
    if ((uint64_t)(map->count + 1) * HASHMAP_LOAD_DEN >
        (uint64_t)map->bucket_count * HASHMAP_LOAD_NUM) {
        (void)rehash(map);   /* on failure, keep going with the current table */
    }
    node = (hashmap_node_t *)kmem_alloc(sizeof(hashmap_node_t) + map->value_size);
    if (!node) {
        return 0;
    }
    node->key = key;
    memset(node_value(node), 0, map->value_size);
    uint32_t idx = hash_index(key, map->bucket_count);
    node->next = map->buckets[idx];
    map->buckets[idx] = node;
    map->count++;
    return node_value(node);
}

int
hashmap_remove(hashmap_t *map, uint32_t key)
{
    if (!map || !map->buckets) {
        return -1;
    }
    uint32_t idx = hash_index(key, map->bucket_count);
    hashmap_node_t *node = map->buckets[idx];
    hashmap_node_t *prev = 0;
    while (node) {
        if (node->key == key) {
            if (prev) {
                prev->next = node->next;
            } else {
                map->buckets[idx] = node->next;
            }
            kmem_free(node);
            map->count--;
            return 0;
        }
        prev = node;
        node = node->next;
    }
    return -1;
}

uint32_t
hashmap_count(const hashmap_t *map)
{
    return map ? map->count : 0;
}

void *
hashmap_first(hashmap_t *map, hashmap_iter_t *iter, uint32_t *out_key)
{
    if (!map || !iter || !map->buckets) {
        return 0;
    }
    iter->map    = map;
    iter->bucket = 0;
    iter->node   = 0;
    return hashmap_next(iter, out_key);
}

void *
hashmap_next(hashmap_iter_t *iter, uint32_t *out_key)
{
    if (!iter || !iter->map) {
        return 0;
    }
    hashmap_t *map = iter->map;
    /* Continue the current chain, then advance through buckets. */
    hashmap_node_t *node = iter->node ? iter->node->next : 0;
    uint32_t bucket = iter->bucket;
    while (!node && bucket < map->bucket_count) {
        node = map->buckets[bucket++];
    }
    if (!node) {
        iter->node = 0;
        return 0;
    }
    iter->bucket = bucket;
    iter->node   = node;
    if (out_key) {
        *out_key = node->key;
    }
    return node_value(node);
}
