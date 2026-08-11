/* thread.h - WASM runtime thread wrappers layered over wasmos hostcalls */
#ifndef WASMOS_LIBC_WASMOS_THREAD_H
#define WASMOS_LIBC_WASMOS_THREAD_H

#include <stdint.h>
#include "wasmos/api.h"

/* Thin wrappers that alias the raw API names to more descriptive identifiers. */

static inline int32_t wasmos_thread_current_tid(void) {
    return wasmos_thread_gettid();
}

static inline int32_t wasmos_thread_cooperate(void) {
    return wasmos_thread_yield();
}

#endif
