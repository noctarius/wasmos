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

enum { USER_MUTEX_OK = 0, USER_MUTEX_BUSY = 1 };

/* Pure state transitions on an already-copied-in mutex word; the two entry
 * points below wrap them with the user-memory copy and the global lock. Both
 * return USER_MUTEX_OK / USER_MUTEX_BUSY / -1 and mutate *state in place.
 * Neither blocks and neither touches user memory itself. */

/* Acquire, or re-acquire recursively when this tid already owns it. Returns
 * USER_MUTEX_OK on acquisition, USER_MUTEX_BUSY when another tid holds it, or
 * -1 for a NULL state, a tid of 0 (reserved for "unlocked"), or a recursion
 * depth that would overflow -- refused rather than wrapped, since wrapping
 * would release a lock the owner still holds. */
static inline int user_mutex_state_try_lock(user_mutex_state_t* state, uint32_t tid) {
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

/* Release one level of ownership, clearing the owner at depth 1. Returns
 * USER_MUTEX_OK, or -1 for a NULL state, a tid of 0, an unlock by a non-owner,
 * or an unlock of an already-unlocked mutex. Never USER_MUTEX_BUSY. */
static inline int user_mutex_state_unlock(user_mutex_state_t* state, uint32_t tid) {
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

/*
 * Apply the corresponding transition to the mutex at `user_addr` in
 * `context_id`'s address space: copy the word in, transform it, copy it back.
 * The whole sequence runs under one kernel-global spinlock, so lockers on
 * different CPUs cannot interleave -- but the lock is global, not per mutex, so
 * unrelated mutexes contend.
 *
 * NON-BLOCKING by construction: there is no wait queue. A contended try-lock
 * reports USER_MUTEX_BUSY and it is the caller's job to retry or park (futex.h
 * is the primitive for actually sleeping on a word).
 *
 * Returns USER_MUTEX_OK, USER_MUTEX_BUSY, or -1 for context_id 0, user_addr 0,
 * a user_addr not 4-byte aligned, a failed copy in either direction, or a state
 * the transition rejects. `out_state` is optional and, when given, receives the
 * state as it stands after the attempt -- including on failure, where it may
 * hold zeros if the copy-in never happened.
 */
int user_mutex_user_try_lock(uint32_t context_id, uint64_t user_addr, uint32_t tid,
                             user_mutex_state_t* out_state);
int user_mutex_user_unlock(uint32_t context_id, uint64_t user_addr, uint32_t tid,
                           user_mutex_state_t* out_state);

#endif
