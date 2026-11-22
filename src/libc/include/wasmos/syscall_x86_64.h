/* syscall_x86_64.h - x86_64 trap stubs of the native ring-3 syscall ABI
 * (int 0x80). Included only through wasmos/syscall_native.h, which defines the
 * syscall numbers, the result records and the wrappers built on these. */
#ifndef WASMOS_LIBC_WASMOS_SYSCALL_X86_64_H
#ifndef WASMOS_LIBC_WASMOS_SYSCALL_NATIVE_H
/* Included directly: syscall_native.h defines the records these stubs return
 * and includes this header again at the point they are needed. */
#include "wasmos/syscall_native.h"
#else
#define WASMOS_LIBC_WASMOS_SYSCALL_X86_64_H

#include <stdint.h>

#if defined(__x86_64__) && !defined(__wasm__)

/* int-0x80 entry stubs. Arguments go in RDI, RSI, RDX, RCX, R8, R9 (RCX is
 * usable because this is a software interrupt, not SYSCALL) and the result comes
 * back in RAX. Every variant clobbers memory, so surrounding loads and stores
 * are not reordered across the trap. */
static inline int64_t wasmos_syscall0(uint64_t id) {
    uint64_t rax = id;
    __asm__ volatile("int $0x80" : "+a"(rax) : : "memory");
    return (int64_t)rax;
}

static inline int64_t wasmos_syscall1(uint64_t id, uint64_t arg0) {
    uint64_t rax = id;
    uint64_t rdi = arg0;
    __asm__ volatile("int $0x80" : "+a"(rax) : "D"(rdi) : "memory");
    return (int64_t)rax;
}

static inline wasmos_sysret2_t wasmos_syscall6_ret2(uint64_t id, uint64_t arg0, uint64_t arg1,
                                                    uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                                    uint64_t arg5) {
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
    out.r0 = (int64_t)rax;
    out.r1 = (int64_t)rdx;
    return out;
}

/* The caller's stack pointer. */
static inline __attribute__((always_inline)) uintptr_t wasmos_native_read_sp(void) {
    uintptr_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
}

#endif /* __x86_64__ && !__wasm__ */
#endif /* WASMOS_LIBC_WASMOS_SYSCALL_NATIVE_H */
#endif /* WASMOS_LIBC_WASMOS_SYSCALL_X86_64_H */
