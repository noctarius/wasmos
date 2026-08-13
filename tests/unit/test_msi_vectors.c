/* Host unit test for the MSI vector bookkeeping (src/kernel/msi_vectors.c).
 *
 * The properties under test are the ones a driver's correctness rests on: a
 * vector has exactly one owner, a freed vector stops being delivered, and a
 * reaped context cannot leave a vector pointing at a released endpoint (the
 * failure mode that strands an INTx line — see test_irq_sharing.c). See
 * docs/architecture/05-x86-cpu-architecture.md §Message-Signalled Interrupts. */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "msi_vectors.h"

#define TEST_VECTORS MSI_VECTOR_COUNT

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

/* ---- fake ops -------------------------------------------------------------- */

static uint32_t g_delivered_to[64];
static uint32_t g_delivered_index[64];
static uint32_t g_deliver_count;
/* Endpoint whose delivery fails, modelling a full endpoint queue. 0 = none. */
static uint32_t g_deliver_fail_endpoint;

static int fake_deliver(uint32_t endpoint, uint32_t index) {
    if (g_deliver_fail_endpoint != 0 && endpoint == g_deliver_fail_endpoint) {
        return -1;
    }
    if (g_deliver_count < 64) {
        g_delivered_to[g_deliver_count] = endpoint;
        g_delivered_index[g_deliver_count] = index;
    }
    g_deliver_count++;
    return 0;
}

static const msi_vector_ops_t OPS = {fake_deliver};

static msi_vector_t g_vectors[TEST_VECTORS];

static void reset(void) {
    memset(g_vectors, 0, sizeof(g_vectors));
    msi_vectors_init(g_vectors, TEST_VECTORS);
    g_deliver_count = 0;
    g_deliver_fail_endpoint = 0;
    memset(g_delivered_to, 0, sizeof(g_delivered_to));
    memset(g_delivered_index, 0, sizeof(g_delivered_index));
}

/* ---- tests ----------------------------------------------------------------- */

static void test_alloc_hands_out_distinct_vectors(void) {
    reset();
    uint32_t a = 0xFFFFFFFFu;
    uint32_t b = 0xFFFFFFFFu;
    expect(msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &a) == 0, "first alloc succeeds");
    expect(msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x101, &b) == 0, "second alloc succeeds");
    expect(a != b, "two allocations never return the same vector");
    expect(msi_vectors_in_use(g_vectors, TEST_VECTORS, a), "first vector is marked in use");
    expect(msi_vectors_in_use(g_vectors, TEST_VECTORS, b), "second vector is marked in use");
}

static void test_alloc_exhaustion_is_reported(void) {
    reset();
    uint32_t index = 0;
    for (uint32_t i = 0; i < TEST_VECTORS; ++i) {
        expect(msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100 + i, &index) == 0,
               "allocating up to the table size succeeds");
    }
    expect(msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x200, &index) ==
               WASMOS_ERR_MSI_NO_VECTORS,
           "one allocation past the table size is refused");
}

static void test_dispatch_reaches_the_owner_with_its_index(void) {
    reset();
    uint32_t a = 0;
    uint32_t b = 0;
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &a);
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 9, 0x200, &b);

    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, b, &OPS) == 1,
           "dispatch reports delivery");
    expect(g_deliver_count == 1, "exactly one endpoint is notified");
    expect(g_delivered_to[0] == 0x200, "the event goes to the vector's own owner only");
    /* This is what INTx could never do: the index tells the driver which of its
     * own interrupt sources fired, with no device register read. */
    expect(g_delivered_index[0] == b, "the event carries the vector index that fired");
}

static void test_dispatch_of_a_free_vector_is_inert(void) {
    reset();
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, 3, &OPS) == 0,
           "an unallocated vector delivers nothing");
    expect(g_deliver_count == 0, "no endpoint is notified");
}

static void test_dispatch_out_of_range_is_inert(void) {
    reset();
    uint32_t a = 0;
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &a);
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, TEST_VECTORS, &OPS) == 0,
           "an index past the table is refused");
    expect(g_deliver_count == 0, "no endpoint is notified");
}

static void test_failed_delivery_is_reported(void) {
    reset();
    uint32_t a = 0;
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &a);
    g_deliver_fail_endpoint = 0x100;
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, a, &OPS) == 0,
           "a refused delivery is not counted as delivered");
    /* Unlike an IRQ line, a failed MSI delivery strands nothing: there is no
     * mask to lift, so the vector keeps working on the next message. */
    expect(msi_vectors_in_use(g_vectors, TEST_VECTORS, a), "the vector stays allocated");
    g_deliver_fail_endpoint = 0;
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, a, &OPS) == 1,
           "the next message is delivered normally");
}

static void test_free_stops_delivery_and_recycles(void) {
    reset();
    uint32_t a = 0;
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &a);
    expect(msi_vectors_free(g_vectors, TEST_VECTORS, a, 7) == 0, "the owner may free its vector");
    expect(!msi_vectors_in_use(g_vectors, TEST_VECTORS, a), "the vector is free again");
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, a, &OPS) == 0,
           "a freed vector delivers nothing");

    uint32_t b = 0;
    expect(msi_vectors_alloc(g_vectors, TEST_VECTORS, 9, 0x200, &b) == 0, "the slot is reusable");
    expect(b == a, "the lowest free slot is handed out again");
}

static void test_free_rejects_non_owner_and_bad_vector(void) {
    reset();
    uint32_t a = 0;
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &a);
    expect(msi_vectors_free(g_vectors, TEST_VECTORS, a, 9) == WASMOS_ERR_MSI_NOT_OWNER,
           "another context cannot free someone else's vector");
    expect(msi_vectors_in_use(g_vectors, TEST_VECTORS, a), "the vector survives the attempt");
    expect(msi_vectors_free(g_vectors, TEST_VECTORS, TEST_VECTORS, 7) == WASMOS_ERR_MSI_BAD_VECTOR,
           "an index past the table is refused");
    expect(msi_vectors_free(g_vectors, TEST_VECTORS, a + 1u, 7) == WASMOS_ERR_MSI_BAD_VECTOR,
           "freeing an unallocated vector is refused");
}

static void test_release_context_drops_only_its_own(void) {
    reset();
    uint32_t mine_a = 0;
    uint32_t mine_b = 0;
    uint32_t theirs = 0;
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x100, &mine_a);
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 9, 0x200, &theirs);
    (void)msi_vectors_alloc(g_vectors, TEST_VECTORS, 7, 0x101, &mine_b);

    msi_vectors_release_context(g_vectors, TEST_VECTORS, 7);
    expect(!msi_vectors_in_use(g_vectors, TEST_VECTORS, mine_a),
           "reaped context's vector is freed");
    expect(!msi_vectors_in_use(g_vectors, TEST_VECTORS, mine_b), "all of its vectors are freed");
    expect(msi_vectors_in_use(g_vectors, TEST_VECTORS, theirs), "another context is untouched");

    /* The point of the sweep: a message from a device whose driver died must not
     * be delivered to a recycled endpoint id. */
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, mine_a, &OPS) == 0,
           "a dead context's vector delivers nothing");
    expect(msi_vectors_dispatch(g_vectors, TEST_VECTORS, theirs, &OPS) == 1,
           "the survivor still receives");
}

static void test_null_table_is_tolerated(void) {
    uint32_t index = 0;
    msi_vectors_init(0, TEST_VECTORS);
    expect(msi_vectors_alloc(0, TEST_VECTORS, 7, 0x100, &index) == WASMOS_ERR_MSI_NO_VECTORS,
           "alloc on a null table is refused");
    expect(msi_vectors_free(0, TEST_VECTORS, 0, 7) == WASMOS_ERR_MSI_BAD_VECTOR,
           "free on a null table is refused");
    expect(msi_vectors_dispatch(0, TEST_VECTORS, 0, &OPS) == 0,
           "dispatch on a null table delivers nothing");
    msi_vectors_release_context(0, TEST_VECTORS, 7);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_alloc_hands_out_distinct_vectors),
        WASMOS_TEST_CASE(test_alloc_exhaustion_is_reported),
        WASMOS_TEST_CASE(test_dispatch_reaches_the_owner_with_its_index),
        WASMOS_TEST_CASE(test_dispatch_of_a_free_vector_is_inert),
        WASMOS_TEST_CASE(test_dispatch_out_of_range_is_inert),
        WASMOS_TEST_CASE(test_failed_delivery_is_reported),
        WASMOS_TEST_CASE(test_free_stops_delivery_and_recycles),
        WASMOS_TEST_CASE(test_free_rejects_non_owner_and_bad_vector),
        WASMOS_TEST_CASE(test_release_context_drops_only_its_own),
        WASMOS_TEST_CASE(test_null_table_is_tolerated),
    };
    const uint64_t seed = wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_msi_vectors: %d passed, %d failed\n", g_checks - g_failures, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
