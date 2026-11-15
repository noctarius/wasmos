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

/* Hand out the next orphan, report its close sent with `request_id`, and confirm
 * it -- the whole happy path of one file. */
static void close_next_orphan(fs_client_table_t* table, int32_t request_id) {
    fsmgr_client_fd_t* fd = fsmgr_client_next_orphan(table);
    assert(fd);
    fsmgr_client_orphan_sent(fd, request_id);
    assert(fsmgr_client_orphan_closed(table, request_id, 0) == 1);
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
    close_next_orphan(&table, 1);

    fs_client_state_t* fresh = fsmgr_client_get(&table, 8);
    assert(fresh->context_id == 8);
    assert(strcmp(fresh->cwd, "/") == 0);
    assert(fresh->mount == FS_MOUNT_ROOT);
    assert(fresh->backend_endpoint == -1);
    assert(!fresh->fds[3].in_use);
    table_free(&table);
}

/* Regression: 2026-10-28-dead-client-fds-stay-open-on-backend
 * Releasing a client dropped its fd mappings and nothing else, so every file it
 * left open stayed open on the backend for that backend's whole life. A tmpfs
 * has 32 open-file slots; the 33rd process to exit with a file open made every
 * later open on that mount fail. A released client's files must be handed out
 * for closing, each exactly once. */
static void test_a_released_clients_open_files_are_handed_out_to_close(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    gone->fds[0] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 40, .backend_fd = 3};
    gone->fds[9] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 41, .backend_fd = 12};
    fsmgr_client_release(&table, 7);

    int seen_40 = 0, seen_41 = 0;
    for (int32_t i = 0; i < 2; ++i) {
        fsmgr_client_fd_t* fd = fsmgr_client_next_orphan(&table);
        assert(fd);
        seen_40 += (fd->backend_endpoint == 40 && fd->backend_fd == 3);
        seen_41 += (fd->backend_endpoint == 41 && fd->backend_fd == 12);
        fsmgr_client_orphan_sent(fd, 10 + i);
    }
    assert(seen_40 == 1 && seen_41 == 1);
    assert(fsmgr_client_next_orphan(&table) == 0);
    table_free(&table);
}

/* While its files are still open on a backend, a released client's slot is
 * nobody's: it answers no lookup, stands in no directory, and is not handed to a
 * new client -- whose state would otherwise be the dead one's open files. */
static void test_a_releasing_slot_is_nobodys_until_its_files_are_closed(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    snprintf(gone->cwd, sizeof(gone->cwd), "%s", "/wfs/docs");
    gone->fds[2] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 40, .backend_fd = 3};
    fsmgr_client_release(&table, 7);

    assert(fsmgr_client_lookup(&table, 7) == 0);
    assert(gone->releasing);
    assert(gone->cwd[0] == '\0');
    assert(fsmgr_client_get(&table, 8) != gone);

    close_next_orphan(&table, 1);
    assert(!gone->in_use);
    assert(fsmgr_client_get(&table, 9) == gone);
    table_free(&table);
}

/* A released client whose backend already took its files away (the backend died
 * or was unmounted) has nothing to close, and its slot must not be stranded. */
static void test_a_releasing_slot_with_nothing_left_is_freed(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    gone->fds[2] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 40, .backend_fd = 3};
    fsmgr_client_release(&table, 7);
    gone->fds[2].in_use = 0; /* as fsmgr_forget_backend_in_clients does */

    assert(fsmgr_client_next_orphan(&table) == 0);
    assert(!gone->in_use);
    table_free(&table);
}

/* Regression: 2026-11-02-orphan-close-lost-on-failure
 * Handing a released client's file out erased its record before the close was
 * even sent, so a send refused by a full queue, or a close the backend refused
 * as FS_BUSY, lost the only record of a file still open on the backend. The
 * record must last until a reply confirms the close. */
static void test_an_orphan_survives_a_close_that_did_not_happen(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    gone->fds[4] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 40, .backend_fd = 6};
    fsmgr_client_release(&table, 7);

    /* The send failed, so nothing was reported: the same file comes back. */
    fsmgr_client_fd_t* fd = fsmgr_client_next_orphan(&table);
    assert(fd && fd->backend_endpoint == 40 && fd->backend_fd == 6);
    fd = fsmgr_client_next_orphan(&table);
    assert(fd && fd->backend_endpoint == 40 && fd->backend_fd == 6);

    /* Sent, then refused as busy: eligible again. */
    fsmgr_client_orphan_sent(fd, 21);
    assert(fsmgr_client_next_orphan(&table) == 0);
    assert(fsmgr_client_orphan_closed(&table, 21, 1) == 1);
    fd = fsmgr_client_next_orphan(&table);
    assert(fd && fd->backend_fd == 6);

    /* Sent again and confirmed: gone, and the slot with it. */
    fsmgr_client_orphan_sent(fd, 22);
    assert(fsmgr_client_orphan_closed(&table, 22, 0) == 1);
    assert(fsmgr_client_next_orphan(&table) == 0);
    assert(!gone->in_use);
    table_free(&table);
}

/* Regression: 2026-11-02-orphan-close-count-drift
 * Outstanding closes were a counter decremented for ANY message on the release
 * endpoint, so a hangup from a dying backend counted as a reply, and a backend
 * that died with closes outstanding never gave its share back -- sixteen of
 * those and cleanup stopped on every mount. Only a reply naming an outstanding
 * close counts, and a forgotten file stops counting. */
static void test_only_a_matching_reply_completes_a_close(void) {
    fs_client_table_t table = {0};
    fs_client_state_t* gone = fsmgr_client_get(&table, 7);
    gone->fds[0] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 40, .backend_fd = 3};
    gone->fds[1] = (fsmgr_client_fd_t){.in_use = 1, .backend_endpoint = 41, .backend_fd = 4};
    fsmgr_client_release(&table, 7);

    fsmgr_client_fd_t* a = fsmgr_client_next_orphan(&table);
    fsmgr_client_orphan_sent(a, 31);
    fsmgr_client_fd_t* b = fsmgr_client_next_orphan(&table);
    fsmgr_client_orphan_sent(b, 32);
    assert(fsmgr_client_orphans_in_flight(&table) == 2u);

    assert(fsmgr_client_orphan_closed(&table, 0, 0) == 0); /* a hangup: no request id */
    assert(fsmgr_client_orphan_closed(&table, 99, 0) == 0);
    assert(fsmgr_client_orphans_in_flight(&table) == 2u);

    assert(fsmgr_client_orphan_closed(&table, 31, 0) == 1);
    assert(fsmgr_client_orphans_in_flight(&table) == 1u);

    /* The other backend dies: fs-manager forgets its files, and the close it
     * never answered stops counting. */
    b->in_use = 0;
    b->closing = 0;
    assert(fsmgr_client_orphans_in_flight(&table) == 0u);
    assert(fsmgr_client_next_orphan(&table) == 0);
    assert(!gone->in_use);
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
        WASMOS_TEST_CASE(test_a_released_clients_open_files_are_handed_out_to_close),
        WASMOS_TEST_CASE(test_a_releasing_slot_is_nobodys_until_its_files_are_closed),
        WASMOS_TEST_CASE(test_a_releasing_slot_with_nothing_left_is_freed),
        WASMOS_TEST_CASE(test_an_orphan_survives_a_close_that_did_not_happen),
        WASMOS_TEST_CASE(test_only_a_matching_reply_completes_a_close),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_clients: ok\n");
    return 0;
}
