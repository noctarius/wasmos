.section .text

# ISR stubs. Exception stubs either panic or hand control to a fault handler
# that may resume the faulting context; IRQ stubs save the register set and
# delegate to C.
#
# Register-frame contract shared with the C side (irq_frame_t in process.h,
# x86_exception_panic_common in cpu_x86_64.c). PUSH_REGS leaves 15 quadwords
# directly below the CPU-pushed frame: r15 at +0 rising to rax at +112. The CPU
# frame therefore starts at +120 and is, in long mode, always five slots wide —
# rip, cs, rflags, rsp, ss — preceded by an error code for the vectors that push
# one. Stubs for vectors without an architectural error code push a zero in its
# place when the C handler expects the error-code form.
#
# Consequences the offsets below depend on: irq_frame_t maps the no-error-code
# layout (rip at +120, user_rsp at +144), the #PF stub reads its error code at
# +120, and a syscall result is stored into the saved rax slot at +112.

.extern x86_exception_panic
.extern x86_exception_panic_frame
.extern x86_page_fault_handler
.extern x86_user_exception_handler
.extern x86_syscall_handler
.extern x86_irq_handler
.extern x86_timer_irq_handler
.extern x86_irq_iret_corrupt
.extern process_preempt_trampoline
.extern g_irq0_ist_stack
.extern g_irq0_ist_canary
.extern x86_irq_ist_corrupt

# Kernel code selector used when the timer preempt path rewrites a CPL3 IRET
# frame to return through a ring0 scheduler trampoline.
#define KERNEL_CS_SELECTOR 0x08

.macro PUSH_REGS
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POP_REGS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

# Call a C function with the 16-byte stack alignment the SysV ABI requires at a
# call site; an interrupt can be taken at any alignment. The unaligned %rsp is
# saved in the 16-byte scratch area and put back afterwards, so the frame the
# stub built is left exactly as it was.
.macro CALL_C_ALIGNED target
    movq %rsp, %rax
    andq $-16, %rsp
    subq $16, %rsp
    movq %rax, 8(%rsp)
    call \target
    movq 8(%rsp), %rsp
.endm

# NMI handler (vector 2), used by kpanic() to stop this CPU and snapshot its
# context. Saves all GPRs so x86_nmi_handler can read rbp (for a backtrace) and
# the iret frame (rip/cs/rflags/rsp). In the panic case x86_nmi_handler never
# returns (it halts this CPU); for an unexpected NMI it returns and this iretqs.
.extern x86_nmi_handler
.global isr_nmi
isr_nmi:
    PUSH_REGS
    movq %rsp, %rdi          # arg0 = pointer to saved register frame
    CALL_C_ALIGNED x86_nmi_handler
    POP_REGS
    iretq

# Catch-all exception stub: no recovery, straight to the panic dump. Only the
# vectors with a dedicated kernel-side handler (#PF and the user-fault vectors)
# get their own stub below.
#
# FIXME: this stub passes the post-PUSH_REGS %rsp, which x86_exception_panic_frame
# reads as the error-code form (+120 = err). That is correct only for the vectors
# declared here that push an error code (8, 10, 11, 21, 29, 30); for the rest
# the dump reports rip as "err", cs as "rip" and so on. The stub would need a
# `pushq $0` for the no-error-code vectors, as isr_exception_3 already does.
.macro DECL_EXC num
.global isr_exception_\num
isr_exception_\num:
    PUSH_REGS
    movq %rsp, %rsi
    mov $\num, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
.endm

.global isr_exception_0
isr_exception_0:
    pushq $0
    mov %rsp, %rsi
    mov $0, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $0, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq

# FIXME: the panic path here omits the PUSH_REGS that every other user-fault
# stub performs, so x86_exception_panic_frame is handed the CPU frame itself
# instead of the register block that precedes it and dumps garbage for a #DB
# the user handler declined.
.global isr_exception_1
isr_exception_1:
    pushq $0
    mov %rsp, %rsi
    mov $1, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    mov %rsp, %rsi
    mov $1, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq

DECL_EXC 2

.global isr_exception_3
isr_exception_3:
    pushq $0
    PUSH_REGS
    mov %rsp, %rsi
    mov $3, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b

 .global isr_exception_4
isr_exception_4:
    pushq $0
    mov %rsp, %rsi
    mov $4, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $4, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq
DECL_EXC 5

.global isr_exception_6
isr_exception_6:
    pushq $0
    mov %rsp, %rsi
    mov $6, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $6, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq

 .global isr_exception_7
isr_exception_7:
    pushq $0
    mov %rsp, %rsi
    mov $7, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $7, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq
DECL_EXC 8
DECL_EXC 9
DECL_EXC 10
DECL_EXC 11
 .global isr_exception_12
isr_exception_12:
    mov %rsp, %rsi
    mov $12, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $12, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq

.global isr_exception_13
isr_exception_13:
    mov %rsp, %rsi
    mov $13, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $13, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq

.global isr_exception_14
isr_exception_14:
    # #PF is the only exception whose handler can return to the faulting
    # instruction (page mapped on demand, or a stale-TLB retry), so the full
    # register set is saved and restored around the call. arg0 = error code,
    # arg1 = the CPU frame it heads.
    PUSH_REGS
    mov 120(%rsp), %rdi
    leaq 120(%rsp), %rsi
    subq $8, %rsp
    call x86_page_fault_handler
    addq $8, %rsp
    test %eax, %eax
    jz 2f
    movq %rsp, %rsi
    mov $14, %rdi
    call x86_exception_panic_frame
2:
    POP_REGS
    add $8, %rsp
    iretq

DECL_EXC 15
DECL_EXC 16
 .global isr_exception_17
isr_exception_17:
    mov %rsp, %rsi
    mov $17, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
    PUSH_REGS
    mov %rsp, %rsi
    mov $17, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
2:
    add $8, %rsp
    iretq
DECL_EXC 18
DECL_EXC 19
DECL_EXC 20
DECL_EXC 21
DECL_EXC 22
DECL_EXC 23
DECL_EXC 24
DECL_EXC 25
DECL_EXC 26
DECL_EXC 27
DECL_EXC 28
DECL_EXC 29
DECL_EXC 30
DECL_EXC 31

# IRQ0 drives timer preemption, so its C handler is allowed to rewrite the IRET
# frame in place — and a handler bug there returns the CPU to an arbitrary
# address. The stub therefore brackets the call with two checks:
#
#  1. IST overflow: this vector runs on IST1, and the lowest quadword of the
#     BSP's IST1 stack holds g_irq0_ist_canary. A mismatch means the stack grew
#     down into its own guard word. Every CPU compares that one array, so an AP
#     with its own IST1 stack (smp.c) is not covered.
#  2. IRET-frame integrity: a 48-byte scratch area below the register block holds
#     two guard words (+0, +8) followed by copies of the entering rip/cs/rflags
#     (+16, +24, +32). After the call the copies are compared against the live
#     frame (rip at +168, cs at +176, rflags at +184 once the scratch is
#     subtracted). The only sanctioned divergence is the preemption rewrite:
#     rip -> process_preempt_trampoline with cs -> KERNEL_CS_SELECTOR. RFLAGS must
#     be unchanged; the matching ss/rsp rewrite is not checked here.
#
# Either check failing is fatal: the stub reports and halts rather than iretq to
# an unverified frame.
.global isr_irq_0
isr_irq_0:
    PUSH_REGS
    movq g_irq0_ist_canary(%rip), %rax
    cmpq %rax, g_irq0_ist_stack(%rip)
    jne 3f
    subq $48, %rsp
    movabs $0xC0FFEE0DD15EA5E, %rax
    movq %rax, (%rsp)
    movq %rax, 8(%rsp)
    movq 168(%rsp), %rax
    movq %rax, 16(%rsp)
    movq 176(%rsp), %rax
    movq %rax, 24(%rsp)
    movq 184(%rsp), %rax
    movq %rax, 32(%rsp)
    leaq 48(%rsp), %rdi
    CALL_C_ALIGNED x86_timer_irq_handler
    movabs $0xC0FFEE0DD15EA5E, %rax
    cmpq %rax, (%rsp)
    jne 1f
    cmpq %rax, 8(%rsp)
    jne 1f
    movq 16(%rsp), %rax
    cmpq %rax, 168(%rsp)
    je 0f
    movq 168(%rsp), %rax
    leaq process_preempt_trampoline(%rip), %rcx
    cmpq %rcx, %rax
    jne 1f
0:
    movq 24(%rsp), %rax
    cmpq %rax, 176(%rsp)
    je 5f
    cmpq $KERNEL_CS_SELECTOR, 176(%rsp)
    jne 1f
5:
    movq 32(%rsp), %rax
    cmpq %rax, 184(%rsp)
    jne 1f
    addq $48, %rsp
    POP_REGS
    iretq
1:
    leaq 16(%rsp), %rdi
    leaq 168(%rsp), %rsi
    call x86_irq_iret_corrupt
2:
    cli
    hlt
    jmp 2b
3:
    call x86_irq_ist_corrupt
4:
    cli
    hlt
    jmp 4b

.macro DECL_IRQ num vec
.global isr_irq_\num
isr_irq_\num:
    PUSH_REGS
    mov $\vec, %rdi
    CALL_C_ALIGNED x86_irq_handler
    POP_REGS
    iretq
.endm

/*
 * MSI vectors (48-63). Identical prologue to DECL_IRQ, but they land in
 * x86_msi_handler: a message-signalled interrupt is edge-triggered and owned by
 * exactly one endpoint, so there is no line to mask and no driver ack gating
 * re-arm — the handler's whole epilogue is the LAPIC EOI.
 */
.macro DECL_MSI num vec
.global isr_msi_\num
isr_msi_\num:
    PUSH_REGS
    mov $\vec, %rdi
    CALL_C_ALIGNED x86_msi_handler
    POP_REGS
    iretq
.endm

DECL_IRQ 1 33
DECL_IRQ 2 34
DECL_IRQ 3 35
DECL_IRQ 4 36
DECL_IRQ 5 37
DECL_IRQ 6 38
DECL_IRQ 7 39
DECL_IRQ 8 40
DECL_IRQ 9 41
DECL_IRQ 10 42
DECL_IRQ 11 43
DECL_IRQ 12 44
DECL_IRQ 13 45
DECL_IRQ 14 46
DECL_IRQ 15 47

DECL_MSI 0 48
DECL_MSI 1 49
DECL_MSI 2 50
DECL_MSI 3 51
DECL_MSI 4 52
DECL_MSI 5 53
DECL_MSI 6 54
DECL_MSI 7 55
DECL_MSI 8 56
DECL_MSI 9 57
DECL_MSI 10 58
DECL_MSI 11 59
DECL_MSI 12 60
DECL_MSI 13 61
DECL_MSI 14 62
DECL_MSI 15 63

/*
 * LAPIC spurious interrupt handler (vector 255).
 * The Intel SDM states that the processor does not latch a spurious
 * interrupt so no EOI is required — simply return from interrupt.
 */
.global isr_lapic_spurious
isr_lapic_spurious:
    iretq

.global isr_syscall_128
isr_syscall_128:
    # Syscall boundary (int 0x80). The C handler receives a pointer to the
    # saved register frame and returns the syscall result in RAX.
    PUSH_REGS
    mov %rsp, %rdi
    CALL_C_ALIGNED x86_syscall_handler
    movq %rax, 112(%rsp)
    POP_REGS
    iretq

.global x86_exception_stub_table
x86_exception_stub_table:
    .quad isr_exception_0
    .quad isr_exception_1
    .quad isr_exception_2
    .quad isr_exception_3
    .quad isr_exception_4
    .quad isr_exception_5
    .quad isr_exception_6
    .quad isr_exception_7
    .quad isr_exception_8
    .quad isr_exception_9
    .quad isr_exception_10
    .quad isr_exception_11
    .quad isr_exception_12
    .quad isr_exception_13
    .quad isr_exception_14
    .quad isr_exception_15
    .quad isr_exception_16
    .quad isr_exception_17
    .quad isr_exception_18
    .quad isr_exception_19
    .quad isr_exception_20
    .quad isr_exception_21
    .quad isr_exception_22
    .quad isr_exception_23
    .quad isr_exception_24
    .quad isr_exception_25
    .quad isr_exception_26
    .quad isr_exception_27
    .quad isr_exception_28
    .quad isr_exception_29
    .quad isr_exception_30
    .quad isr_exception_31

.global x86_irq_stub_table
x86_irq_stub_table:
    .quad isr_irq_0
    .quad isr_irq_1
    .quad isr_irq_2
    .quad isr_irq_3
    .quad isr_irq_4
    .quad isr_irq_5
    .quad isr_irq_6
    .quad isr_irq_7
    .quad isr_irq_8
    .quad isr_irq_9
    .quad isr_irq_10
    .quad isr_irq_11
    .quad isr_irq_12
    .quad isr_irq_13
    .quad isr_irq_14
    .quad isr_irq_15

.global x86_msi_stub_table
x86_msi_stub_table:
    .quad isr_msi_0
    .quad isr_msi_1
    .quad isr_msi_2
    .quad isr_msi_3
    .quad isr_msi_4
    .quad isr_msi_5
    .quad isr_msi_6
    .quad isr_msi_7
    .quad isr_msi_8
    .quad isr_msi_9
    .quad isr_msi_10
    .quad isr_msi_11
    .quad isr_msi_12
    .quad isr_msi_13
    .quad isr_msi_14
    .quad isr_msi_15
