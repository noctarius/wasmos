.section .start.low, "ax"
.global _start
.extern kmain
.extern __stack_top
.extern __bss_start
.extern __bss_end

.set PT_FLAG_PRESENT, 0x1
.set PT_FLAG_WRITE, 0x2
.set PT_FLAG_LARGE, 0x80
.set HIGHER_HALF_PDPT_INDEX, 510

_start:
    cld
    mov %rcx, bootstrap_boot_info(%rip)

    lea bootstrap_pml4(%rip), %rdi
    xor %rax, %rax
    mov $512, %rcx
    rep stosq
    lea bootstrap_pdpt_low(%rip), %rdi
    xor %rax, %rax
    mov $512, %rcx
    rep stosq
    lea bootstrap_pdpt_high(%rip), %rdi
    xor %rax, %rax
    mov $512, %rcx
    rep stosq

    lea bootstrap_pd0(%rip), %rdi
    call fill_pd_2m_identity
    lea bootstrap_pd1(%rip), %rdi
    mov $0x40000000, %rax
    call fill_pd_2m_identity_with_base
    lea bootstrap_pd2(%rip), %rdi
    mov $0x80000000, %rax
    call fill_pd_2m_identity_with_base
    lea bootstrap_pd3(%rip), %rdi
    mov $0xC0000000, %rax
    call fill_pd_2m_identity_with_base
    lea bootstrap_pd_high(%rip), %rdi
    xor %rax, %rax
    mov $32, %rcx
1:
    mov %rax, %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE), %rdx
    mov %rdx, (%rdi)
    add $8, %rdi
    add $0x200000, %rax
    loop 1b

    lea bootstrap_pml4(%rip), %rax
    lea bootstrap_pdpt_low(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, 0(%rax)
    lea bootstrap_pdpt_high(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, 511*8(%rax)

    lea bootstrap_pdpt_low(%rip), %rax
    lea bootstrap_pd0(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, 0(%rax)
    lea bootstrap_pd1(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, 1*8(%rax)
    lea bootstrap_pd2(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, 2*8(%rax)
    lea bootstrap_pd3(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, 3*8(%rax)

    lea bootstrap_pdpt_high(%rip), %rax
    lea bootstrap_pd_high(%rip), %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE), %rdx
    mov %rdx, HIGHER_HALF_PDPT_INDEX*8(%rax)

    lea bootstrap_pml4(%rip), %rax
    mov %rax, %cr3
    movabs $_start_high, %rax
    jmp *%rax

fill_pd_2m_identity:
    xor %rax, %rax
fill_pd_2m_identity_with_base:
    mov $512, %rcx
2:
    mov %rax, %rdx
    or $(PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE), %rdx
    mov %rdx, (%rdi)
    add $8, %rdi
    add $0x200000, %rax
    loop 2b
    ret

.section .text
.global _start_high
_start_high:
    lea __stack_top(%rip), %rsp
    cld
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %rax, %rax
    rep stosb
    movabs $bootstrap_boot_info, %rax
    mov (%rax), %rdi
    call kmain
3:
    hlt
    jmp 3b

.section .start.low.bss, "aw", @nobits
.align 4096
bootstrap_pml4:
    .skip 4096
bootstrap_pdpt_low:
    .skip 4096
bootstrap_pdpt_high:
    .skip 4096
bootstrap_pd0:
    .skip 4096
bootstrap_pd1:
    .skip 4096
bootstrap_pd2:
    .skip 4096
bootstrap_pd3:
    .skip 4096
bootstrap_pd_high:
    .skip 4096
.section .start.low.data, "aw"
.align 8
bootstrap_boot_info:
    .quad 0
