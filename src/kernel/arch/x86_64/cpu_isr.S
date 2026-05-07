.section .text

# The ISR stubs are intentionally split: exceptions either panic or hand control
# to a dedicated fault handler, while IRQ stubs do the minimum register save
# needed before delegating to C.

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

.macro DECL_EXC num
.global isr_exception_\num
isr_exception_\num:
    # Simple exceptions currently panic immediately; richer recovery is only
    # implemented for the faults that already have kernel-side handlers.
    mov $\num, %rdi
    call x86_exception_panic
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
 .global isr_exception_1
isr_exception_1:
    pushq $0
    mov %rsp, %rsi
    mov $1, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
DECL_EXC 2
 .global isr_exception_3
isr_exception_3:
    pushq $0
    mov %rsp, %rsi
    mov $3, %rdi
    call x86_exception_panic_frame
1:
    cli
    hlt
    jmp 1b
DECL_EXC 4
DECL_EXC 5
 .global isr_exception_6
isr_exception_6:
    pushq $0
    mov %rsp, %rsi
    mov $6, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
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
DECL_EXC 7
DECL_EXC 8
DECL_EXC 9
DECL_EXC 10
DECL_EXC 11
DECL_EXC 12
 .global isr_exception_13
isr_exception_13:
    mov %rsp, %rsi
    mov $13, %rdi
    call x86_user_exception_handler
    test %eax, %eax
    jz 2f
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
    # Page faults are the only recoverable exception path today. Push the full
    # register set so the C handler can inspect and potentially resume.
    PUSH_REGS
    mov 120(%rsp), %rdi
    leaq 120(%rsp), %rsi
    subq $8, %rsp
    call x86_page_fault_handler
    addq $8, %rsp
    test %eax, %eax
    jz 2f
    leaq 120(%rsp), %rsi
    mov $14, %rdi
    call x86_exception_panic_frame
2:
    POP_REGS
    add $8, %rsp
    iretq
DECL_EXC 15
DECL_EXC 16
DECL_EXC 17
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

 .global isr_irq_0
isr_irq_0:
    # IRQ0 is special because it drives timer preemption. Capture the IRET frame,
    # preserve a small canary-protected scratch area, and let the C timer path
    # decide whether the interrupted RIP should be redirected.
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
    subq $8, %rsp
    call x86_timer_irq_handler
    addq $8, %rsp
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
    subq $8, %rsp
    call x86_irq_handler
    addq $8, %rsp
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

.global isr_syscall_128
isr_syscall_128:
    # Syscall boundary (int 0x80). The C handler receives a pointer to the
    # saved register frame and returns the syscall result in RAX.
    PUSH_REGS
    mov %rsp, %rdi
    subq $8, %rsp
    call x86_syscall_handler
    addq $8, %rsp
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
