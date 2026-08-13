/* test_idtable.c — the id-addressed, owner-scoped object table (idtable.h,
 * docs/architecture/35-kernel-object-tables.md).
 *
 * The cases below are the contract its callers -- the ipc.c endpoint and select
 * tables -- depend on: a store that grows out of kmem, ids that never collide
 * with a live object even across a counter wrap, lookup from nothing but an id,
 * a per-owner bound so one context cannot take every slot, and wholesale
 * release when a context dies. A wrapped id landing on a live endpoint and an
 * unbounded table starving its other owners have each been a real bug.
 */

#include <stdint.h>

#include "idtable.h"
#include "test_shuffle.h"

#include <stdio.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* An element embeds the header FIRST, then whatever it likes. */
typedef struct {
    idtable_header_t header;
    uint32_t payload;
} thing_t;

#define CHUNK_CAPACITY 8u
#define PER_OWNER_MAX 4u

static void table_init(idtable_t* table, uint32_t per_owner_max) {
    CHECK(idtable_init(table, sizeof(thing_t), CHUNK_CAPACITY, per_owner_max) == WASMOS_OK,
          "the table initialises");
}

/* Allocation hands back a usable element with a non-reserved id, and lookup
 * finds exactly that element. */
static void test_alloc_assigns_an_id_and_lookup_finds_it(void) {
    idtable_t table;
    table_init(&table, 0);

    int status = WASMOS_OK;
    thing_t* a = (thing_t*)idtable_alloc(&table, 7u, &status);
    CHECK(a != 0 && status == WASMOS_OK, "an element is allocated");
    CHECK(a->header.id != 0 && a->header.id != IDTABLE_ID_NONE, "with a usable id");
    CHECK(a->header.owner_context_id == 7u, "recording its owner");
    a->payload = 0xC0FFEEu;

    thing_t* b = (thing_t*)idtable_alloc(&table, 7u, &status);
    CHECK(b != 0 && b->header.id != a->header.id, "a second element gets a different id");

    CHECK((thing_t*)idtable_get(&table, a->header.id) == a, "lookup finds the first");
    CHECK(((thing_t*)idtable_get(&table, a->header.id))->payload == 0xC0FFEEu, "with its payload");
    CHECK((thing_t*)idtable_get(&table, b->header.id) == b, "and the second");
    CHECK(idtable_get(&table, 0u) == 0, "id 0 is never an object");
    CHECK(idtable_get(&table, IDTABLE_ID_NONE) == 0, "nor is the reserved id");

    idtable_destroy(&table);
}

/* Freeing releases the id for reuse and makes lookup fail, and freeing an id
 * that is not live is refused rather than corrupting a neighbour. */
static void test_free_releases_the_slot(void) {
    idtable_t table;
    table_init(&table, 0);

    int status = WASMOS_OK;
    thing_t* a = (thing_t*)idtable_alloc(&table, 1u, &status);
    const uint32_t id = a->header.id;
    CHECK(idtable_free(&table, id) == WASMOS_OK, "a live id is freed");
    CHECK(idtable_get(&table, id) == 0, "and can no longer be found");
    CHECK(idtable_free(&table, id) == WASMOS_NOENT, "freeing it twice reports NOENT");
    CHECK(idtable_free(&table, 0u) == WASMOS_NOENT, "as does freeing a reserved id");

    idtable_destroy(&table);
}

/* Ids come from an incrementing counter, so once it wraps it must skip the ids
 * still held by live objects: a wrapped id colliding with a live object would
 * give that object a second, ambiguous owner. */
static void test_a_wrapped_id_skips_live_objects(void) {
    idtable_t table;
    table_init(&table, 0);

    int status = WASMOS_OK;
    idtable_test_set_next_id(&table, 1u, 0);
    thing_t* low = (thing_t*)idtable_alloc(&table, 1u, &status);
    CHECK(low->header.id == 1u, "the low id is taken");

    /* Drive the counter to the wrap point and over it. */
    idtable_test_set_next_id(&table, IDTABLE_ID_NONE - 1u, 0);
    thing_t* last = (thing_t*)idtable_alloc(&table, 1u, &status);
    thing_t* wrapped = (thing_t*)idtable_alloc(&table, 1u, &status);
    CHECK(last != 0 && wrapped != 0, "both allocations succeed across the wrap");
    CHECK(wrapped->header.id != low->header.id, "the wrapped id skips the live object");
    CHECK(wrapped->header.id != 0 && wrapped->header.id != IDTABLE_ID_NONE,
          "and is never a reserved value");
    CHECK((thing_t*)idtable_get(&table, low->header.id) == low,
          "so the original is still reachable by its own id");

    /* The counter can only produce a reserved value if something seeds it there,
     * which the id allocation refuses to hand out regardless. The invariant is
     * "never a reserved id", not "never a reserved id as long as nobody seeds
     * the counter". */
    idtable_test_set_next_id(&table, 0u, 0);
    thing_t* from_zero = (thing_t*)idtable_alloc(&table, 1u, &status);
    CHECK(from_zero != 0 && from_zero->header.id != 0u, "a counter seeded at 0 skips it");

    idtable_test_set_next_id(&table, IDTABLE_ID_NONE, 0);
    thing_t* from_none = (thing_t*)idtable_alloc(&table, 1u, &status);
    CHECK(from_none != 0 && from_none->header.id != IDTABLE_ID_NONE,
          "and one seeded at the reserved id skips that too");

    idtable_destroy(&table);
}

/* The quota is per owner, and the property that matters is the NEIGHBOUR still
 * working -- a global limit would satisfy the refusal on its own. */
static void test_the_quota_is_per_owner(void) {
    idtable_t table;
    table_init(&table, PER_OWNER_MAX);

    int status = WASMOS_OK;
    uint32_t n = 0;
    while (n < PER_OWNER_MAX + 4u && idtable_alloc(&table, 1u, &status) != 0) {
        n++;
    }
    CHECK(n == PER_OWNER_MAX, "an owner gets exactly its quota");
    CHECK(status == WASMOS_FULL, "and the next allocation reports FULL");

    thing_t* neighbour = (thing_t*)idtable_alloc(&table, 2u, &status);
    CHECK(neighbour != 0 && status == WASMOS_OK, "another owner is not starved");
    CHECK(idtable_count_for_owner(&table, 1u) == PER_OWNER_MAX, "the greedy owner is counted");
    CHECK(idtable_count_for_owner(&table, 2u) == 1u, "and so is the neighbour, separately");

    /* Releasing frees the quota, not just the slot. */
    CHECK(idtable_free(&table, neighbour->header.id) == WASMOS_OK, "the neighbour releases");
    CHECK(idtable_count_for_owner(&table, 2u) == 0u, "and stops being counted");

    idtable_destroy(&table);
}

/* A quota of zero means unbounded, so a table that does not want one does not
 * have to invent a number. */
static void test_a_zero_quota_is_unbounded(void) {
    idtable_t table;
    table_init(&table, 0);

    int status = WASMOS_OK;
    uint32_t n = 0;
    while (n < CHUNK_CAPACITY * 4u && idtable_alloc(&table, 1u, &status) != 0) {
        n++;
    }
    CHECK(n == CHUNK_CAPACITY * 4u, "allocation continues past several chunks");
    CHECK(idtable_count_for_owner(&table, 1u) == n, "and every one is counted");

    idtable_destroy(&table);
}

static uint32_t g_released;
static uint32_t g_released_payload_sum;

static void on_release(void* elem, void* user) {
    thing_t* t = (thing_t*)elem;
    (void)user;
    g_released++;
    g_released_payload_sum += t->payload;
}

/* A dying context takes its objects with it, and only its own: releasing by
 * owner is how process teardown avoids leaking the whole table. */
static void test_release_owner_takes_only_that_owner(void) {
    idtable_t table;
    table_init(&table, 0);

    int status = WASMOS_OK;
    for (uint32_t i = 0; i < 3u; ++i) {
        thing_t* t = (thing_t*)idtable_alloc(&table, 1u, &status);
        t->payload = 10u;
    }
    thing_t* keep = (thing_t*)idtable_alloc(&table, 2u, &status);
    keep->payload = 99u;

    g_released = 0;
    g_released_payload_sum = 0;
    CHECK(idtable_release_owner(&table, 1u, on_release, 0) == 3u, "every object of the owner goes");
    CHECK(g_released == 3u, "the callback saw each one");
    CHECK(g_released_payload_sum == 30u, "with its payload still intact when called");
    CHECK(idtable_count_for_owner(&table, 1u) == 0u, "the owner holds nothing after");
    CHECK(idtable_count_for_owner(&table, 2u) == 1u, "and the other owner is untouched");
    CHECK((thing_t*)idtable_get(&table, keep->header.id) == keep, "whose object is still there");

    idtable_destroy(&table);
}

/* Bad arguments are refused with a named status rather than a bare -1. */
static void test_bad_arguments_are_refused(void) {
    idtable_t table;
    int status = WASMOS_OK;

    CHECK(idtable_init(0, sizeof(thing_t), CHUNK_CAPACITY, 0) == WASMOS_INVAL,
          "a NULL table is refused");
    CHECK(idtable_init(&table, sizeof(idtable_header_t) - 1u, CHUNK_CAPACITY, 0) == WASMOS_INVAL,
          "an element too small to hold the header is refused");

    table_init(&table, 0);
    CHECK(idtable_alloc(0, 1u, &status) == 0, "allocating from a NULL table is refused");
    CHECK(status == WASMOS_INVAL, "with INVAL");
    CHECK(idtable_get(0, 1u) == 0, "as is looking up in one");
    CHECK(idtable_free(0, 1u) == WASMOS_INVAL, "and freeing from one");
    CHECK(idtable_count_for_owner(0, 1u) == 0u, "counting one is empty, not a crash");
    idtable_destroy(&table);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_alloc_assigns_an_id_and_lookup_finds_it),
        WASMOS_TEST_CASE(test_free_releases_the_slot),
        WASMOS_TEST_CASE(test_a_wrapped_id_skips_live_objects),
        WASMOS_TEST_CASE(test_the_quota_is_per_owner),
        WASMOS_TEST_CASE(test_a_zero_quota_is_unbounded),
        WASMOS_TEST_CASE(test_release_owner_takes_only_that_owner),
        WASMOS_TEST_CASE(test_bad_arguments_are_refused),
    };
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    const uint64_t seed = wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));

    printf("test_idtable: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
