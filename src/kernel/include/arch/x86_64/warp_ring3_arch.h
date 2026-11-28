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

/* Host-call arguments in registers, and the offset from the trapping user sp
 * of the first one on the stack: past the return address the call into the
 * stub pushed. */
#define WARP_R3_HC_REG_ARGS 6u
#define WARP_R3_HC_STACK_ARGS_AT 8u

/* Host-call argument `index` of the call that trapped with `frame`: the first
 * six from the registers int 0x80 preserved, the rest from the caller's stack.
 * The stack is read through the guest's user mapping, so the guest's root must
 * be current, and the caller must have checked the stack pointer with
 * arch_warp_r3_hostcall_stack_ok. */
static inline uint64_t arch_warp_r3_hostcall_arg(const syscall_frame_t* frame, unsigned index) {
    if (index < WARP_R3_HC_REG_ARGS) {
        return arch_syscall_arg(frame, index);
    }
    uint64_t sp = arch_syscall_user_sp(frame);
    return *(const uint64_t*)(uintptr_t)(sp + WARP_R3_HC_STACK_ARGS_AT +
                                         (uint64_t)(index - WARP_R3_HC_REG_ARGS) * 8u);
}
