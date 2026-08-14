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
 * replayed turns a real bug into a ghost, so the seed is printed BEFORE the
 * first case runs and flushed immediately -- not on failure. A suite that
 * aborts, segfaults or hangs never reaches its failure path, and those are
 * precisely the runs whose order you need back. Setting WASMOS_TEST_SEED
 * replays it.
 *
 * The generator is a fixed splitmix64 rather than the host's. srand()/rand() are
 * seed-deterministic too, but only per libc implementation: the sequence for a
 * given seed differs between Apple libc, glibc and musl, so a seed printed by a
 * CI failure on Linux would replay a DIFFERENT order on a developer's macOS box
 * -- which is the one job the seed has. (arc4random cannot be seeded at all.)
 * Five lines buys a seed that means the same thing everywhere.
 *
 * Three entry points, because the suites here come in three shapes.
 *
 * A suite whose cases return a marker, and which stops at the first failure:
 *     static const wasmos_test_case_t k_cases[] = {
 *         WASMOS_TEST_CASE(test_a),
 *         WASMOS_TEST_CASE(test_b),
 *     };
 *     int main(void) { return wasmos_test_run_all(k_cases, 2); }
 *
 * A suite whose cases return nothing and report through their own failure
 * counter, so every case runs:
 *     static const wasmos_test_void_case_t k_cases[] = { ... };
 *     uint64_t seed = wasmos_test_run_all_void(k_cases, 2);
 *     if (failures) wasmos_test_report_seed(seed);
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
#include <stdlib.h>

/* Some of these suites compile with src/libc/include on the include path, where
 * wasmos's own freestanding headers shadow the host's and declare far less.
 * They still LINK against the host libc, so the three functions used here are
 * declared directly, with signatures identical to both the host's and wasmos's.
 * Declaring them unconditionally rather than behind a shadow-detecting guard is
 * deliberate: a guard would depend on whether this header is included before or
 * after the ones it is detecting, and that is not a property a header should
 * have. The extern "C" is load-bearing for the C++ suites: without it these
 * would be declared with C++ linkage and fail to resolve against libc. */
#ifdef __cplusplus
extern "C" {
#endif
extern char* getenv(const char* name);
extern unsigned long long strtoull(const char* text, char** end, int base);
extern long write(int fd, const void* buf, unsigned long count);
#ifdef __cplusplus
}
#endif

/* Upper bound on a suite's case count, set by the fixed-size order[] array the
 * two runners put on the stack. A larger count is rejected rather than
 * truncated, so a suite that outgrows this fails loudly. Raising it costs
 * 4 bytes of stack per case. */
#define WASMOS_TEST_MAX_CASES 128

/* A case returns 0 to pass. Any non-zero value is a failure marker naming the
 * assertion that failed -- by convention __LINE__, which the runner prints. */
typedef int (*wasmos_test_fn_t)(void);

typedef struct {
    const char* name; /* Borrowed; must outlive the run. WASMOS_TEST_CASE supplies a literal. */
    wasmos_test_fn_t fn;
} wasmos_test_case_t;

/* The other case shape in this tree: a void case that reports through the
 * suite's own failure counter rather than by returning a marker. */
typedef void (*wasmos_test_void_fn_t)(void);

typedef struct {
    const char* name; /* Borrowed; must outlive the run. */
    wasmos_test_void_fn_t fn;
} wasmos_test_void_case_t;

/* Table entry for a case, naming it with its own identifier so the name printed
 * on failure cannot drift from the function actually run. Takes a function
 * designator, not a pointer expression: the argument is stringified. */
#define WASMOS_TEST_CASE(function) {#function, function}

/* One splitmix64 draw, advancing *state in place.
 *
 * The state chains through the raw counter -- *state accumulates the golden
 * constant and the mixing is applied only to the returned value. tests/unit/as/
 * shuffle.ts chains through the mixed output instead, so the same seed yields a
 * different sequence there: a seed reproduces an order within one of the two
 * helpers, never across both. */
static inline uint64_t wasmos_test_next_random(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Write `prefix` followed by `seed` as 16 lowercase hex digits and a newline
 * straight to fd 1.
 *
 * Unbuffered, and free of any stream object: wasmos's <stdio.h> has its own
 * FILE type that is not the host's, so no FILE* may be passed across. The line
 * is assembled in a 96-byte stack buffer, and the prefix is truncated to leave
 * room for the 16 digits and the newline -- a long prefix loses its tail, the
 * seed is never lost. A write failure is ignored; there is nowhere to report it
 * that would be more reliable than the write itself. */
static inline void wasmos_test_write_seed(const char* prefix, uint64_t seed) {
    char line[96];
    unsigned pos = 0;
    while (prefix[pos] && pos + 20u < sizeof(line)) {
        line[pos] = prefix[pos];
        ++pos;
    }
    for (int shift = 60; shift >= 0; shift -= 4) {
        line[pos++] = "0123456789abcdef"[(seed >> shift) & 0xFu];
    }
    line[pos++] = '\n';
    (void)write(1, line, pos);
}

/* The seed for this run: WASMOS_TEST_SEED when set to a non-empty value,
 * otherwise one derived from the stack address of a local.
 *
 * WASMOS_TEST_SEED is parsed with base 0, so 0x-prefixed hex -- the form
 * wasmos_test_write_seed prints -- and plain decimal both work. An unparsable
 * value yields 0, which is a perfectly usable seed rather than an error, so a
 * typo silently replays the "seed 0" order instead of failing. Setting it to the
 * empty string is the same as not setting it. */
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
 *
 * `order` must have room for `count` ints; the caller owns it and it is written
 * in full. A NULL `order` or a non-positive `count` leaves it untouched and
 * returns the seed without printing it -- the seed line marks a shuffle that
 * actually happened. `count` is not checked against WASMOS_TEST_MAX_CASES here;
 * the two runners do that before calling.
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
    /* Written straight to fd 1 rather than printf'd, so there is no buffer to
     * lose: the run this seed matters most for is the one that dies before it
     * can print anything else. */
    wasmos_test_write_seed("test_shuffle: WASMOS_TEST_SEED=0x", seed);
    return seed;
}

/** Repeat the seed next to a failure, so it is not only at the top of the log. */
static inline void wasmos_test_report_seed(uint64_t seed) {
    wasmos_test_write_seed("  replay this order with WASMOS_TEST_SEED=0x", seed);
}

/**
 * Run every void case in a seed-determined order. These report failures through
 * the suite's own counter, so there is nothing to stop on; the seed is returned
 * for the caller to repeat next to its summary when that counter is non-zero.
 *
 * A NULL `cases`, a non-positive `count`, or a `count` past
 * WASMOS_TEST_MAX_CASES runs nothing, complains on stdout and returns 0 -- which
 * is indistinguishable from a legitimate seed of 0, so a caller cannot detect
 * the rejection from the return value alone. The suite's own failure counter
 * stays at whatever it was, so a rejected list reports as a pass unless the
 * suite checks that its cases ran.
 */
static inline uint64_t wasmos_test_run_all_void(const wasmos_test_void_case_t* cases, int count) {
    int order[WASMOS_TEST_MAX_CASES];
    uint64_t seed;

    if (!cases || count <= 0 || count > WASMOS_TEST_MAX_CASES) {
        printf("test_shuffle: bad case list (%d cases)\n", count);
        return 0;
    }
    seed = wasmos_test_shuffle(order, count);
    for (int i = 0; i < count; ++i) {
        cases[order[i]].fn();
    }
    return seed;
}

/**
 * Run every case in a seed-determined order, stopping at the first failure.
 * Returns 0 when all pass, 1 otherwise.
 *
 * Suitable as the whole body of main(). On failure it prints the case name and
 * its marker, repeats the seed, and lists the cases run so far in the order
 * taken -- the prefix that produced the failure. Cases after the failing one are
 * not run, so one run reports one failure. A NULL `cases`, a non-positive
 * `count`, or a `count` past WASMOS_TEST_MAX_CASES also returns 1, after
 * complaining on stdout and running nothing.
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
