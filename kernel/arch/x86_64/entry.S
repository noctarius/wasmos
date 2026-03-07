.section .text
.global _start
.extern kmain
.extern __bss_start
.extern __bss_end
.extern __data_start
.extern __data_end
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
    lea __data_start(%rip), %rdi
    lea __data_end(%rip), %rsi
    sub %rdi, %rsi
    test %rsi, %rsi
    jz 2f
    xor %rax, %rax
    rep stosb
2:
    mov %rbx, %rdi
    call kmain

3:
    hlt
    jmp 3b
