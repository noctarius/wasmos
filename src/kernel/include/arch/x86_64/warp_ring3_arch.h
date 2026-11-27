/* warp_ring3_arch.h - x86_64 half of WARP's ring-3 execution model. Included
 * only through arch/warp_ring3_arch.h, which documents the contract.
 *
 * WARP's x86_64 backend calls native code with the SysV ABI: integer arguments
 * in rdi, rsi, rdx, rcx, r8, r9, the rest on the stack, result in rax. A host
 * call stub is `mov eax, nr; int 0x80; ret`: int 0x80 leaves every argument
 * register as the JIT set it, so the syscall frame holds the call's first six
 * arguments, and the stub's caller left its return address at the user rsp the
 * CPU saved. */
#pragma once

#include <stdint.h>

#include "arch/arch_context.h"

/* Bytes per host-call stub in the trampoline page: mov eax, imm32 (5),
 * int 0x80 (2), ret (1). */
#define WARP_R3_HC_STUB_BYTES 8u

/* Host-call argument `index` of the call that trapped with `frame`: the first
 * six from the registers int 0x80 preserved, the rest from the caller's stack,
 * past the return address its call into the stub pushed. The stack is read
 * through the guest's user mapping, so the guest's root must be current. */
static inline uint64_t arch_warp_r3_hostcall_arg(const syscall_frame_t* frame, unsigned index) {
    if (index < 6u) {
        return arch_syscall_arg(frame, index);
    }
    uint64_t sp = arch_syscall_user_sp(frame);
    return *(const uint64_t*)(uintptr_t)(sp + 8u + (uint64_t)(index - 6u) * 8u);
}
