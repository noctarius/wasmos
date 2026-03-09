.section .text

.extern x86_exception_panic
.extern x86_page_fault_handler
.extern x86_irq_handler
.extern x86_timer_irq_handler

.macro PUSH_REGS
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax
.endm

.macro POP_REGS
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
.endm

.macro DECL_EXC num
.global isr_exception_\num
isr_exception_\num:
    mov $\num, %rdi
    call x86_exception_panic
1:
    cli
    hlt
    jmp 1b
.endm

DECL_EXC 0
DECL_EXC 1
DECL_EXC 2
DECL_EXC 3
DECL_EXC 4
DECL_EXC 5
DECL_EXC 6
DECL_EXC 7
DECL_EXC 8
DECL_EXC 9
DECL_EXC 10
DECL_EXC 11
DECL_EXC 12
DECL_EXC 13
 .global isr_exception_14
isr_exception_14:
    PUSH_REGS
    mov 120(%rsp), %rdi
    call x86_page_fault_handler
    test %eax, %eax
    jz 2f
    mov $14, %rdi
    call x86_exception_panic
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
    PUSH_REGS
    mov %rsp, %rdi
    call x86_timer_irq_handler
    test %rax, %rax
    jz 1f
    movq %rax, %rdx
    movq 0(%rdx), %r15
    movq 8(%rdx), %r14
    movq 16(%rdx), %r13
    movq 24(%rdx), %r12
    movq 32(%rdx), %r11
    movq 40(%rdx), %r10
    movq 48(%rdx), %r9
    movq 56(%rdx), %r8
    movq 80(%rdx), %rbp
    movq 104(%rdx), %rbx
    movq 120(%rdx), %rsp
    movq 136(%rdx), %rax
    pushq %rax
    popfq
    movq 112(%rdx), %rax
    movq 64(%rdx), %rdi
    movq 72(%rdx), %rsi
    subq $8, %rsp
    movq 128(%rdx), %rcx
    movq %rcx, (%rsp)
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    ret
1:
    POP_REGS
    iretq

.macro DECL_IRQ num vec
.global isr_irq_\num
isr_irq_\num:
    PUSH_REGS
    mov $\vec, %rdi
    call x86_irq_handler
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
