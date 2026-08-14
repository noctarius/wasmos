/* test_user_mutex.c — host tests for the recursive user-space mutex state machine.
 *
 * user_mutex_state_try_lock and user_mutex_state_unlock are static inlines in
 * user_mutex.h, so the code under test is the real one and nothing is stubbed: no
 * kernel object is linked and the state word lives in a plain host-local
 * user_mutex_state_t rather than in ring-3 memory.
 *
 * What that leaves uncovered is everything wrapped around them in
 * src/kernel/user_mutex.c: the context/address validation, the 4-byte alignment
 * check, the global spinlock, and the two mm_copy_{from,to}_user round trips that
 * carry the state to and from the guest. There is no blocking behaviour to model
 * here -- the primitive is non-blocking by construction, a contended try-lock
 * reports USER_MUTEX_BUSY and it is the caller's job to retry or park on a futex.
 *
 * Return convention of both helpers: USER_MUTEX_OK (0) on success,
 * USER_MUTEX_BUSY (1) when another tid holds the lock, and -1 for a NULL state, a
 * tid of 0, an unlock by a non-owner or of an unheld lock, and recursion-depth
 * overflow. Both mutate *state in place on the paths that succeed.
 */

#include <assert.h>
#include <stdint.h>

#include "test_shuffle.h"

#include "user_mutex.h"

static void test_basic_lock_unlock(void) {
    user_mutex_state_t state = {0};
    assert(user_mutex_state_try_lock(&state, 7u) == USER_MUTEX_OK);
    assert(state.owner_tid == 7u);
    assert(state.recursion_depth == 1u);

    assert(user_mutex_state_unlock(&state, 7u) == USER_MUTEX_OK);
    assert(state.owner_tid == 0u);
    assert(state.recursion_depth == 0u);
}

static void test_recursive_locking(void) {
    user_mutex_state_t state = {0};
    assert(user_mutex_state_try_lock(&state, 11u) == USER_MUTEX_OK);
    assert(user_mutex_state_try_lock(&state, 11u) == USER_MUTEX_OK);
    assert(state.owner_tid == 11u);
    assert(state.recursion_depth == 2u);

    assert(user_mutex_state_unlock(&state, 11u) == USER_MUTEX_OK);
    assert(state.owner_tid == 11u);
    assert(state.recursion_depth == 1u);

    assert(user_mutex_state_unlock(&state, 11u) == USER_MUTEX_OK);
    assert(state.owner_tid == 0u);
    assert(state.recursion_depth == 0u);
}

static void test_contention_and_non_owner_unlock(void) {
    user_mutex_state_t state = {0};
    assert(user_mutex_state_try_lock(&state, 3u) == USER_MUTEX_OK);
    assert(user_mutex_state_try_lock(&state, 9u) == USER_MUTEX_BUSY);
    assert(state.owner_tid == 3u);
    assert(state.recursion_depth == 1u);

    assert(user_mutex_state_unlock(&state, 9u) < 0);
    assert(state.owner_tid == 3u);
    assert(state.recursion_depth == 1u);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_basic_lock_unlock),
        WASMOS_TEST_CASE(test_recursive_locking),
        WASMOS_TEST_CASE(test_contention_and_non_owner_unlock),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
