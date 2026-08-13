/* Unit test for the native-service slab allocator (src/libsys/native/heap_native.c).
 *
 * heap_native.c is compiled separately with malloc/free/calloc/realloc renamed to
 * hn_* (see the run-kernel-unit-tests wiring), so it links alongside host libc.
 * vm_map/vm_unmap are mocked with host malloc/free, isolating the allocator's
 * logic from the kernel's physical-page behavior. Covers the small
 * (slab) and large (per-mapping) paths, realloc data preservation, calloc
 * zeroing, corruption detection, and a mixed stress loop that mimics parsing a
 * large certificate bundle (many small allocs plus some > 4096 B). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos_native_driver.h"

extern void wasmos_native_heap_init(wasmos_driver_api_t* api);
extern void* hn_malloc(size_t size);
extern void hn_free(void* ptr);
extern void* hn_calloc(size_t count, size_t size);
extern void* hn_realloc(void* ptr, size_t size);

static int g_failures = 0;
static int g_corrupt = 0;

/* Strong override of heap_native.c's weak hook: record instead of proc_exit. */
void heap_corruption_detected(const char* reason, const void* p) {
    (void)reason;
    (void)p;
    g_corrupt = 1;
}

static void expect(int cond, const char* what) {
    if (!cond) {
        printf("  [FAIL] %s\n", what);
        g_failures++;
    }
}

static void* mock_vm_map(uint32_t size) {
    /* heap_native aligns its blocks internally, so host malloc's alignment is
     * sufficient for a logic test (the real vm_map returns page-aligned pages). */
    return malloc(size);
}
static void mock_vm_unmap(void* addr, uint32_t size) {
    (void)size;
    free(addr);
}

static int filled_with(const void* p, uint32_t n, uint8_t v) {
    const uint8_t* b = p;
    for (uint32_t i = 0; i < n; ++i) {
        if (b[i] != v) {
            return 0;
        }
    }
    return 1;
}

static uint32_t g_rng = 0x12345678u;
static uint32_t xr(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

#define STRESS_N 4000
static void* g_ptrs[STRESS_N];
static uint32_t g_sizes[STRESS_N];

/* Heap state is process-global and deliberately NOT reset between cases: the
 * allocator has to survive any interleaving, which is what the shuffled order
 * checks. */
static void test_small_alloc_holds_its_data(void) {
    uint8_t* p = hn_malloc(100);
    expect(p != NULL, "malloc(100) non-NULL");
    memset(p, 0xAB, 100);
    expect(filled_with(p, 100, 0xAB), "small block holds its data");
    hn_free(p);
}

static void test_calloc_zeroes(void) {
    uint8_t* p = hn_calloc(64, 4);
    expect(p != NULL, "calloc non-NULL");
    expect(filled_with(p, 256, 0), "calloc zeroes the block");
    hn_free(p);
}

/* An allocation above HEAP_MAX_SMALL_ALLOCATION (4096) gets its own mapping
 * with a LargeAllocationHeader; free() must recover that header from the same
 * offset malloc placed it at, or it reports corruption instead of unmapping. */
static void test_large_alloc_and_free(void) {
    uint8_t* p = hn_malloc(20000);
    expect(p != NULL, "malloc(20000) large non-NULL");
    memset(p, 0x5A, 20000);
    expect(filled_with(p, 20000, 0x5A), "large block holds its data");
    g_corrupt = 0;
    hn_free(p);
    expect(g_corrupt == 0, "freeing a large block is not flagged as corruption");
}

static void test_realloc_grow_preserves_prefix(void) {
    uint8_t* p = hn_malloc(50);
    memset(p, 0x11, 50);
    uint8_t* q = hn_realloc(p, 5000); /* small -> large */
    expect(q != NULL, "realloc grow non-NULL");
    expect(filled_with(q, 50, 0x11), "realloc grow preserves prefix");
    hn_free(q);
}

/* A shrink that still fits the block's size class is served in place: only
 * requested_size changes, so the retained prefix must be byte-identical. */
static void test_realloc_shrink_preserves_prefix(void) {
    uint8_t* p = hn_malloc(2000);
    memset(p, 0x22, 2000);
    uint8_t* q = hn_realloc(p, 100);
    expect(q != NULL, "realloc shrink non-NULL");
    expect(filled_with(q, 100, 0x22), "realloc shrink preserves prefix");
    hn_free(q);
}

static void test_realloc_null_allocates_and_zero_frees(void) {
    uint8_t* p = hn_realloc(NULL, 128);
    expect(p != NULL, "realloc(NULL, n) allocates");
    uint8_t* q = hn_realloc(p, 0);
    expect(q == NULL, "realloc(p, 0) returns NULL");
}

static void test_free_null_is_safe(void) {
    g_corrupt = 0;
    hn_free(NULL);
    expect(g_corrupt == 0, "free(NULL) is safe");
}

static void test_double_free_is_detected(void) {
    uint8_t* p = hn_malloc(64);
    hn_free(p);
    g_corrupt = 0;
    hn_free(p); /* second free of a small block */
    expect(g_corrupt == 1, "double free is detected");
    g_corrupt = 0;
}

/* Mimics a large cert-bundle parse: thousands of mostly-small allocations plus
 * some large ones, accumulated then freed over several rounds, verifying
 * integrity throughout (catches overlapping/aliased blocks). */
static void test_mixed_stress(void) {
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < STRESS_N; ++i) {
            uint32_t r = xr();
            uint32_t sz = (r % 100u < 90u) ? (16u + (r % 2032u)) : (4097u + (r % 8000u));
            void* p = hn_malloc(sz);
            expect(p != NULL, "stress malloc non-NULL");
            if (p == NULL) {
                break;
            }
            memset(p, (uint8_t)(i & 0xFF), sz);
            g_ptrs[i] = p;
            g_sizes[i] = sz;
        }
        for (int i = 0; i < STRESS_N; ++i) {
            if (g_ptrs[i] && !filled_with(g_ptrs[i], g_sizes[i], (uint8_t)(i & 0xFF))) {
                expect(0, "stress block integrity");
                break;
            }
        }
        for (int i = 0; i < STRESS_N; ++i) {
            int j = (int)(xr() % STRESS_N);
            if (!g_ptrs[j]) {
                continue;
            }
            if ((xr() & 3u) == 0u) {
                uint32_t ns = 16u + (xr() % 4000u);
                void* q = hn_realloc(g_ptrs[j], ns);
                if (q != NULL) {
                    uint32_t keep = ns < g_sizes[j] ? ns : g_sizes[j];
                    if (!filled_with(q, keep, (uint8_t)(j & 0xFF))) {
                        expect(0, "stress realloc preserves prefix");
                    }
                    memset(q, (uint8_t)(j & 0xFF), ns);
                    g_ptrs[j] = q;
                    g_sizes[j] = ns;
                }
            } else {
                hn_free(g_ptrs[j]);
                g_ptrs[j] = NULL;
            }
        }
        for (int i = 0; i < STRESS_N; ++i) {
            if (g_ptrs[i]) {
                hn_free(g_ptrs[i]);
                g_ptrs[i] = NULL;
            }
        }
    }
    expect(g_corrupt == 0, "no corruption across stress rounds");
}

int main(void) {
    wasmos_driver_api_t api;
    memset(&api, 0, sizeof(api));
    api.vm_map = mock_vm_map;
    api.vm_unmap = mock_vm_unmap;
    wasmos_native_heap_init(&api);

    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_small_alloc_holds_its_data),
        WASMOS_TEST_CASE(test_calloc_zeroes),
        WASMOS_TEST_CASE(test_large_alloc_and_free),
        WASMOS_TEST_CASE(test_realloc_grow_preserves_prefix),
        WASMOS_TEST_CASE(test_realloc_shrink_preserves_prefix),
        WASMOS_TEST_CASE(test_realloc_null_allocates_and_zero_frees),
        WASMOS_TEST_CASE(test_free_null_is_safe),
        WASMOS_TEST_CASE(test_double_free_is_detected),
        WASMOS_TEST_CASE(test_mixed_stress),
    };
    const uint64_t seed = wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));

    if (g_failures != 0) {
        printf("test_heap_native: %d FAILED\n", g_failures);
        wasmos_test_report_seed(seed);
        return 1;
    }
    printf("test_heap_native: ok\n");
    return 0;
}
