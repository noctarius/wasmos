#include "hashmap.h"
#include <stdint.h>

typedef struct {
    uint32_t tag;
    uint32_t value;
} test_val_t;

static int
test_init_validation(void)
{
    hashmap_t map;
    if (hashmap_init(0, sizeof(test_val_t), 8) == 0) return __LINE__;
    if (hashmap_init(&map, 0, 8) == 0) return __LINE__;
    if (hashmap_init(&map, sizeof(test_val_t), 0) != 0) return __LINE__; /* 0 -> default */
    hashmap_destroy(&map);
    return 0;
}

static int
test_put_get_remove(void)
{
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0) return __LINE__;

    if (hashmap_get(&map, 42) != 0) return __LINE__;          /* absent */
    test_val_t *a = (test_val_t *)hashmap_put(&map, 42);
    if (!a) return __LINE__;
    if (a->tag != 0 || a->value != 0) return __LINE__;        /* zeroed */
    a->tag = 1; a->value = 100;

    /* get-or-create is idempotent: same key returns the same storage. */
    test_val_t *a2 = (test_val_t *)hashmap_put(&map, 42);
    if (a2 != a) return __LINE__;
    if (a2->value != 100) return __LINE__;
    if ((test_val_t *)hashmap_get(&map, 42) != a) return __LINE__;
    if (hashmap_count(&map) != 1) return __LINE__;

    if (hashmap_remove(&map, 42) != 0) return __LINE__;
    if (hashmap_remove(&map, 42) != 0 - 1) return __LINE__;    /* absent now */
    if (hashmap_get(&map, 42) != 0) return __LINE__;
    if (hashmap_count(&map) != 0) return __LINE__;

    hashmap_destroy(&map);
    return 0;
}

/* Insert far more keys than the initial bucket count to force rehash, and
 * confirm every key is still retrievable with the right value + count. */
static int
test_growth_and_lookup(void)
{
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0) return __LINE__;

    const uint32_t N = 500;
    for (uint32_t k = 1; k <= N; ++k) {
        test_val_t *v = (test_val_t *)hashmap_put(&map, k);
        if (!v) return __LINE__;
        v->value = k * 7u;
    }
    if (hashmap_count(&map) != N) return __LINE__;
    if (map.bucket_count <= 8) return __LINE__;                /* must have grown */

    for (uint32_t k = 1; k <= N; ++k) {
        test_val_t *v = (test_val_t *)hashmap_get(&map, k);
        if (!v || v->value != k * 7u) return __LINE__;
    }
    /* Use large/sparse keys too (hash spread, not just dense ids). */
    test_val_t *big = (test_val_t *)hashmap_put(&map, 0xDEADBEEFu);
    if (!big) return __LINE__;
    big->value = 1234;
    if (((test_val_t *)hashmap_get(&map, 0xDEADBEEFu))->value != 1234) return __LINE__;

    hashmap_destroy(&map);
    return 0;
}

/* A value pointer must stay valid across rehash (triggered by later inserts)
 * and across removal of OTHER keys. */
static int
test_pointer_stability(void)
{
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0) return __LINE__;

    test_val_t *pinned = (test_val_t *)hashmap_put(&map, 7);
    if (!pinned) return __LINE__;
    pinned->tag = 0xABCD; pinned->value = 999;

    /* Force several rehashes. */
    for (uint32_t k = 100; k < 400; ++k) {
        if (!hashmap_put(&map, k)) return __LINE__;
    }
    if (pinned->tag != 0xABCD || pinned->value != 999) return __LINE__;
    if ((test_val_t *)hashmap_get(&map, 7) != pinned) return __LINE__;

    /* Remove a bunch of other keys; pinned must be untouched. */
    for (uint32_t k = 100; k < 400; ++k) {
        if (hashmap_remove(&map, k) != 0) return __LINE__;
    }
    if (pinned->tag != 0xABCD || pinned->value != 999) return __LINE__;
    if ((test_val_t *)hashmap_get(&map, 7) != pinned) return __LINE__;

    hashmap_destroy(&map);
    return 0;
}

static int
test_iteration(void)
{
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0) return __LINE__;

    const uint32_t N = 64;
    for (uint32_t k = 1; k <= N; ++k) {
        test_val_t *v = (test_val_t *)hashmap_put(&map, k);
        if (!v) return __LINE__;
        v->value = k;
    }

    hashmap_iter_t it;
    uint32_t seen = 0;
    uint64_t key_sum = 0, val_sum = 0;
    uint32_t key = 0;
    for (test_val_t *v = (test_val_t *)hashmap_first(&map, &it, &key);
         v != 0;
         v = (test_val_t *)hashmap_next(&it, &key)) {
        seen++;
        key_sum += key;
        val_sum += v->value;
    }
    hashmap_destroy(&map);
    if (seen != N) return __LINE__;
    /* sum 1..N */
    if (key_sum != (uint64_t)N * (N + 1) / 2) return __LINE__;
    if (val_sum != key_sum) return __LINE__;
    return 0;
}

int
main(void)
{
    int rc = 0;
    rc = test_init_validation();   if (rc != 0) return rc;
    rc = test_put_get_remove();    if (rc != 0) return rc;
    rc = test_growth_and_lookup(); if (rc != 0) return rc;
    rc = test_pointer_stability(); if (rc != 0) return rc;
    rc = test_iteration();         if (rc != 0) return rc;
    return 0;
}
