/* thread.h - WASM runtime thread wrappers layered over wasmos hostcalls */
#ifndef WASMOS_LIBC_WASMOS_THREAD_H
#define WASMOS_LIBC_WASMOS_THREAD_H

#include <stdint.h>
#include "wasmos/api.h"

/* Thin wrappers that alias the raw API names to more descriptive identifiers.
 * Both forward directly to the host call of the same meaning and add nothing:
 * the TID is process-local and non-zero for a live thread, and the yield returns
 * 0 once the thread is scheduled again. Meant for WASM guests: in a native
 * build the underlying host calls are ordinary externs the component would have
 * to supply itself, and native code uses the int-0x80 wrappers in
 * wasmos/syscall_x86_64.h instead. */

static inline int32_t wasmos_thread_current_tid(void) {
    return wasmos_thread_gettid();
}

static inline int32_t wasmos_thread_cooperate(void) {
    return wasmos_thread_yield();
}

#endif
