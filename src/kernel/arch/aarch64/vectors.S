/* vectors.S - aarch64 exception vector table, the common exception entry and
 * exit, and arch_call_on_stack.
 *
 * Every vector saves an irq_frame_t (include/arch/aarch64/arch_context.h) on
 * the current EL1 stack, with the FP/SIMD state (q0..q31, FPSR, FPCR) in an
 * area directly above it, and calls aarch64_exception(frame, kind), where kind
 * is the vector index: (source << 2) | type, source 0 = EL1 with SP_EL0,
 * 1 = EL1 with SP_ELx, 2 = EL0 AArch64, 3 = EL0 AArch32; type 0 = synchronous,
 * 1 = IRQ, 2 = FIQ, 3 = SError. When the handler returns, the frame is
 * restored (FP/SIMD state, SPSR, ELR, SP_EL0 for an EL0 frame, x0..x30) and
 * the exception returns. The FP/SIMD state is saved whole because the handlers
 * are C that may use vector registers, and the interrupted code may be the
 * wasm3 interpreter running float code at EL1.
 *
 * TODO: an EL1 frame's `sp` is recorded but not restored, so redirecting an
 * interrupted kernel context onto another stack is not supported yet; neither
 * is a separate exception stack (arm64 port, Phase 2).
 */
.set IRQ_FRAME_SIZE, 272
/* q0..q31 (512 bytes), then FPSR and FPCR. */
.set FP_AREA, IRQ_FRAME_SIZE
.set FP_AREA_SIZE, 528
.set FRAME_SIZE, (IRQ_FRAME_SIZE + FP_AREA_SIZE)

.macro VENTRY kind
.balign 128
    sub sp, sp, #FRAME_SIZE
    stp x0, x1, [sp, #0]
    mov x1, #\kind
    b aarch64_exception_entry
.endm

.section .text
.balign 2048
.global aarch64_vectors
aarch64_vectors:
    VENTRY 0
    VENTRY 1
    VENTRY 2
    VENTRY 3
    VENTRY 4
    VENTRY 5
    VENTRY 6
    VENTRY 7
    VENTRY 8
    VENTRY 9
    VENTRY 10
    VENTRY 11
    VENTRY 12
    VENTRY 13
    VENTRY 14
    VENTRY 15

.extern aarch64_exception
aarch64_exception_entry:
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    add x9, sp, #FP_AREA
    stp q0, q1, [x9, #0]
    stp q2, q3, [x9, #32]
    stp q4, q5, [x9, #64]
    stp q6, q7, [x9, #96]
    stp q8, q9, [x9, #128]
    stp q10, q11, [x9, #160]
    stp q12, q13, [x9, #192]
    stp q14, q15, [x9, #224]
    stp q16, q17, [x9, #256]
    stp q18, q19, [x9, #288]
    stp q20, q21, [x9, #320]
    stp q22, q23, [x9, #352]
    stp q24, q25, [x9, #384]
    stp q26, q27, [x9, #416]
    stp q28, q29, [x9, #448]
    stp q30, q31, [x9, #480]
    mrs x10, fpsr
    mrs x11, fpcr
    add x12, x9, #512
    stp x10, x11, [x12]
    mrs x2, spsr_el1
    mrs x3, elr_el1
    and x4, x2, #0xf
    cbz x4, 1f
    add x4, sp, #FRAME_SIZE
    b 2f
1:
    mrs x4, sp_el0
2:
    str x4, [sp, #248]
    str x3, [sp, #256]
    str x2, [sp, #264]
    mov x0, sp
    bl aarch64_exception

    add x9, sp, #FP_AREA
    ldp q0, q1, [x9, #0]
    ldp q2, q3, [x9, #32]
    ldp q4, q5, [x9, #64]
    ldp q6, q7, [x9, #96]
    ldp q8, q9, [x9, #128]
    ldp q10, q11, [x9, #160]
    ldp q12, q13, [x9, #192]
    ldp q14, q15, [x9, #224]
    ldp q16, q17, [x9, #256]
    ldp q18, q19, [x9, #288]
    ldp q20, q21, [x9, #320]
    ldp q22, q23, [x9, #352]
    ldp q24, q25, [x9, #384]
    ldp q26, q27, [x9, #416]
    ldp q28, q29, [x9, #448]
    ldp q30, q31, [x9, #480]
    add x12, x9, #512
    ldp x10, x11, [x12]
    msr fpsr, x10
    msr fpcr, x11
    ldr x2, [sp, #264]
    ldr x3, [sp, #256]
    msr spsr_el1, x2
    msr elr_el1, x3
    and x4, x2, #0xf
    cbnz x4, 3f
    ldr x4, [sp, #248]
    msr sp_el0, x4
3:
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    ldp x0, x1, [sp, #0]
    add sp, sp, #FRAME_SIZE
    eret

/* uint64_t arch_call_on_stack(stack_top, fn, a0, a1, a2): call fn(a0, a1, a2)
 * on the stack whose top is stack_top and return its result on the caller's
 * stack. x19 holds the caller's stack pointer across the call (callee-saved,
 * so fn preserves it). */
.global arch_call_on_stack
arch_call_on_stack:
    stp x29, x30, [sp, #-32]!
    str x19, [sp, #16]
    mov x29, sp
    mov x19, sp
    mov sp, x0
    mov x9, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    blr x9
    mov sp, x19
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/* void aarch64_switch_ttbr1(uint64_t ttbr1): install a new kernel-half table
 * and drop every cached translation. Called through its identity alias
 * (TTBR0), with IRQs masked: it touches no TTBR1 address -- no stack, no data
 * -- so no access can hit a mix of old and new kernel-half entries, which on
 * hardware is a TLB conflict when the two use different block sizes
 * (ARM ARM D8.16.2). Returns to the caller's TTBR1 address afterwards. */
.global aarch64_switch_ttbr1
aarch64_switch_ttbr1:
    dsb ishst
    msr ttbr1_el1, x0
    isb
    tlbi vmalle1is
    dsb ish
    isb
    ret
