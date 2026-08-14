/* test_hashmap.c — the uint32-keyed hash map (hashmap.h).
 *
 * hashmap.c and kmem.c are compiled in for real; only the slab allocator
 * underneath kmem is replaced, by tests/unit/stubs_slab.c, which forwards to the
 * host heap. Allocation failure paths are therefore not reachable here.
 *
 * The properties under test are the ones callers rely on and a rehash can break:
 * put is get-or-create and returns the same storage for a repeated key, a value
 * pointer survives both rehash and the removal of other keys, and iteration
 * visits every live entry exactly once. The map reads map.bucket_count directly
 * to confirm growth actually happened rather than inferring it from the entry
 * count.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all
 * shuffles the cases and stops at the first failure (test_shuffle.h).
 */
#include "hashmap.h"
#include <stdint.h>

#include "test_shuffle.h"

/* Payload element. Both fields are the test's own: `tag` is a sentinel used to
 * detect a value that moved or was overwritten during a rehash. */
typedef struct {
    uint32_t tag;
    uint32_t value;
} test_val_t;

static int test_init_validation(void) {
    hashmap_t map;
    if (hashmap_init(0, sizeof(test_val_t), 8) == 0)
        return __LINE__;
    if (hashmap_init(&map, 0, 8) == 0)
        return __LINE__;
    if (hashmap_init(&map, sizeof(test_val_t), 0) != 0)
        return __LINE__; /* 0 -> default */
    hashmap_destroy(&map);
    return 0;
}

static int test_put_get_remove(void) {
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0)
        return __LINE__;

    if (hashmap_get(&map, 42) != 0)
        return __LINE__; /* absent */
    test_val_t* a = (test_val_t*)hashmap_put(&map, 42);
    if (!a)
        return __LINE__;
    if (a->tag != 0 || a->value != 0)
        return __LINE__; /* zeroed */
    a->tag = 1;
    a->value = 100;

    /* get-or-create is idempotent: same key returns the same storage. */
    test_val_t* a2 = (test_val_t*)hashmap_put(&map, 42);
    if (a2 != a)
        return __LINE__;
    if (a2->value != 100)
        return __LINE__;
    if ((test_val_t*)hashmap_get(&map, 42) != a)
        return __LINE__;
    if (hashmap_count(&map) != 1)
        return __LINE__;

    if (hashmap_remove(&map, 42) != 0)
        return __LINE__;
    if (hashmap_remove(&map, 42) != 0 - 1)
        return __LINE__; /* absent now */
    if (hashmap_get(&map, 42) != 0)
        return __LINE__;
    if (hashmap_count(&map) != 0)
        return __LINE__;

    hashmap_destroy(&map);
    return 0;
}

/* Insert far more keys than the initial bucket count to force rehash, and
 * confirm every key is still retrievable with the right value + count. */
static int test_growth_and_lookup(void) {
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0)
        return __LINE__;

    const uint32_t N = 500;
    for (uint32_t k = 1; k <= N; ++k) {
        test_val_t* v = (test_val_t*)hashmap_put(&map, k);
        if (!v)
            return __LINE__;
        v->value = k * 7u;
    }
    if (hashmap_count(&map) != N)
        return __LINE__;
    if (map.bucket_count <= 8)
        return __LINE__; /* must have grown */

    for (uint32_t k = 1; k <= N; ++k) {
        test_val_t* v = (test_val_t*)hashmap_get(&map, k);
        if (!v || v->value != k * 7u)
            return __LINE__;
    }
    /* Use large/sparse keys too (hash spread, not just dense ids). */
    test_val_t* big = (test_val_t*)hashmap_put(&map, 0xDEADBEEFu);
    if (!big)
        return __LINE__;
    big->value = 1234;
    if (((test_val_t*)hashmap_get(&map, 0xDEADBEEFu))->value != 1234)
        return __LINE__;

    hashmap_destroy(&map);
    return 0;
}

/* A value pointer must stay valid across rehash (triggered by later inserts)
 * and across removal of OTHER keys. */
static int test_pointer_stability(void) {
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0)
        return __LINE__;

    test_val_t* pinned = (test_val_t*)hashmap_put(&map, 7);
    if (!pinned)
        return __LINE__;
    pinned->tag = 0xABCD;
    pinned->value = 999;

    /* Force several rehashes. */
    for (uint32_t k = 100; k < 400; ++k) {
        if (!hashmap_put(&map, k))
            return __LINE__;
    }
    if (pinned->tag != 0xABCD || pinned->value != 999)
        return __LINE__;
    if ((test_val_t*)hashmap_get(&map, 7) != pinned)
        return __LINE__;

    /* Remove a bunch of other keys; pinned must be untouched. */
    for (uint32_t k = 100; k < 400; ++k) {
        if (hashmap_remove(&map, k) != 0)
            return __LINE__;
    }
    if (pinned->tag != 0xABCD || pinned->value != 999)
        return __LINE__;
    if ((test_val_t*)hashmap_get(&map, 7) != pinned)
        return __LINE__;

    hashmap_destroy(&map);
    return 0;
}

static int test_iteration(void) {
    hashmap_t map;
    if (hashmap_init(&map, sizeof(test_val_t), 8) != 0)
        return __LINE__;

    const uint32_t N = 64;
    for (uint32_t k = 1; k <= N; ++k) {
        test_val_t* v = (test_val_t*)hashmap_put(&map, k);
        if (!v)
            return __LINE__;
        v->value = k;
    }

    hashmap_iter_t it;
    uint32_t seen = 0;
    uint64_t key_sum = 0, val_sum = 0;
    uint32_t key = 0;
    for (test_val_t* v = (test_val_t*)hashmap_first(&map, &it, &key); v != 0;
         v = (test_val_t*)hashmap_next(&it, &key)) {
        seen++;
        key_sum += key;
        val_sum += v->value;
    }
    hashmap_destroy(&map);
    if (seen != N)
        return __LINE__;
    /* sum 1..N */
    if (key_sum != (uint64_t)N * (N + 1) / 2)
        return __LINE__;
    if (val_sum != key_sum)
        return __LINE__;
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_init_validation),   WASMOS_TEST_CASE(test_put_get_remove),
        WASMOS_TEST_CASE(test_growth_and_lookup), WASMOS_TEST_CASE(test_pointer_stability),
        WASMOS_TEST_CASE(test_iteration),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
