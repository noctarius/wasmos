#ifndef WASMOS_LIBC_WASMOS_THREAD_X86_64_H
#define WASMOS_LIBC_WASMOS_THREAD_X86_64_H

#include <stddef.h>
#include <stdint.h>

#include "wasmos/syscall_x86_64.h"

#if defined(__x86_64__) && !defined(__wasm__)

typedef void (*wasmos_thread_entry_fn_t)(void *arg);
typedef void (*wasmos_thread_continue_fn_t)(void *ctx, int32_t status);

typedef struct {
    wasmos_thread_entry_fn_t entry;
    void *arg;
} wasmos_thread_start_t;

static inline uintptr_t
wasmos_thread_align_down(uintptr_t v)
{
    return v & ~(uintptr_t)0xFULL;
}

static inline void
wasmos_thread_bootstrap(void)
{
    uintptr_t sp;
    wasmos_thread_start_t *start;

    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    start = (wasmos_thread_start_t *)sp;
    start->entry(start->arg);
    wasmos_sys_thread_exit(0);
}

/* Continuation-style spawn: caller provides stack storage and an optional
 * continuation callback that receives spawn status (tid or negative error). */
static inline int32_t
wasmos_thread_spawn_cont(void *stack_base,
                         size_t stack_size,
                         wasmos_thread_entry_fn_t entry,
                         void *arg,
                         wasmos_thread_continue_fn_t cont,
                         void *cont_ctx,
                         uint32_t *out_tid)
{
    uintptr_t top;
    wasmos_thread_start_t *start;
    int64_t rc;

    if (!stack_base || stack_size < sizeof(wasmos_thread_start_t) || !entry) {
        if (cont) {
            cont(cont_ctx, -1);
        }
        return -1;
    }

    top = wasmos_thread_align_down((uintptr_t)stack_base + stack_size);
    top -= sizeof(wasmos_thread_start_t);
    top = wasmos_thread_align_down(top);
    start = (wasmos_thread_start_t *)top;
    start->entry = entry;
    start->arg = arg;

    rc = wasmos_sys_thread_create((uint64_t)(uintptr_t)wasmos_thread_bootstrap, (uint64_t)top);
    if (rc > 0 && out_tid) {
        *out_tid = (uint32_t)rc;
    }

    if (cont) {
        cont(cont_ctx, (int32_t)rc);
    }

    return (int32_t)rc;
}

/* Continuation-style join: callback receives joined thread exit status or
 * negative error code. */
static inline int32_t
wasmos_thread_join_cont(uint32_t tid, wasmos_thread_continue_fn_t cont, void *cont_ctx)
{
    int32_t rc = (int32_t)wasmos_sys_thread_join(tid);
    if (cont) {
        cont(cont_ctx, rc);
    }
    return rc;
}

/* Continuation-style detach: callback receives detach status. */
static inline int32_t
wasmos_thread_detach_cont(uint32_t tid, wasmos_thread_continue_fn_t cont, void *cont_ctx)
{
    int32_t rc = (int32_t)wasmos_sys_thread_detach(tid);
    if (cont) {
        cont(cont_ctx, rc);
    }
    return rc;
}

#endif /* __x86_64__ && !__wasm__ */
#endif /* WASMOS_LIBC_WASMOS_THREAD_X86_64_H */
