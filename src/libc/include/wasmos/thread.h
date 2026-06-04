/* thread.h - WASM runtime thread wrappers layered over wasmos hostcalls */
#ifndef WASMOS_LIBC_WASMOS_THREAD_H
#define WASMOS_LIBC_WASMOS_THREAD_H

#include <stdint.h>
#include "wasmos/api.h"

/* Thin wrappers that alias the raw API names to more descriptive identifiers. */

static inline int32_t
wasmos_thread_current_tid(void)
{
    return wasmos_thread_gettid();
}

static inline int32_t
wasmos_thread_spawn(int32_t entry_token, int32_t arg0, int32_t arg1, int32_t flags)
{
    return wasmos_thread_create(entry_token, arg0, arg1, flags);
}

static inline int32_t
wasmos_thread_join_wait(int32_t tid)
{
    return wasmos_thread_join(tid);
}

static inline int32_t
wasmos_thread_detach_self(int32_t tid)
{
    return wasmos_thread_detach(tid);
}

static inline int32_t
wasmos_thread_cooperate(void)
{
    return wasmos_thread_yield();
}

static inline int32_t
wasmos_thread_finish(int32_t status)
{
    return wasmos_thread_exit(status);
}

#endif
