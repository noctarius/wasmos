/* context_switch.S - aarch64 thread context switch and preemption trampoline.
 *
 * Every displacement indexes process_context_t
 * (include/arch/aarch64/arch_context.h), whose layout is pinned there by
 * _Static_asserts: x[0..30] at 0, sp 248, pc 256, pstate 264, user_sp 272,
 * root_table 280, fp_callee (d8..d15) 288.
 *
 * Contexts are always reached through their higher-half addresses: the
 * aarch64 kernel never runs from the identity map, so nothing here rebases a
 * lower-half pointer as the x86_64 switch does.
 *
 * A context is resumed with ERET, for kernel (EL1h) and user (EL0t) contexts
 * alike, so the saved DAIF mask comes back atomically with the jump. The
 * restore runs with every exception masked while ELR/SPSR hold the target.
 */
.set CTX_SP, 248
.set CTX_PC, 256
.set CTX_PSTATE, 264
.set CTX_USER_SP, 272
.set CTX_ROOT, 280
.set CTX_FP, 288
/* Offset of cpu_local_t.in_context_switch, reached through TPIDR_EL1;
 * cpu_local.h asserts it. */
.set CPU_LOCAL_IN_CTX_SW_OFFSET, 13
.set SPSR_EL1H, 0x5
.set OA_MASK_LO, 0xfffffffff000

.section .text

/* restore_context: x0 = context. Never returns. */
restore_context:
    msr daifset, #0xf
    ldr x1, [x0, #CTX_PC]
    msr elr_el1, x1
    ldr x1, [x0, #CTX_PSTATE]
    msr spsr_el1, x1
    ldr x1, [x0, #CTX_USER_SP]
    msr sp_el0, x1
    ldr x1, [x0, #CTX_SP]
    mov sp, x1
    /* A different TTBR0 table: switch and drop the old root's translations
     * (no ASIDs yet, as in paging_switch_root). */
    ldr x1, [x0, #CTX_ROOT]
    cbz x1, 1f
    mrs x2, ttbr0_el1
    and x2, x2, #OA_MASK_LO
    cmp x1, x2
    b.eq 1f
    msr ttbr0_el1, x1
    isb
    dsb ishst
    tlbi vmalle1is
    dsb ish
    isb
1:
    ldp d8, d9, [x0, #(CTX_FP + 0)]
    ldp d10, d11, [x0, #(CTX_FP + 16)]
    ldp d12, d13, [x0, #(CTX_FP + 32)]
    ldp d14, d15, [x0, #(CTX_FP + 48)]
    mrs x1, tpidr_el1
    strb wzr, [x1, #CPU_LOCAL_IN_CTX_SW_OFFSET]
    ldp x2, x3, [x0, #16]
    ldp x4, x5, [x0, #32]
    ldp x6, x7, [x0, #48]
    ldp x8, x9, [x0, #64]
    ldp x10, x11, [x0, #80]
    ldp x12, x13, [x0, #96]
    ldp x14, x15, [x0, #112]
    ldp x16, x17, [x0, #128]
    ldp x18, x19, [x0, #144]
    ldp x20, x21, [x0, #160]
    ldp x22, x23, [x0, #176]
    ldp x24, x25, [x0, #192]
    ldp x26, x27, [x0, #208]
    ldp x28, x29, [x0, #224]
    ldr x30, [x0, #240]
    ldr x1, [x0, #8]
    ldr x0, [x0, #0]
    eret

/* void context_switch(process_context_t* out, process_context_t* in)
 *
 * Saves the caller's callee-saved state (x19..x30, d8..d15), stack pointers,
 * DAIF and TTBR0 into out with context_switch_resume as the resume PC, then
 * restores in. Returns only when some later switch resumes out, with every
 * callee-saved register as it was on entry. Sets
 * cpu_local_t.in_context_switch for the save/restore window. */
.global context_switch
context_switch:
    mrs x9, tpidr_el1
    mov w10, #1
    strb w10, [x9, #CPU_LOCAL_IN_CTX_SW_OFFSET]
    stp x19, x20, [x0, #152]
    stp x21, x22, [x0, #168]
    stp x23, x24, [x0, #184]
    stp x25, x26, [x0, #200]
    stp x27, x28, [x0, #216]
    stp x29, x30, [x0, #232]
    stp d8, d9, [x0, #(CTX_FP + 0)]
    stp d10, d11, [x0, #(CTX_FP + 16)]
    stp d12, d13, [x0, #(CTX_FP + 32)]
    stp d14, d15, [x0, #(CTX_FP + 48)]
    mov x9, sp
    str x9, [x0, #CTX_SP]
    adr x9, context_switch_resume
    str x9, [x0, #CTX_PC]
    mrs x9, daif
    mov x10, #SPSR_EL1H
    orr x9, x9, x10
    str x9, [x0, #CTX_PSTATE]
    mrs x9, sp_el0
    str x9, [x0, #CTX_USER_SP]
    mrs x9, ttbr0_el1
    and x9, x9, #OA_MASK_LO
    str x9, [x0, #CTX_ROOT]
    mov x0, x1
    b restore_context

/* The resume point of a context saved by context_switch: x30 holds the
 * original caller's return address again. Exported because a kernel worker's
 * scheduler context is snapshotted in C and names it (process.c). */
.global context_switch_resume
context_switch_resume:
    mrs x9, tpidr_el1
    strb wzr, [x9, #CPU_LOCAL_IN_CTX_SW_OFFSET]
    ret

/* void context_switch_to(process_context_t* in): restore in, abandoning the
 * caller's stack. Never returns. */
.global context_switch_to
context_switch_to:
    mrs x9, tpidr_el1
    mov w10, #1
    strb w10, [x9, #CPU_LOCAL_IN_CTX_SW_OFFSET]
    b restore_context

/* Resume target an IRQ handler plants to preempt the interrupted thread: hand
 * the CPU to its scheduler context, so the next decision is made on the
 * scheduler's own stack. Never returns. */
.global process_preempt_trampoline
process_preempt_trampoline:
    bl cpu_local_sched_ctx
    b context_switch_to
