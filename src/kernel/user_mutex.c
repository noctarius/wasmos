/* user_mutex.c - Kernel-backed recursive user-space mutex.
 *
 * NON-BLOCKING by construction: there is no wait queue and no sleep here.  The
 * state word lives in user memory and every operation is a read-modify-write
 * performed by the kernel under one global spinlock (g_user_mutex_lock), so
 * concurrent lockers on different CPUs cannot interleave.  A contended
 * try-lock reports USER_MUTEX_BUSY and it is the caller's job to retry or
 * park; futex.c is the primitive for actually sleeping on a word.
 *
 * Return contract of both entry points: >= 0 is the op's own result
 * (USER_MUTEX_OK / USER_MUTEX_BUSY), -1 for a bad context/address, a
 * misaligned user_addr, a failed user copy in either direction, or a state the
 * op rejects (unlock by a non-owner, recursion-depth overflow). */
#include "user_mutex.h"

#include "memory.h"
#include "sync/spinlock.h"

static ksync_spinlock_t g_user_mutex_lock;

static int user_mutex_access(uint32_t context_id, uint64_t user_addr, uint32_t tid,
                             int (*op)(user_mutex_state_t* state, uint32_t tid),
                             user_mutex_state_t* out_state) {
    user_mutex_state_t state = {0};
    int rc = -1;

    if (context_id == 0u || user_addr == 0u || !op) {
        return -1;
    }
    if ((user_addr & (uint64_t)(sizeof(uint32_t) - 1u)) != 0u) {
        return -1;
    }

    ksync_spinlock_lock(&g_user_mutex_lock);
    if (mm_copy_from_user(context_id, &state, user_addr, (uint64_t)sizeof(state)) == 0) {
        rc = op(&state, tid);
        if (rc >= 0 &&
            mm_copy_to_user(context_id, user_addr, &state, (uint64_t)sizeof(state)) != 0) {
            rc = -1;
        }
    }
    ksync_spinlock_unlock(&g_user_mutex_lock);

    if (out_state) {
        *out_state = state;
    }
    return rc;
}

int user_mutex_user_try_lock(uint32_t context_id, uint64_t user_addr, uint32_t tid,
                             user_mutex_state_t* out_state) {
    return user_mutex_access(context_id, user_addr, tid, user_mutex_state_try_lock, out_state);
}

int user_mutex_user_unlock(uint32_t context_id, uint64_t user_addr, uint32_t tid,
                           user_mutex_state_t* out_state) {
    return user_mutex_access(context_id, user_addr, tid, user_mutex_state_unlock, out_state);
}
