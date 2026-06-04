/* user_mutex.h - Kernel-backed recursive mutex for ring-3 processes.
 *
 * The mutex state lives in user memory (user_mutex_state_t is written through
 * mm_copy_to_user).  The kernel validates ownership and performs the lock/unlock
 * atomically from the syscall path, avoiding the need for a futex-style sleep queue
 * in the simple single-waiter case.  Recursion is allowed within the same tid. */
#ifndef WASMOS_KERNEL_USER_MUTEX_H
#define WASMOS_KERNEL_USER_MUTEX_H

#include <stdint.h>

/* Stored in ring-3 address space at the mutex object address.
 * owner_tid == 0 means unlocked; recursion_depth counts re-entrant acquires. */
typedef struct {
    uint32_t owner_tid;
    uint32_t recursion_depth;
} user_mutex_state_t;

enum {
    USER_MUTEX_OK = 0,
    USER_MUTEX_BUSY = 1
};

static inline int
user_mutex_state_try_lock(user_mutex_state_t *state, uint32_t tid)
{
    if (!state || tid == 0u) {
        return -1;
    }
    if (state->owner_tid == 0u) {
        state->owner_tid = tid;
        state->recursion_depth = 1u;
        return USER_MUTEX_OK;
    }
    if (state->owner_tid == tid) {
        if (state->recursion_depth == 0xFFFFFFFFu) {
            return -1;
        }
        state->recursion_depth++;
        return USER_MUTEX_OK;
    }
    return USER_MUTEX_BUSY;
}

static inline int
user_mutex_state_unlock(user_mutex_state_t *state, uint32_t tid)
{
    if (!state || tid == 0u) {
        return -1;
    }
    if (state->owner_tid != tid || state->recursion_depth == 0u) {
        return -1;
    }
    if (state->recursion_depth > 1u) {
        state->recursion_depth--;
        return USER_MUTEX_OK;
    }
    state->owner_tid = 0u;
    state->recursion_depth = 0u;
    return USER_MUTEX_OK;
}

int user_mutex_user_try_lock(uint32_t context_id,
                             uint64_t user_addr,
                             uint32_t tid,
                             user_mutex_state_t *out_state);
int user_mutex_user_unlock(uint32_t context_id,
                           uint64_t user_addr,
                           uint32_t tid,
                           user_mutex_state_t *out_state);

#endif
