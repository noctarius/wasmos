.text
.globl wasmos_native_coroutine_context_switch
.type wasmos_native_coroutine_context_switch,@function
/* Save the SysV callee-saved register set and resume another context.
 * Context layout is wasmos_native_coroutine_context_t in coroutine_native.h. */
wasmos_native_coroutine_context_switch:
    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    leaq .Lresume(%rip), %rax
    movq %rax, 56(%rdi)

    movq 8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    movq 0(%rsi), %rsp
    jmp *56(%rsi)
.Lresume:
    ret
.size wasmos_native_coroutine_context_switch, .-wasmos_native_coroutine_context_switch
