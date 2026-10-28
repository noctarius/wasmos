/* test_fs_manager_clients.c — the FS manager's per-client state table
 * (fs_manager_clients.h): claiming a client's state, releasing it when the
 * client's process ends, and what a later client finds in a reused slot.
 *
 * src/services/fs_manager/fs_manager_clients.c is the only source linked in.
 * fsmgr_clients_alloc_chunk is supplied here over malloc and counts its calls,
 * which is how a case observes whether the table grew.
 *
 * The cases report through assert(), not through a failure counter: the first
 * failure aborts the process, and main() prints "ok" only if every case ran to
 * completion. The suite is compiled without -DNDEBUG, which those asserts
 * depend on.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "fs_manager_clients.h"

static uint32_t g_chunks_allocated;

void* fsmgr_clients_alloc_chunk(uint32_t size) {
    g_chunks_allocated++;
    return malloc(size);
}

static void table_free(fs_client_table_t* table) {
    fs_client_chunk_t* chunk = table->chunks;
    while (chunk) {
        fs_client_chunk_t* next = chunk->next;
        free(chunk);
        chunk = next;
    }
    table->chunks = 0;
}

static void test_a_new_client_starts_at_the_root_with_no_open_files(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* state = fsmgr_client_get(&table, 7);
    assert(state);
    assert(state->in_use);
    assert(state->context_id == 7);
    assert(strcmp(state->cwd, "/") == 0);
    assert(state->backend_endpoint == -1);
    for (uint32_t f = 0; f < FSMGR_CLIENT_FD_CAP; ++f) {
        assert(!state->fds[f].in_use);
    }
    table_free(&table);
}

static void test_a_context_keeps_one_state(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* first = fsmgr_client_get(&table, 7);
    assert(fsmgr_client_get(&table, 7) == first);
    assert(fsmgr_client_lookup(&table, 7) == first);
    assert(fsmgr_client_get(&table, 8) != first);
    table_free(&table);
}

static void test_a_released_client_has_no_state(void) {
    fs_client_table_t table = {0};
    (void)fsmgr_client_get(&table, 7);
    (void)fsmgr_client_get(&table, 8);
    fsmgr_client_release(&table, 7);
    assert(fsmgr_client_lookup(&table, 7) == 0);
    assert(fsmgr_client_lookup(&table, 8) != 0);
    /* Releasing what is not held changes nothing. */
    fsmgr_client_release(&table, 7);
    fsmgr_client_release(&table, 99);
    assert(fsmgr_client_lookup(&table, 8) != 0);
    table_free(&table);
}

/* Regression: 2026-10-28-released-client-slot-not-reused
 * Releasing a client cleared its slot but the table only ever claimed slots
 * past the high-water mark, so a released slot was never used again. Every
 * process that ever touched the filesystem still cost fs-manager one slot of its
 * bump heap, which is the leak the hangup was meant to end. */
static void test_a_released_slot_is_reused(void) {
    fs_client_table_t table = {0};
    g_chunks_allocated = 0;

    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    fsmgr_client_release(&table, 7);
    assert(fsmgr_client_get(&table, 8) == gone);

    /* Process churn: many more short-lived clients than one chunk holds, never
     * more than two alive at once, must not grow the table past one chunk. */
    for (int32_t ctx = 100; ctx < 100 + 4 * FS_CLIENT_CHUNK_CAP; ++ctx) {
        assert(fsmgr_client_get(&table, ctx));
        fsmgr_client_release(&table, ctx);
    }
    assert(g_chunks_allocated == 1u);
    table_free(&table);
}

/* A reused slot is a NEW client's state: nothing of the released one may show
 * through, or a process would start inside another's directory holding its fds. */
static void test_a_reused_slot_carries_nothing_over(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    snprintf(gone->cwd, sizeof(gone->cwd), "%s", "/wfs/docs");
    gone->mount = FS_MOUNT_BACKEND;
    gone->backend_endpoint = 42;
    gone->fds[3].in_use = 1;
    gone->fds[3].backend_endpoint = 42;
    gone->fds[3].backend_fd = 5;
    fsmgr_client_release(&table, 7);

    fs_client_state_t* fresh = fsmgr_client_get(&table, 8);
    assert(fresh->context_id == 8);
    assert(strcmp(fresh->cwd, "/") == 0);
    assert(fresh->mount == FS_MOUNT_ROOT);
    assert(fresh->backend_endpoint == -1);
    assert(!fresh->fds[3].in_use);
    table_free(&table);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_new_client_starts_at_the_root_with_no_open_files),
        WASMOS_TEST_CASE(test_a_context_keeps_one_state),
        WASMOS_TEST_CASE(test_a_released_client_has_no_state),
        WASMOS_TEST_CASE(test_a_released_slot_is_reused),
        WASMOS_TEST_CASE(test_a_reused_slot_carries_nothing_over),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_clients: ok\n");
    return 0;
}
