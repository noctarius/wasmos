/* warp_jmp_aarch64.S - warp_setjmp/warp_longjmp for aarch64; the contract is in
 * include/arch/warp_jmp.h. Buffer layout, one 8-byte word each:
 *   [0] sp  [1..10] x19..x28  [11] x29  [12] x30  [13..20] d8..d15
 * Only AAPCS64 callee-saved state is kept: everything else is dead across the
 * call to warp_setjmp by the calling convention. Builds for the kernel and,
 * with the Darwin symbol prefix, for an arm64 host unit test. */

#if defined(__APPLE__)
#define WARP_SETJMP _warp_setjmp_aarch64
#define WARP_LONGJMP _warp_longjmp_aarch64
#else
#define WARP_SETJMP warp_setjmp_aarch64
#define WARP_LONGJMP warp_longjmp_aarch64
#endif

.text

/* int warp_setjmp_aarch64(void** buf): record and return 0. */
.globl WARP_SETJMP
.p2align 2
WARP_SETJMP:
    mov x1, sp
    str x1, [x0, #0]
    stp x19, x20, [x0, #8]
    stp x21, x22, [x0, #24]
    stp x23, x24, [x0, #40]
    stp x25, x26, [x0, #56]
    stp x27, x28, [x0, #72]
    stp x29, x30, [x0, #88]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

/* void warp_longjmp_aarch64(void** buf): restore and return 1 from the
 * warp_setjmp_aarch64 call that filled buf. */
.globl WARP_LONGJMP
.p2align 2
WARP_LONGJMP:
    ldp x19, x20, [x0, #8]
    ldp x21, x22, [x0, #24]
    ldp x23, x24, [x0, #40]
    ldp x25, x26, [x0, #56]
    ldp x27, x28, [x0, #72]
    ldp x29, x30, [x0, #88]
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    ldr x1, [x0, #0]
    mov sp, x1
    mov w0, #1
    ret
