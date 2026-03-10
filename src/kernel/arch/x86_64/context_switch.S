.section .text
.global context_switch
.global context_switch_to
.extern g_in_context_switch
.extern g_in_scheduler
.extern g_sched_ctx
context_switch:
    movb $1, g_in_context_switch(%rip)
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r11, 32(%rdi)
    movq %r10, 40(%rdi)
    movq %r9, 48(%rdi)
    movq %r8, 56(%rdi)
    movq %rdi, 64(%rdi)
    movq %rsi, 72(%rdi)
    movq %rbp, 80(%rdi)
    movq %rdx, 88(%rdi)
    movq %rcx, 96(%rdi)
    movq %rbx, 104(%rdi)
    movq %rax, 112(%rdi)
    movq %rsp, 120(%rdi)
    leaq 1f(%rip), %rax
    movq %rax, 128(%rdi)
    pushfq
    popq %rax
    movq %rax, 136(%rdi)

    movq %rsi, %rdx
    movq 0(%rdx), %r15
    movq 8(%rdx), %r14
    movq 16(%rdx), %r13
    movq 24(%rdx), %r12
    movq 32(%rdx), %r11
    movq 40(%rdx), %r10
    movq 48(%rdx), %r9
    movq 56(%rdx), %r8
    movq 64(%rdx), %rdi
    movq 72(%rdx), %rsi
    movq 80(%rdx), %rbp
    movq 104(%rdx), %rbx
    movq 120(%rdx), %rsp
    movq 136(%rdx), %rax
    movq 128(%rdx), %rcx
    subq $16, %rsp
    movq %rax, (%rsp)
    movq %rcx, 8(%rsp)
    popfq
    movq 112(%rdx), %rax
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    movb $0, g_in_context_switch(%rip)
    ret
1:
    movb $0, g_in_context_switch(%rip)
    ret

context_switch_to:
    movb $1, g_in_context_switch(%rip)
    movq %rdi, %rdx
    movq 0(%rdx), %r15
    movq 8(%rdx), %r14
    movq 16(%rdx), %r13
    movq 24(%rdx), %r12
    movq 32(%rdx), %r11
    movq 40(%rdx), %r10
    movq 48(%rdx), %r9
    movq 56(%rdx), %r8
    movq 64(%rdx), %rdi
    movq 72(%rdx), %rsi
    movq 80(%rdx), %rbp
    movq 104(%rdx), %rbx
    movq 120(%rdx), %rsp
    movq 136(%rdx), %rax
    movq 128(%rdx), %rcx
    subq $16, %rsp
    movq %rax, (%rsp)
    movq %rcx, 8(%rsp)
    popfq
    movq 112(%rdx), %rax
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    movb $0, g_in_context_switch(%rip)
    ret

.global process_preempt_trampoline
process_preempt_trampoline:
    leaq g_sched_ctx(%rip), %rdi
    jmp context_switch_to
