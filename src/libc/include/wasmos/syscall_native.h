/* syscall_native.h - native ring-3 syscall ABI: syscall numbers, result
 * records and the per-call wrappers. For non-WASM userland code paths; it stays
 * separate from the WASM host imports in wasmos/api.h. The wrappers are built
 * on three trap stubs each architecture provides (wasmos_syscall0,
 * wasmos_syscall1, wasmos_syscall6_ret2) and on wasmos_native_read_sp():
 * wasmos/syscall_x86_64.h (int 0x80) and wasmos/syscall_aarch64.h (svc #0). */
#ifndef WASMOS_LIBC_WASMOS_SYSCALL_NATIVE_H
#define WASMOS_LIBC_WASMOS_SYSCALL_NATIVE_H

#include <stdint.h>

#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(__wasm__)

/* Syscall numbers (RAX on x86_64, x8 on aarch64). The values are the ABI
 * between ring-3 native binaries and the kernel's syscall dispatcher: they are append-only, and a
 * number outside this set returns -1. */
typedef enum {
    WASMOS_SYSCALL_NOP = 0,
    WASMOS_SYSCALL_GETPID = 1,
    WASMOS_SYSCALL_EXIT = 2,
    WASMOS_SYSCALL_YIELD = 3,
    WASMOS_SYSCALL_WAIT = 4,
    WASMOS_SYSCALL_IPC_NOTIFY = 5,
    WASMOS_SYSCALL_IPC_CALL = 6,
    WASMOS_SYSCALL_GETTID = 7,
    WASMOS_SYSCALL_THREAD_YIELD = 8,
    WASMOS_SYSCALL_THREAD_EXIT = 9,
    WASMOS_SYSCALL_THREAD_CREATE = 10,
    WASMOS_SYSCALL_THREAD_JOIN = 11,
    WASMOS_SYSCALL_THREAD_DETACH = 12,
    WASMOS_SYSCALL_NOTIFY_READY = 13,
    WASMOS_SYSCALL_MUTEX_TRY_LOCK = 14,
    WASMOS_SYSCALL_MUTEX_UNLOCK = 15
} wasmos_syscall_id_t;

/* Raw two-register return of a syscall that reports a secondary value: `r0`
 * (RAX on x86_64, x0 on aarch64) carries the outcome and `r1` (RDX / x1) the
 * extra word. The kernel zeroes r1 on every error path, so a stale register is
 * never mistaken for a payload. */
typedef struct {
    int64_t r0;
    int64_t r1;
} wasmos_sysret2_t;

/* Decoded IPC_CALL result: `status` is 0 or a negative IPC error, and
 * `reply_arg0` is the reply's first payload word, valid only when status == 0. */
typedef struct {
    int64_t status;
    uint32_t reply_arg0;
} wasmos_ipc_call_result_t;

typedef struct {
    int64_t status;      /* 0, or a negative packed WASMOS_ERR_* code */
    int32_t exit_status; /* the joined thread's status; valid only when status == 0 */
} wasmos_thread_join_result_t;

#if defined(__x86_64__)
#include "wasmos/syscall_x86_64.h"
#else
#include "wasmos/syscall_aarch64.h"
#endif

/* The thin per-call wrappers. Unless a comment below says otherwise they return
 * the kernel's first result register verbatim: 0 or the queried value on success, -1 or a negative
 * packed code on failure. nop/notify_ready/yield/thread_yield always return 0.
 * getpid/gettid return the caller's own ids and cannot fail. */
static inline int64_t wasmos_sys_nop(void) {
    return wasmos_syscall0(WASMOS_SYSCALL_NOP);
}
static inline int64_t wasmos_sys_getpid(void) {
    return wasmos_syscall0(WASMOS_SYSCALL_GETPID);
}
static inline int64_t wasmos_sys_gettid(void) {
    return wasmos_syscall0(WASMOS_SYSCALL_GETTID);
}
static inline int64_t wasmos_sys_yield(void) {
    return wasmos_syscall0(WASMOS_SYSCALL_YIELD);
}
static inline int64_t wasmos_sys_thread_yield(void) {
    return wasmos_syscall0(WASMOS_SYSCALL_THREAD_YIELD);
}
/* Create a thread that starts at `entry_rip` with its stack pointer set to
 * `user_stack_top`
 * (the caller owns that stack and must keep it alive). Returns the new TID
 * (> 0), or -1 if the thread could not be created. Does not wait for it to
 * run. */
static inline int64_t wasmos_sys_thread_create(uint64_t entry_rip, uint64_t user_stack_top) {
    return wasmos_syscall6_ret2(WASMOS_SYSCALL_THREAD_CREATE, entry_rip, user_stack_top, 0, 0, 0, 0)
        .r0;
}
/* Join returns two things, so it uses the two-register form the IPC call
 * already established: r0 carries the outcome and r1 the joined thread's exit
 * status. The status is chosen by the guest and may be any 32-bit value,
 * negative ones included, so it cannot share a register with the error codes --
 * a thread exiting -1 would be indistinguishable from a failed join.
 * BLOCKS until the target thread exits. `.status` is 0 on success or a negative
 * packed code (invalid argument, self-join, detached or unknown tid), and
 * `.exit_status` is meaningful only when `.status` is 0. */
static inline wasmos_thread_join_result_t wasmos_sys_thread_join(uint32_t tid) {
    wasmos_sysret2_t raw = wasmos_syscall6_ret2(WASMOS_SYSCALL_THREAD_JOIN, tid, 0, 0, 0, 0, 0);
    wasmos_thread_join_result_t out;
    out.status = raw.r0;
    out.exit_status = (int32_t)raw.r1;
    return out;
}
/* Mark `tid` self-reaping so it can no longer be joined. Returns 0, or -1 for an
 * unknown or already-detached thread. */
static inline int64_t wasmos_sys_thread_detach(uint32_t tid) {
    return wasmos_syscall1(WASMOS_SYSCALL_THREAD_DETACH, tid);
}
/* Kernel-arbitrated recursive lock on the wasmos_mutex_t at `mutex_addr` (which
 * must be 4-byte aligned and in the caller's own address space). try_lock
 * returns 0 when the mutex is now held by this thread, 1 when another thread
 * owns it (never blocks), -1 on a bad address or a depth overflow; unlock
 * returns 0 or -1 when the caller is not the owner. */
static inline int64_t wasmos_sys_mutex_try_lock(uint64_t mutex_addr) {
    return wasmos_syscall1(WASMOS_SYSCALL_MUTEX_TRY_LOCK, mutex_addr);
}
static inline int64_t wasmos_sys_mutex_unlock(uint64_t mutex_addr) {
    return wasmos_syscall1(WASMOS_SYSCALL_MUTEX_UNLOCK, mutex_addr);
}
/* BLOCK until child process `pid` exits, then return ITS exit status, or -1 if
 * the wait itself failed (no such child, or the caller has no process). A child
 * that exits -1 is therefore indistinguishable from a failed wait. */
static inline int64_t wasmos_sys_wait(uint32_t pid) {
    return wasmos_syscall1(WASMOS_SYSCALL_WAIT, pid);
}
/* Announce that initialization finished, waking anything waiting for this
 * process to become ready. Returns 0, including when the caller has no process
 * to mark. */
static inline int64_t wasmos_sys_notify_ready(void) {
    return wasmos_syscall0(WASMOS_SYSCALL_NOTIFY_READY);
}
/* Send a contentless notification to `endpoint`. Returns 0 (IPC_OK) or a
 * negative IPC error (no such endpoint, permission denied, queue full). Does not
 * block. */
static inline int64_t wasmos_sys_ipc_notify(uint32_t endpoint) {
    return wasmos_syscall1(WASMOS_SYSCALL_IPC_NOTIFY, endpoint);
}

/* Synchronous request/reply: the kernel assigns the request id, sends the
 * message to `endpoint` from an implicit per-process reply endpoint, and BLOCKS
 * until the authentic reply to that id arrives; replies with other ids are held
 * for later and forged sources are dropped. r0 is 0 or a negative IPC error,
 * r1 the reply's arg0 (zeroed on every error path). */
static inline wasmos_sysret2_t wasmos_sys_ipc_call(uint32_t endpoint, uint32_t type, uint32_t arg0,
                                                   uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    return wasmos_syscall6_ret2(WASMOS_SYSCALL_IPC_CALL, endpoint, type, arg0, arg1, arg2, arg3);
}

/* IPC_CALL contract:
 * - status == 0: reply_arg0 is valid
 * - status < 0: reply_arg0 is undefined by caller contract (kernel currently
 *   zeroes r1 on error to avoid stale register reuse) */
static inline wasmos_ipc_call_result_t wasmos_sys_ipc_call_result(uint32_t endpoint, uint32_t type,
                                                                  uint32_t arg0, uint32_t arg1,
                                                                  uint32_t arg2, uint32_t arg3) {
    wasmos_sysret2_t raw = wasmos_sys_ipc_call(endpoint, type, arg0, arg1, arg2, arg3);
    wasmos_ipc_call_result_t out;
    out.status = raw.r0;
    out.reply_arg0 = (uint32_t)raw.r1;
    return out;
}

/* Does not return when the syscall path succeeds. This is userland (ring 3 /
 * EL0): it cannot kpanic (a kernel symbol) or halt (privileged). If the exit
 * syscall ever fails to take effect, just keep re-issuing it so the thread/
 * process never falls through into caller code. */
static inline void wasmos_sys_exit(int32_t status) {
    for (;;) {
        (void)wasmos_syscall1(WASMOS_SYSCALL_EXIT, (uint32_t)status);
    }
}

static inline void wasmos_sys_thread_exit(int32_t status) {
    for (;;) {
        (void)wasmos_syscall1(WASMOS_SYSCALL_THREAD_EXIT, (uint32_t)status);
    }
}

#endif /* (__x86_64__ || __aarch64__) && !__wasm__ */
#endif /* WASMOS_LIBC_WASMOS_SYSCALL_NATIVE_H */
