#include <assert.h>
#include <stdint.h>

#include "user_mutex.h"

static void
test_basic_lock_unlock(void)
{
    user_mutex_state_t state = {0};
    assert(user_mutex_state_try_lock(&state, 7u) == USER_MUTEX_OK);
    assert(state.owner_tid == 7u);
    assert(state.recursion_depth == 1u);

    assert(user_mutex_state_unlock(&state, 7u) == USER_MUTEX_OK);
    assert(state.owner_tid == 0u);
    assert(state.recursion_depth == 0u);
}

static void
test_recursive_locking(void)
{
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

static void
test_contention_and_non_owner_unlock(void)
{
    user_mutex_state_t state = {0};
    assert(user_mutex_state_try_lock(&state, 3u) == USER_MUTEX_OK);
    assert(user_mutex_state_try_lock(&state, 9u) == USER_MUTEX_BUSY);
    assert(state.owner_tid == 3u);
    assert(state.recursion_depth == 1u);

    assert(user_mutex_state_unlock(&state, 9u) < 0);
    assert(state.owner_tid == 3u);
    assert(state.recursion_depth == 1u);
}

int
main(void)
{
    test_basic_lock_unlock();
    test_recursive_locking();
    test_contention_and_non_owner_unlock();
    return 0;
}
