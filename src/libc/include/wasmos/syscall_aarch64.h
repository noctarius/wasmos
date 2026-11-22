/* syscall_aarch64.h - aarch64 trap stubs of the native ring-3 syscall ABI
 * (svc #0). Included only through wasmos/syscall_native.h, which defines the
 * syscall numbers, the result records and the wrappers built on these.
 *
 * The syscall number goes in x8, arguments in x0..x5, and the results come
 * back in x0 and x1 (the Linux arm64 assignment; the kernel side decodes it in
 * arch/aarch64/arch_context.h). Every variant clobbers memory, so surrounding
 * loads and stores are not reordered across the trap.
 *
 * TODO: the kernel has no EL0 SVC entry yet (arm64 port, Phase 2); until then
 * these stubs only compile. */
#ifndef WASMOS_LIBC_WASMOS_SYSCALL_AARCH64_H
#define WASMOS_LIBC_WASMOS_SYSCALL_AARCH64_H

#include <stdint.h>

static inline int64_t wasmos_syscall0(uint64_t id) {
    register uint64_t x8 __asm__("x8") = id;
    register uint64_t x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "x1", "memory");
    return (int64_t)x0;
}

static inline int64_t wasmos_syscall1(uint64_t id, uint64_t arg0) {
    register uint64_t x8 __asm__("x8") = id;
    register uint64_t x0 __asm__("x0") = arg0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "x1", "memory");
    return (int64_t)x0;
}

static inline wasmos_sysret2_t wasmos_syscall6_ret2(uint64_t id, uint64_t arg0, uint64_t arg1,
                                                    uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                                    uint64_t arg5) {
    wasmos_sysret2_t out;
    register uint64_t x8 __asm__("x8") = id;
    register uint64_t x0 __asm__("x0") = arg0;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x3 __asm__("x3") = arg3;
    register uint64_t x4 __asm__("x4") = arg4;
    register uint64_t x5 __asm__("x5") = arg5;
    __asm__ volatile("svc #0"
                     : "+r"(x0), "+r"(x1)
                     : "r"(x8), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    out.r0 = (int64_t)x0;
    out.r1 = (int64_t)x1;
    return out;
}

/* The caller's stack pointer. */
static inline __attribute__((always_inline)) uintptr_t wasmos_native_read_sp(void) {
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

#endif /* WASMOS_LIBC_WASMOS_SYSCALL_AARCH64_H */
