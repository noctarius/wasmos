/* warp_ring3_arch.h - aarch64 half of WARP's ring-3 execution model. Included
 * only through arch/warp_ring3_arch.h, which documents the contract.
 *
 * WARP's aarch64 backend calls native code with AAPCS64: integer arguments in
 * x0..x7, the rest on the stack from sp upward in 8-byte slots, result in x0,
 * return address in x30. A host-call stub is `movz x8, #nr; svc #0; ret`
 * (the kernel's EL0 syscall convention takes the number in x8): svc leaves
 * x0..x7 as the JIT set them, so the syscall frame holds the first eight
 * arguments, and the stub's caller pushed nothing, so the ninth is at the user
 * sp the exception saved. */
#pragma once

#include <stdint.h>

#include "arch/arch_context.h"

/* Bytes per host-call stub: three 4-byte instructions, padded to 16. */
#define WARP_R3_HC_STUB_BYTES 16u

/* Host-call arguments in registers, and the offset from the trapping user sp
 * of the first one on the stack. */
#define WARP_R3_HC_REG_ARGS 8u
#define WARP_R3_HC_STACK_ARGS_AT 0u

/* Host-call argument `index` of the call that trapped with `frame`: the first
 * eight from x0..x7, the rest from the caller's stack. The stack is read through
 * the guest's user mapping, so the guest's root must be current, and the caller
 * must have checked the stack pointer with arch_warp_r3_hostcall_stack_ok. */
static inline uint64_t arch_warp_r3_hostcall_arg(const syscall_frame_t* frame, unsigned index) {
    if (index < WARP_R3_HC_REG_ARGS) {
        return frame->x[index];
    }
    uint64_t sp = arch_syscall_user_sp(frame);
    return *(const uint64_t*)(uintptr_t)(sp + WARP_R3_HC_STACK_ARGS_AT +
                                         (uint64_t)(index - WARP_R3_HC_REG_ARGS) * 8u);
}
