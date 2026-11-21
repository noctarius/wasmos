/* test_fs_manager_pending.c — the FS manager's table of requests in flight to
 * backends (fs_manager_pending.h): the ids a record is matched by, what ends a
 * record's claim on a client, and finding every record a backend holds.
 *
 * src/services/fs_manager/fs_manager_pending.c is the only source linked in.
 *
 * The cases report through assert(), not through a failure counter: the first
 * failure aborts the process, and main() prints "ok" only if every case ran to
 * completion. The suite is compiled without -DNDEBUG, which those asserts
 * depend on.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "fs_manager_pending.h"

static void test_a_new_record_is_in_flight_under_a_positive_id(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    fsmgr_pending_t* entry = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    assert(entry);
    assert(entry->id > 0);
    assert(entry->backend == 40);
    assert(entry->step == FSMGR_STEP_FORWARD);
    /* Nothing lent, owned or answered until the caller says so. */
    assert(entry->client == -1);
    assert(entry->context == -1);
    assert(entry->borrow == -1);
    assert(entry->buffer == -1);
    assert(fsmgr_pending_find(&table, entry->id) == entry);
    assert(fsmgr_pending_count(&table) == 1u);
}

/* Two clients may send the same request id to the same backend, so a reply is
 * matched by fs-manager's own id, and each record must hold a different one. */
static void test_two_clients_with_one_request_id_are_told_apart(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    fsmgr_pending_t* a = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    fsmgr_pending_t* b = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    assert(a && b);
    a->client = 100;
    a->client_request_id = 7;
    b->client = 200;
    b->client_request_id = 7;
    assert(a->id != b->id);
    assert(fsmgr_pending_find(&table, a->id)->client == 100);
    assert(fsmgr_pending_find(&table, b->id)->client == 200);
}

/* A late reply to a step of a chain must not complete the step after it. */
static void test_a_renewed_record_no_longer_answers_to_its_old_id(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    fsmgr_pending_t* entry = fsmgr_pending_alloc(&table, FSMGR_STEP_READ_PATH_OPEN, 40);
    int32_t old_id = entry->id;
    int32_t new_id = fsmgr_pending_renew(&table, entry);
    assert(new_id > 0);
    assert(new_id != old_id);
    assert(fsmgr_pending_find(&table, old_id) == 0);
    assert(fsmgr_pending_find(&table, new_id) == entry);
}

/* Ids wrap to 1, never to zero or a negative, and never onto a record still in
 * flight -- a long-held request outlives any number of others. */
static void test_ids_wrap_past_the_records_still_in_flight(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    table.next_id = 1;
    fsmgr_pending_t* held = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    assert(held->id == 1);
    table.next_id = INT32_MAX;
    fsmgr_pending_t* last = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    assert(last->id == INT32_MAX);
    fsmgr_pending_t* wrapped = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    assert(wrapped->id == 2);
}

static void test_a_full_table_refuses_and_a_freed_slot_is_reused(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    fsmgr_pending_t* first = 0;
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        fsmgr_pending_t* entry = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
        assert(entry);
        if (!first) {
            first = entry;
        }
    }
    assert(fsmgr_pending_count(&table) == FSMGR_PENDING_CAP);
    assert(fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40) == 0);
    int32_t freed_id = first->id;
    fsmgr_pending_free(first);
    assert(fsmgr_pending_find(&table, freed_id) == 0);
    fsmgr_pending_t* again = fsmgr_pending_alloc(&table, FSMGR_STEP_MKDIR, 41);
    assert(again == first);
    assert(again->step == FSMGR_STEP_MKDIR);
    assert(again->backend == 41);
}

/* A record reused after a client's request carries none of it over: a stale
 * client, fd or grant would be answered or released on the next request's
 * behalf. */
static void test_a_reused_record_carries_nothing_over(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    fsmgr_pending_t* entry = fsmgr_pending_alloc(&table, FSMGR_STEP_CHDIR_STAT, 40);
    entry->client = 100;
    entry->context = 9;
    entry->borrow = 5;
    entry->buffer = 6;
    entry->client_fd = 3;
    entry->fd = 4;
    entry->result = 77;
    strcpy(entry->path, "/wfs/docs");
    fsmgr_pending_free(entry);
    fsmgr_pending_t* reused = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 41);
    assert(reused == entry);
    assert(reused->client == -1);
    assert(reused->context == -1);
    assert(reused->borrow == -1);
    assert(reused->buffer == -1);
    assert(reused->client_fd == -1);
    assert(reused->fd == -1);
    assert(reused->result == 0);
    assert(reused->path[0] == '\0');
}

/* A client that ended is answered by nobody, but its records stay in flight:
 * their replies still arrive, and what was lent for them is still lent. */
static void test_an_ended_client_leaves_its_records_in_flight_unanswered(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    fsmgr_pending_t* gone = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    fsmgr_pending_t* also_gone = fsmgr_pending_alloc(&table, FSMGR_STEP_READ_PATH_READ, 41);
    fsmgr_pending_t* other = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    gone->client = 100;
    gone->context = 9;
    gone->borrow = 5;
    also_gone->client = 100;
    also_gone->context = 9;
    other->client = 200;
    other->context = 10;
    assert(fsmgr_pending_orphan_client(&table, 100) == 2u);
    assert(fsmgr_pending_find(&table, gone->id) == gone);
    assert(gone->client == -1);
    assert(gone->context == -1);
    assert(gone->borrow == 5);
    assert(also_gone->client == -1);
    assert(other->client == 200);
    assert(other->context == 10);
    /* No record answers nobody: the internal requests are not anyone's. */
    assert(fsmgr_pending_orphan_client(&table, -1) == 0u);
    assert(fsmgr_pending_count(&table) == 3u);
}

/* A backend that ended takes every request it held with it; each is found,
 * failed and freed in turn, and the other backends' are left alone. */
static void test_every_record_a_backend_holds_is_found(void) {
    fsmgr_pending_table_t table;
    memset(&table, 0, sizeof(table));
    (void)fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 40);
    fsmgr_pending_t* kept = fsmgr_pending_alloc(&table, FSMGR_STEP_FORWARD, 41);
    (void)fsmgr_pending_alloc(&table, FSMGR_STEP_READDIR, 40);
    (void)fsmgr_pending_alloc(&table, FSMGR_STEP_UNMOUNT, 40);
    uint32_t failed = 0;
    fsmgr_pending_t* entry;
    while ((entry = fsmgr_pending_next_for_backend(&table, 40)) != 0) {
        assert(entry->backend == 40);
        fsmgr_pending_free(entry);
        failed++;
    }
    assert(failed == 3u);
    assert(fsmgr_pending_count(&table) == 1u);
    assert(fsmgr_pending_next_for_backend(&table, 41) == kept);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_new_record_is_in_flight_under_a_positive_id),
        WASMOS_TEST_CASE(test_two_clients_with_one_request_id_are_told_apart),
        WASMOS_TEST_CASE(test_a_renewed_record_no_longer_answers_to_its_old_id),
        WASMOS_TEST_CASE(test_ids_wrap_past_the_records_still_in_flight),
        WASMOS_TEST_CASE(test_a_full_table_refuses_and_a_freed_slot_is_reused),
        WASMOS_TEST_CASE(test_a_reused_record_carries_nothing_over),
        WASMOS_TEST_CASE(test_an_ended_client_leaves_its_records_in_flight_unanswered),
        WASMOS_TEST_CASE(test_every_record_a_backend_holds_is_found),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_pending: ok\n");
    return 0;
}
