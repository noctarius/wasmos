.section .text
.global _start
.extern kmain
.extern __bss_start
.extern __bss_end
.extern __stack_top

_start:
    # UEFI enters with the boot_info_t pointer in RCX under the Microsoft x64
    # calling convention. Preserve it before repointing RSP at the kernel stack.
    mov %rcx, %rbx
    lea __stack_top(%rip), %rsp

    # Clear .bss explicitly; there is no hosted runtime to do this for us.
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rsi
    sub %rdi, %rsi
    mov %rsi, %rcx
    xor %rax, %rax
    test %rcx, %rcx
    jz 1f
    rep stosb
1:
    # kmain() follows the SysV ABI, so move the preserved boot_info pointer into
    # RDI before handing off to C code.
    mov %rbx, %rdi
    call kmain

2:
    # The kernel is not expected to return. If it does, halt forever.
    hlt
    jmp 2b
