#ifndef WASMOS_TEST_SHUFFLE_H
#define WASMOS_TEST_SHUFFLE_H

/* test_shuffle.h - run a suite's cases in a randomized order.
 *
 * Why. A suite written as `if (rc == 0) rc = test_a(); if (rc == 0) rc =
 * test_b();` only ever proves the cases pass IN THAT ORDER. A case that leaks
 * state -- a static left set, a global runtime pointer, a table not reset --
 * makes its neighbour pass for the wrong reason, and the suite stays green
 * until someone reorders it or a harness runs the cases differently. That is
 * not hypothetical here: the Rust coroutine suite failed roughly 1 run in 10
 * because libtest ran its cases on parallel threads and they raced on
 * coroutine_wasm.c's g_current_runtime. Order-independence is a property worth
 * testing, so this shuffles.
 *
 * Reproducibility is the whole trick. A randomized order that cannot be
 * replayed turns a real bug into a ghost, so the seed is ALWAYS printed, and
 * setting WASMOS_TEST_SEED replays that exact order.
 *
 * The generator is a fixed splitmix64 rather than the host's. srand()/rand() are
 * seed-deterministic too, but only per libc implementation: the sequence for a
 * given seed differs between Apple libc, glibc and musl, so a seed printed by a
 * CI failure on Linux would replay a DIFFERENT order on a developer's macOS box
 * -- which is the one job the seed has. (arc4random cannot be seeded at all.)
 * Five lines buys a seed that means the same thing everywhere.
 *
 * Two entry points, because the suites here come in two shapes.
 *
 * A suite whose cases return a marker:
 *     static const wasmos_test_case_t k_cases[] = {
 *         WASMOS_TEST_CASE(test_a),
 *         WASMOS_TEST_CASE(test_b),
 *     };
 *     int main(void) { return wasmos_test_run_all(k_cases, 2); }
 *
 * A suite that already has its own named table and failure counter keeps its
 * runner and shuffles the order it walks:
 *     int order[WASMOS_TEST_MAX_CASES];
 *     uint64_t seed = wasmos_test_shuffle(order, count);
 *     for (int i = 0; i < count; ++i) run(tests[order[i]]);
 *     if (failures) wasmos_test_report_seed(seed);
 */

#include <stdint.h>
#include <stdio.h>

/* These suites compile with src/libc/include on the include path, where wasmos's
 * own freestanding <stdlib.h> shadows the host's -- so the two host functions
 * used here are declared directly rather than included. The host libc is still
 * what they link against. */
extern char* getenv(const char* name);
extern unsigned long long strtoull(const char* text, char** end, int base);

#define WASMOS_TEST_MAX_CASES 128

typedef int (*wasmos_test_fn_t)(void);

typedef struct {
    const char* name;
    wasmos_test_fn_t fn;
} wasmos_test_case_t;

#define WASMOS_TEST_CASE(function)                                                                 \
    { #function, function }

static inline uint64_t wasmos_test_next_random(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static inline uint64_t wasmos_test_seed(void) {
    const char* configured = getenv("WASMOS_TEST_SEED");
    if (configured && configured[0]) {
        return strtoull(configured, 0, 0);
    }
    /* Address-space layout is the one source of per-run entropy available
     * without pulling in time or process headers, and it varies under ASLR. */
    uintptr_t entropy = (uintptr_t)(void*)&configured;
    return (uint64_t)entropy * 0x2545F4914F6CDD1Dull;
}

/**
 * Fill `order[0..count)` with a seed-determined permutation of 0..count-1 and
 * return the seed, which the caller reports if anything fails.
 */
static inline uint64_t wasmos_test_shuffle(int* order, int count) {
    uint64_t seed = wasmos_test_seed();
    uint64_t state = seed;

    if (!order || count <= 0) {
        return seed;
    }
    for (int i = 0; i < count; ++i) {
        order[i] = i;
    }
    /* Fisher-Yates, so every permutation is reachable. */
    for (int i = count - 1; i > 0; --i) {
        int j = (int)(wasmos_test_next_random(&state) % (uint64_t)(i + 1));
        int swap = order[i];
        order[i] = order[j];
        order[j] = swap;
    }
    return seed;
}

/** Print how to replay the order that just failed. */
static inline void wasmos_test_report_seed(uint64_t seed) {
    printf("  replay this order with WASMOS_TEST_SEED=0x%llx\n", (unsigned long long)seed);
}

/**
 * Run every case in a seed-determined order, stopping at the first failure.
 * Returns 0 when all pass, 1 otherwise.
 */
static inline int wasmos_test_run_all(const wasmos_test_case_t* cases, int count) {
    int order[WASMOS_TEST_MAX_CASES];
    uint64_t seed;

    if (!cases || count <= 0 || count > WASMOS_TEST_MAX_CASES) {
        printf("test_shuffle: bad case list (%d cases)\n", count);
        return 1;
    }
    seed = wasmos_test_shuffle(order, count);

    for (int i = 0; i < count; ++i) {
        const wasmos_test_case_t* test = &cases[order[i]];
        int rc = test->fn();
        if (rc != 0) {
            printf("FAIL %s (marker %d)\n", test->name, rc);
            wasmos_test_report_seed(seed);
            printf("  order was:");
            for (int j = 0; j <= i; ++j) {
                printf(" %s", cases[order[j]].name);
            }
            printf("\n");
            return 1;
        }
    }
    return 0;
}

#endif /* WASMOS_TEST_SHUFFLE_H */
