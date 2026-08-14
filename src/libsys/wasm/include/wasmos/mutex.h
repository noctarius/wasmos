/* mutex.h - WASM recursive mutex for libsys (WASM target).
 * The kernel owns the locking logic behind the mutex_try_lock / mutex_unlock
 * imports (declared in wasmos/api.h); this header only holds the shared state
 * struct and the yield-retry loop that turns try_lock into a blocking lock.
 * try_lock/lock return 0 when acquired, <0 on error (unlock also reports <0 for
 * a non-owner); lock never returns 1, the contended status it retries on.
 * Keep in sync with the identical definitions in src/libc/include/wasmos/mutex.h,
 * which add the native x86_64 syscall path. */
#ifndef WASMOS_LIBSYS_WASMOS_MUTEX_H
#define WASMOS_LIBSYS_WASMOS_MUTEX_H

#include <stdint.h>
#include <stddef.h>

#include "wasmos_cast.h"
#include "wasmos/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared state read and written by the kernel behind the mutex hostcalls: the
 * owning thread id (0 when unlocked) and how many times that owner has taken
 * it. The guest only ever zeroes it; every transition is the kernel's. */
typedef struct {
    volatile uint32_t owner_tid;
    volatile uint32_t recursion_depth;
} wasmos_mutex_t;

/* Static-initialiser form of wasmos_mutex_init(): an unlocked mutex. */
#define WASMOS_MUTEX_INITIALIZER {0u, 0u}

/* Reset a mutex to unlocked. Only valid before first use or when the caller
 * knows nobody holds it; it does not release a held lock, it forgets it.
 * NULL is ignored. */
static inline void wasmos_mutex_init(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

/* Acquire without waiting: returns 0 when the lock is held by this thread
 * (including a recursive re-entry), 1 when another thread holds it, and -1 for
 * a NULL mutex or a rejected hostcall. */
static inline int32_t wasmos_mutex_try_lock(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return -1;
    }
    return wasmos_mutex_try_lock_host(addr_cast(int32_t, mutex));
}

/* Acquire, yielding the thread between attempts until it succeeds. Returns 0
 * once held, or -1 for a NULL mutex or a rejected hostcall; it never returns 1.
 * Contention is a yield-spin, not a sleep, so a lock held across a long
 * operation costs scheduler slots. */
static inline int32_t wasmos_mutex_lock(wasmos_mutex_t* mutex) {
    int32_t rc = -1;
    if (!mutex) {
        return -1;
    }
    /* TODO(user-mutex-futex): add a sleep/wake path so contended user mutexes
     * stop yield-spinning once the kernel grows a futex-style primitive. */
    for (;;) {
        rc = wasmos_mutex_try_lock(mutex);
        if (rc != 1) {
            return rc;
        }
        (void)wasmos_thread_yield();
    }
}

/* Release one level of ownership; the mutex only becomes free when the
 * recursion depth reaches zero. Returns 0 on success, or <0 for a NULL mutex or
 * a caller that is not the owner. */
static inline int32_t wasmos_mutex_unlock(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return -1;
    }
    return wasmos_mutex_unlock_host(addr_cast(int32_t, mutex));
}

#ifdef __cplusplus
}
#endif

#endif
