/* mutex.h - dual-target recursive mutex: hostcall path for WASM,
 * native syscall path for x86_64 native builds */
#ifndef WASMOS_LIBC_WASMOS_MUTEX_H
#define WASMOS_LIBC_WASMOS_MUTEX_H

#include <stdint.h>
#include <stddef.h>

#include "wasmos_cast.h"
#include "wasmos/api.h"
#include "wasmos/syscall_x86_64.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t owner_tid;
    volatile uint32_t recursion_depth;
} wasmos_mutex_t;

#define WASMOS_MUTEX_INITIALIZER {0u, 0u}

static inline void wasmos_mutex_init(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

/* Returns 0 when the mutex is held by this thread (recursion depth raised), 1
 * when another thread owns it, and a negative code on error or on a build that
 * is neither WASM nor x86_64. */
static inline int32_t wasmos_mutex_try_lock(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return -1;
    }
#if defined(__wasm__)
    return wasmos_mutex_try_lock_host(addr_cast(int32_t, mutex));
#elif defined(__x86_64__) && !defined(__wasm__)
    return (int32_t)wasmos_sys_mutex_try_lock(addr_cast(uint64_t, mutex));
#else
    return -1;
#endif
}

/* Acquire, yielding the thread between attempts while another owner holds it.
 * Returns 0 once held, or the negative code that ended the retry loop. */
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
#if defined(__wasm__)
        (void)wasmos_thread_yield();
#elif defined(__x86_64__) && !defined(__wasm__)
        (void)wasmos_sys_thread_yield();
#else
        return -1;
#endif
    }
}

static inline int32_t wasmos_mutex_unlock(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return -1;
    }
#if defined(__wasm__)
    return wasmos_mutex_unlock_host(addr_cast(int32_t, mutex));
#elif defined(__x86_64__) && !defined(__wasm__)
    return (int32_t)wasmos_sys_mutex_unlock(addr_cast(uint64_t, mutex));
#else
    return -1;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
