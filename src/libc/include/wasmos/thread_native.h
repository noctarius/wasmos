/* thread_native.h - native thread spawn/join helpers on the ring-3 syscall
 * ABI (wasmos/syscall_native.h). Provides the stack layout and entry trampoline
 * for user-space threads created via wasmos_thread_spawn_cont in ring-3 native
 * binaries. */
#ifndef WASMOS_LIBC_WASMOS_THREAD_NATIVE_H
#define WASMOS_LIBC_WASMOS_THREAD_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#include "wasmos_cast.h"
#include "wasmos/syscall_native.h"

#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(__wasm__)

/* Thread body. Returning from it exits the thread with status 0. */
typedef void (*wasmos_thread_entry_fn_t)(void* arg);
/* Completion callback for the *_cont helpers. `status` is the operation's own
 * outcome (a TID for spawn, 0 or a negative packed code for join/detach), never
 * the joined thread's exit status. Invoked synchronously on the calling thread
 * before the helper returns, including on the failure paths. */
typedef void (*wasmos_thread_continue_fn_t)(void* ctx, int32_t status);

/* Hand-off record the spawning thread writes at the top of the new thread's
 * stack; wasmos_thread_bootstrap reads it back from the stack pointer. It lives
 * in the child's stack, so the stack must stay valid for the thread's whole
 * life. */
typedef struct {
    wasmos_thread_entry_fn_t entry;
    void* arg;
} wasmos_thread_start_t;

/* Round down to the 16-byte stack alignment both the SysV x86_64 ABI and AAPCS64
 * require at a call boundary. */
static inline uintptr_t wasmos_thread_align_down(uintptr_t v) {
    return v & ~(uintptr_t)0xFULL;
}

/* Runs a spawned thread's body from its hand-off record and exits the thread
 * with status 0 when the body returns. Reached only from
 * wasmos_thread_bootstrap. */
__attribute__((used, noreturn)) static void wasmos_thread_run(wasmos_thread_start_t* start) {
    start->entry(start->arg);
    for (;;) {
        wasmos_sys_thread_exit(0);
    }
}

/* Entry trampoline for a spawned thread, entered by the kernel with the stack
 * pointer at the wasmos_thread_start_t that wasmos_thread_spawn_cont wrote.
 * Naked, because the record is found through the INCOMING stack pointer: an
 * ordinary function's prologue moves it first (a push on x86_64, the X30 save on
 * aarch64), and the record read back would be the saved register instead. The
 * pointer is handed to wasmos_thread_run as its argument; the call keeps the
 * ABI's stack alignment, because the record sits at a 16-byte boundary. */
__attribute__((naked, used)) static void wasmos_thread_bootstrap(void) {
#if defined(__x86_64__)
    __asm__ volatile("mov %rsp, %rdi\n"
                     "call wasmos_thread_run\n"
                     "ud2\n");
#else
    __asm__ volatile("mov x0, sp\n"
                     "bl wasmos_thread_run\n"
                     "brk #0\n");
#endif
}

/* Continuation-style spawn: caller provides stack storage and an optional
 * continuation callback that receives spawn status (tid or negative error).
 *
 * `stack_base`/`stack_size` name memory the CALLER owns and must keep alive
 * until the thread has exited and been joined; the hand-off record is written
 * into its top and the new thread runs on the rest. `arg` is passed to `entry`
 * untouched. On success returns the new TID (> 0) and stores it through
 * `out_tid` when that is non-NULL; returns -1 without spawning for a NULL stack
 * or entry or a stack too small to hold the hand-off record, and a negative
 * kernel status if the thread could not be created. `cont` is called with the
 * same value in every case. Returns as soon as the thread exists; it does not
 * wait for the thread to run. */
static inline int32_t wasmos_thread_spawn_cont(void* stack_base, size_t stack_size,
                                               wasmos_thread_entry_fn_t entry, void* arg,
                                               wasmos_thread_continue_fn_t cont, void* cont_ctx,
                                               uint32_t* out_tid) {
    uintptr_t top;
    wasmos_thread_start_t* start;
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
    start = (wasmos_thread_start_t*)top;
    start->entry = entry;
    start->arg = arg;

    /* The new thread starts in wasmos_thread_bootstrap with its stack pointer at
     * `top`, where the wasmos_thread_start_t it reads back was just written. */
    rc = wasmos_sys_thread_create(addr_cast(uint64_t, wasmos_thread_bootstrap), (uint64_t)top);
    if (rc > 0 && out_tid) {
        *out_tid = (uint32_t)rc;
    }

    if (cont) {
        cont(cont_ctx, (int32_t)rc);
    }

    return (int32_t)rc;
}

/* Continuation-style join: the callback receives the OUTCOME (0 or a negative
 * packed code), and the joined thread's exit status is stored at `out_status`
 * when the join succeeded. The two are separate values: sharing one makes a
 * thread that exits negative indistinguishable from a failed join. `out_status`
 * may be null if the caller only cares whether the join worked.
 * BLOCKS in the kernel until the target thread exits. Returns 0 on a successful
 * join, or the negative packed code (self-join, unknown or detached tid, …);
 * *out_status is written only on success. */
static inline int32_t wasmos_thread_join_cont(uint32_t tid, wasmos_thread_continue_fn_t cont,
                                              void* cont_ctx, int32_t* out_status) {
    wasmos_thread_join_result_t res = wasmos_sys_thread_join(tid);
    if (out_status && res.status == 0) {
        *out_status = res.exit_status;
    }
    if (cont) {
        cont(cont_ctx, (int32_t)res.status);
    }
    return (int32_t)res.status;
}

/* Continuation-style detach: callback receives detach status. Marks `tid` as
 * self-reaping, so it can no longer be joined. Returns 0 on success and -1 on
 * any rejection (unknown tid, already detached, or a caller with no process).
 * Returns immediately. */
static inline int32_t wasmos_thread_detach_cont(uint32_t tid, wasmos_thread_continue_fn_t cont,
                                                void* cont_ctx) {
    int32_t rc = (int32_t)wasmos_sys_thread_detach(tid);
    if (cont) {
        cont(cont_ctx, rc);
    }
    return rc;
}

#endif /* (__x86_64__ || __aarch64__) && !__wasm__ */
#endif /* WASMOS_LIBC_WASMOS_THREAD_NATIVE_H */
