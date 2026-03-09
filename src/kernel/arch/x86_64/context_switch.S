.section .text
.global context_switch
context_switch:
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbx, 32(%rdi)
    movq %rbp, 40(%rdi)
    movq %rsp, 48(%rdi)
    leaq 1f(%rip), %rax
    movq %rax, 56(%rdi)

    movq 0(%rsi), %r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbx
    movq 40(%rsi), %rbp
    movq 48(%rsi), %rsp
    movq 56(%rsi), %rax
    jmp *%rax
1:
    ret
