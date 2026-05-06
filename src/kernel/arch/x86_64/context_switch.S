.section .text
.global context_switch
.global context_switch_to
.extern g_in_context_switch
.extern g_in_scheduler
.extern g_sched_ctx
.extern g_ctxsw_last_out_ctx
.extern g_ctxsw_last_out_rip
.extern g_ctxsw_last_out_rsp
.extern g_ctxsw_last_out_rflags
.extern g_ctxsw_last_in_ctx
.extern g_ctxsw_last_in_rip
.extern g_ctxsw_last_in_rsp
.extern g_ctxsw_last_in_rflags
.extern g_ctx_watch_ctx
.extern g_ctx_watch_last_ctx
.extern g_ctx_watch_last_rip
.extern g_ctx_watch_last_rsp
.extern g_ctx_watch_last_rflags
.extern g_ctx_watch_hits
.extern g_ctx_watch_reason
.extern g_ctx_restore_ctx
.extern g_ctx_restore_rip
.extern g_ctx_restore_rsp
.extern g_ctx_restore_rflags
.extern g_current_pml4_phys

#define KERNEL_HIGHER_HALF_BASE 0xFFFFFFFF80000000

# The context switch path stores and restores the full general-purpose register
# set plus RIP/RSP/RFLAGS. It also carries CS/SS/user-RSP metadata so ring3
# contexts can resume via iretq while ring0 contexts keep the ret fast path.
context_switch:
    # Mark the transition so debug code can distinguish genuine process state
    # corruption from an in-flight save/restore sequence.
    movb $1, g_in_context_switch(%rip)
    movq %rdi, g_ctxsw_last_out_ctx(%rip)
    movq %rsi, g_ctxsw_last_in_ctx(%rip)
    movq g_ctx_watch_ctx(%rip), %rax
    cmpq %rax, %rdi
    jne 0f
    movq %rdi, g_ctx_watch_last_ctx(%rip)
    movq %rsp, g_ctx_watch_last_rsp(%rip)
    leaq 1f(%rip), %rax
    movq %rax, g_ctx_watch_last_rip(%rip)
    pushfq
    popq %rax
    movq %rax, g_ctx_watch_last_rflags(%rip)
    movq $1, g_ctx_watch_reason(%rip)
    incq g_ctx_watch_hits(%rip)
0:
    # Save the outgoing context first. RIP is synthesized from a local label so
    # resuming the old context returns just after the switch call.
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
    movq %rsp, g_ctxsw_last_out_rsp(%rip)
    movq %rsp, 120(%rdi)
    leaq 1f(%rip), %rax
    movq %rax, g_ctxsw_last_out_rip(%rip)
    movq %rax, 128(%rdi)
    pushfq
    popq %rax
    movq %rax, g_ctxsw_last_out_rflags(%rip)
    movq %rax, 136(%rdi)
    movq $0x08, 144(%rdi)
    movq $0x10, 152(%rdi)
    movq %rsp, 160(%rdi)
    movq %cr3, %rax
    movq %rax, 168(%rdi)

    movq %rsi, %rdx
    testq %rdx, %rdx
    js 7f
    movabs $KERNEL_HIGHER_HALF_BASE, %r15
    addq %r15, %rdx
7:
    movq %rsi, g_ctxsw_last_in_ctx(%rip)
    movq 120(%rdx), %rax
    movq %rax, g_ctxsw_last_in_rsp(%rip)
    movq 128(%rdx), %rax
    movq %rax, g_ctxsw_last_in_rip(%rip)
    movq 136(%rdx), %rax
    movq %rax, g_ctxsw_last_in_rflags(%rip)
    # Restore the incoming context and stage RFLAGS/RIP on the new stack so a
    # plain ret returns into the restored execution stream.
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
    movq 120(%rdx), %r10
    movq 136(%rdx), %rax
    movq 128(%rdx), %rcx
    movq 144(%rdx), %rsi
    movq 160(%rdx), %rdi
    movq 152(%rdx), %r11
    testq %rdx, %rdx
    js 8f
    movabs $KERNEL_HIGHER_HALF_BASE, %r15
    addq %r15, %rdx
8:
    movq 168(%rdx), %r8
    movq %r8, g_current_pml4_phys(%rip)
    movq %r8, %cr3
    movq 0(%rdx), %r15
    movq %r10, %rsp
    testb $0x3, %sil
    jne 2f
    testq %rcx, %rcx
    js 10f
    pushq %r15
    movabs $KERNEL_HIGHER_HALF_BASE, %r15
    addq %r15, %rcx
    popq %r15
10:
    subq $16, %rsp
    movq %rax, (%rsp)
    movq %rcx, 8(%rsp)
    movq %rdx, g_ctx_restore_ctx(%rip)
    movq %rcx, g_ctx_restore_rip(%rip)
    movq %rsp, g_ctx_restore_rsp(%rip)
    movq %rax, g_ctx_restore_rflags(%rip)
    popfq
    movq 112(%rdx), %rax
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    movb $0, g_in_context_switch(%rip)
    ret
2:
    movq %r11, %rsi
    subq $40, %rsp
    movq %rcx, 0(%rsp)
    movq 144(%rdx), %r11
    movq %r11, 8(%rsp)
    movq %rax, 16(%rsp)
    movq %rdi, 24(%rsp)
    movq %rsi, 32(%rsp)
    movq %rdx, g_ctx_restore_ctx(%rip)
    movq %rcx, g_ctx_restore_rip(%rip)
    movq %rsp, g_ctx_restore_rsp(%rip)
    movq %rax, g_ctx_restore_rflags(%rip)
    movq 112(%rdx), %rax
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    movb $0, g_in_context_switch(%rip)
    iretq
1:
    movb $0, g_in_context_switch(%rip)
    ret

context_switch_to:
    # context_switch_to is the one-way variant used when no outgoing task state
    # needs to be saved, for example when jumping from the scheduler context
    # into the very first runnable task or the preempt trampoline.
    movb $1, g_in_context_switch(%rip)
    movq %rdi, %rdx
    testq %rdx, %rdx
    js 6f
    movabs $KERNEL_HIGHER_HALF_BASE, %r15
    addq %r15, %rdx
6:
    movq %rdi, g_ctxsw_last_in_ctx(%rip)
    movq 120(%rdx), %rax
    movq %rax, g_ctxsw_last_in_rsp(%rip)
    movq 128(%rdx), %rax
    movq %rax, g_ctxsw_last_in_rip(%rip)
    movq 136(%rdx), %rax
    movq %rax, g_ctxsw_last_in_rflags(%rip)
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
    movq 120(%rdx), %r10
    movq 136(%rdx), %rax
    movq 128(%rdx), %rcx
    movq 144(%rdx), %rsi
    movq 160(%rdx), %rdi
    movq 152(%rdx), %r11
    testq %rdx, %rdx
    js 9f
    movabs $KERNEL_HIGHER_HALF_BASE, %r15
    addq %r15, %rdx
9:
    movq 168(%rdx), %r8
    movq %r8, g_current_pml4_phys(%rip)
    movq %r8, %cr3
    movq 0(%rdx), %r15
    movq %r10, %rsp
    testb $0x3, %sil
    jne 3f
    testq %rcx, %rcx
    js 11f
    pushq %r15
    movabs $KERNEL_HIGHER_HALF_BASE, %r15
    addq %r15, %rcx
    popq %r15
11:
    subq $16, %rsp
    movq %rax, (%rsp)
    movq %rcx, 8(%rsp)
    movq %rdx, g_ctx_restore_ctx(%rip)
    movq %rcx, g_ctx_restore_rip(%rip)
    movq %rsp, g_ctx_restore_rsp(%rip)
    movq %rax, g_ctx_restore_rflags(%rip)
    popfq
    movq 112(%rdx), %rax
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    movb $0, g_in_context_switch(%rip)
    ret
3:
    movq %r11, %rsi
    subq $40, %rsp
    movq %rcx, 0(%rsp)
    movq 144(%rdx), %r11
    movq %r11, 8(%rsp)
    movq %rax, 16(%rsp)
    movq %rdi, 24(%rsp)
    movq %rsi, 32(%rsp)
    movq %rdx, g_ctx_restore_ctx(%rip)
    movq %rcx, g_ctx_restore_rip(%rip)
    movq %rsp, g_ctx_restore_rsp(%rip)
    movq %rax, g_ctx_restore_rflags(%rip)
    movq 112(%rdx), %rax
    movq 96(%rdx), %rcx
    movq 88(%rdx), %rdx
    movb $0, g_in_context_switch(%rip)
    iretq

.global process_preempt_trampoline
process_preempt_trampoline:
    # Timer IRQ preemption rewrites the interrupted RIP to this symbol. Instead
    # of re-entering user code directly, control returns to the scheduler
    # context, which can then pick the next runnable process normally.
    leaq g_sched_ctx(%rip), %rdi
    jmp context_switch_to
