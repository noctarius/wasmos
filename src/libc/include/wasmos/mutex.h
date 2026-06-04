/* mutex.h - dual-target recursive mutex: hostcall path for WASM,
 * native syscall path for x86_64 native builds */
#ifndef WASMOS_LIBC_WASMOS_MUTEX_H
#define WASMOS_LIBC_WASMOS_MUTEX_H

#include <stdint.h>
#include <stddef.h>

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

static inline void
wasmos_mutex_init(wasmos_mutex_t *mutex)
{
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

static inline int32_t
wasmos_mutex_try_lock(wasmos_mutex_t *mutex)
{
    if (!mutex) {
        return -1;
    }
#if defined(__wasm__)
    return wasmos_mutex_try_lock_host((int32_t)(uintptr_t)mutex);
#elif defined(__x86_64__) && !defined(__wasm__)
    return (int32_t)wasmos_sys_mutex_try_lock((uint64_t)(uintptr_t)mutex);
#else
    return -1;
#endif
}

static inline int32_t
wasmos_mutex_lock(wasmos_mutex_t *mutex)
{
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

static inline int32_t
wasmos_mutex_unlock(wasmos_mutex_t *mutex)
{
    if (!mutex) {
        return -1;
    }
#if defined(__wasm__)
    return wasmos_mutex_unlock_host((int32_t)(uintptr_t)mutex);
#elif defined(__x86_64__) && !defined(__wasm__)
    return (int32_t)wasmos_sys_mutex_unlock((uint64_t)(uintptr_t)mutex);
#else
    return -1;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
