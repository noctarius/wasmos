/* entry.S - Kernel entry point and bootstrap page table setup.
 *
 * _start runs at the physical load address (lower half) where the UEFI
 * bootloader jumped.  It builds a minimal two-region identity map (lower 4 GB
 * via 2 MB large pages) and a 64 MB higher-half alias at virtual 0xFFFFFF8000000000
 * so the kernel can relocate to its linked address before calling kmain.
 *
 * After loading CR3 and jumping to _start_high (the linked virtual address),
 * the lower-half identity map remains valid for the entire kernel lifetime as
 * the boot-time physical alias.
 */
.section .start.low, "ax"
.global _start
.extern kmain
.extern __stack_top
.extern __bss_start
.extern __bss_end

.set PT_FLAG_PRESENT, 0x1
.set PT_FLAG_WRITE, 0x2
.set PT_FLAG_LARGE, 0x80
.set HIGHER_HALF_PDPT_INDEX, 510  /* PML4 slot 511 maps the higher-half PDPT */

_start:
    cld
    /* Carry the boot_info pointer (UEFI MS-ABI arg0 = rcx) in a callee-saved
     * register across paging setup AND the BSS clear in _start_high. Storing it
     * to a .data variable is fragile: depending on linker layout that variable
     * can fall inside the __bss_start..__bss_end range that _start_high zeroes,
     * which nulled boot_info under ld.lld (observed in CI). r15 is untouched by
     * the paging helpers and the BSS clear. */
    mov %rcx, %r15

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
    mov %r15, %rdi
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
