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

/* Mutex state, living in the caller's own memory: the kernel reads and writes it
 * through the lock/unlock entry points under a global lock, so the words must
 * never be modified directly by user code. owner_tid is 0 when the mutex is
 * free, otherwise the TID holding it; recursion_depth counts the acquisitions
 * that owner still has to undo. The object must be 4-byte aligned (a misaligned
 * address is refused) and is not position-dependent: any thread of the same
 * process may lock it. */
typedef struct {
    volatile uint32_t owner_tid;
    volatile uint32_t recursion_depth;
} wasmos_mutex_t;

/* Static initializer for an unlocked mutex; equivalent to wasmos_mutex_init. */
#define WASMOS_MUTEX_INITIALIZER {0u, 0u}

/* Reset a mutex to unlocked. Only valid before first use or once the owner has
 * released it — resetting a held mutex discards the owner's claim without
 * telling it. A NULL `mutex` is a no-op. */
static inline void wasmos_mutex_init(wasmos_mutex_t* mutex) {
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

/* Returns 0 when the mutex is held by this thread (recursion depth raised), 1
 * when another thread owns it, and a negative code on error or on a build that
 * is neither WASM nor x86_64.
 * Never blocks: a contended lock reports 1 immediately and the caller decides
 * whether to retry. Recursive acquisition by the owning thread always succeeds
 * until the depth counter would overflow, which is reported as an error. */
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
 * Returns 0 once held, or the negative code that ended the retry loop.
 * Unbounded: it spins-with-yield until it wins or the try-lock errors, so a
 * caller that must not stall should use wasmos_mutex_try_lock directly. */
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

/* Drop one acquisition, freeing the mutex when the recursion depth reaches
 * zero. Returns 0 on success and a negative code when the caller is not the
 * owner, the mutex is already free, the pointer is NULL or misaligned, or the
 * build is neither WASM nor x86_64. */
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
