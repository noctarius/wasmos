#ifndef WASMOS_LIBC_WASMOS_SYSCALL_X86_64_H
#define WASMOS_LIBC_WASMOS_SYSCALL_X86_64_H

#include <stdint.h>

/* Native ring3 syscall ABI for x86_64 (int 0x80).
 * This header is for non-WASM userland code paths and stays separate from
 * WASM host imports in wasmos/api.h. */
#if defined(__x86_64__) && !defined(__wasm__)

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
    WASMOS_SYSCALL_THREAD_JOIN = 11
} wasmos_syscall_id_t;

typedef struct {
    int64_t rax;
    int64_t rdx;
} wasmos_sysret2_t;

typedef struct {
    int64_t status;
    uint32_t reply_arg0;
} wasmos_ipc_call_result_t;

static inline int64_t
wasmos_syscall0(uint64_t id)
{
    uint64_t rax = id;
    __asm__ volatile("int $0x80" : "+a"(rax) : : "memory");
    return (int64_t)rax;
}

static inline int64_t
wasmos_syscall1(uint64_t id, uint64_t arg0)
{
    uint64_t rax = id;
    uint64_t rdi = arg0;
    __asm__ volatile("int $0x80" : "+a"(rax) : "D"(rdi) : "memory");
    return (int64_t)rax;
}

static inline wasmos_sysret2_t
wasmos_syscall6_ret2(uint64_t id,
                     uint64_t arg0,
                     uint64_t arg1,
                     uint64_t arg2,
                     uint64_t arg3,
                     uint64_t arg4,
                     uint64_t arg5)
{
    wasmos_sysret2_t out;
    uint64_t rax = id;
    uint64_t rdi = arg0;
    uint64_t rsi = arg1;
    uint64_t rdx = arg2;
    uint64_t rcx = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile("int $0x80"
                     : "+a"(rax), "+d"(rdx)
                     : "D"(rdi), "S"(rsi), "c"(rcx), "r"(r8), "r"(r9)
                     : "memory");
    out.rax = (int64_t)rax;
    out.rdx = (int64_t)rdx;
    return out;
}

static inline int64_t wasmos_sys_nop(void) { return wasmos_syscall0(WASMOS_SYSCALL_NOP); }
static inline int64_t wasmos_sys_getpid(void) { return wasmos_syscall0(WASMOS_SYSCALL_GETPID); }
static inline int64_t wasmos_sys_gettid(void) { return wasmos_syscall0(WASMOS_SYSCALL_GETTID); }
static inline int64_t wasmos_sys_yield(void) { return wasmos_syscall0(WASMOS_SYSCALL_YIELD); }
static inline int64_t wasmos_sys_thread_yield(void) { return wasmos_syscall0(WASMOS_SYSCALL_THREAD_YIELD); }
static inline int64_t wasmos_sys_thread_create(uint64_t entry_rip, uint64_t user_stack_top)
{
    return wasmos_syscall6_ret2(WASMOS_SYSCALL_THREAD_CREATE, entry_rip, user_stack_top, 0, 0, 0, 0).rax;
}
static inline int64_t wasmos_sys_thread_join(uint32_t tid)
{
    return wasmos_syscall1(WASMOS_SYSCALL_THREAD_JOIN, tid);
}
static inline int64_t wasmos_sys_wait(uint32_t pid) { return wasmos_syscall1(WASMOS_SYSCALL_WAIT, pid); }
static inline int64_t wasmos_sys_ipc_notify(uint32_t endpoint)
{
    return wasmos_syscall1(WASMOS_SYSCALL_IPC_NOTIFY, endpoint);
}

static inline wasmos_sysret2_t
wasmos_sys_ipc_call(uint32_t endpoint,
                    uint32_t type,
                    uint32_t arg0,
                    uint32_t arg1,
                    uint32_t arg2,
                    uint32_t arg3)
{
    return wasmos_syscall6_ret2(WASMOS_SYSCALL_IPC_CALL, endpoint, type, arg0, arg1, arg2, arg3);
}

/* IPC_CALL contract:
 * - status == 0: reply_arg0 is valid
 * - status < 0: reply_arg0 is undefined by caller contract (kernel currently
 *   zeroes RDX on error to avoid stale register reuse) */
static inline wasmos_ipc_call_result_t
wasmos_sys_ipc_call_result(uint32_t endpoint,
                           uint32_t type,
                           uint32_t arg0,
                           uint32_t arg1,
                           uint32_t arg2,
                           uint32_t arg3)
{
    wasmos_sysret2_t raw = wasmos_sys_ipc_call(endpoint, type, arg0, arg1, arg2, arg3);
    wasmos_ipc_call_result_t out;
    out.status = raw.rax;
    out.reply_arg0 = (uint32_t)raw.rdx;
    return out;
}

/* Does not return when syscall path succeeds. */
static inline void
wasmos_sys_exit(int32_t status)
{
    (void)wasmos_syscall1(WASMOS_SYSCALL_EXIT, (uint32_t)status);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static inline void
wasmos_sys_thread_exit(int32_t status)
{
    (void)wasmos_syscall1(WASMOS_SYSCALL_THREAD_EXIT, (uint32_t)status);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

#endif /* __x86_64__ && !__wasm__ */
#endif /* WASMOS_LIBC_WASMOS_SYSCALL_X86_64_H */
