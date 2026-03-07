.section .text
.global _start
.extern kmain
.extern __bss_start
.extern __bss_end
.extern __stack_top

_start:
    mov %rcx, %rbx
    lea __stack_top(%rip), %rsp

    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rsi
    sub %rdi, %rsi
    xor %rax, %rax
    test %rsi, %rsi
    jz 1f
    rep stosb
1:
    mov %rbx, %rdi
    call kmain

2:
    hlt
    jmp 2b
