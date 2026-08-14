/* entry.S - Kernel entry point and bootstrap page table setup.
 *
 * _start runs at the physical load address (lower half) where the UEFI
 * bootloader jumped.  It builds a minimal two-region map: the lower 4 GB
 * identity-mapped with 2 MB large pages, plus a 64 MB alias of physical 0 at
 * virtual 0xFFFFFFFF80000000 (PML4 slot 511, PDPT slot 510), which is the
 * KERNEL_HIGHER_HALF_BASE the kernel is linked against, so it can jump to its
 * linked addresses before calling kmain.
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
/* Slot 510 of the PDPT hung off PML4 slot 511: 511*512GB + 510*1GB gives
 * virtual 0xFFFFFFFF80000000. */
.set HIGHER_HALF_PDPT_INDEX, 510

/* _start - kernel entry, called by the UEFI loader through an ms_abi function
 * pointer immediately after ExitBootServices.
 *
 * Entry state: rcx = boot_info_t* (MS x64 arg0), long mode active on the
 * firmware's page tables and GDT, rsp on a firmware-provided stack.  Nothing
 * else is defined: no kernel stack, no BSS, no IDT, and no interrupt handler for
 * anything the firmware may have left unmasked.
 * Exit: does not return.  Ends with an absolute jump to _start_high at the
 * kernel's linked higher-half address, which calls kmain(boot_info).
 * Clobbers every register; the loader expects no callee-saved state back.
 *
 * Preserves rcx by moving it to r15, which no code between here and the kmain
 * call touches.  Runs on the firmware stack until _start_high switches to
 * __stack_top, so this routine must not recurse or allocate. */
_start:
    cld
    /* Carry the boot_info pointer (UEFI MS-ABI arg0 = rcx) in a callee-saved
     * register across paging setup AND the BSS clear in _start_high. A memory
     * slot cannot hold it: depending on linker layout the slot can fall inside
     * the __bss_start..__bss_end range that _start_high zeroes, which nulls
     * boot_info under ld.lld. r15 is untouched by the paging helpers and by the
     * BSS clear. */
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

/* fill_pd_2m_identity[_with_base] - fill one 512-entry page directory with
 * consecutive present/writable 2 MB large-page mappings.
 * Entry: rdi = page-directory base (written, 4096 bytes), rax = first physical
 * address (the _with_base entry point; the plain entry starts at 0).
 * Exit: rdi advanced past the table, rax past the last mapped frame, rcx zero.
 * Preserves nothing else and touches no memory outside [rdi, rdi+4096).
 * Runs on the firmware stack, so it uses only a call/ret frame. */
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
/* _start_high - first code to execute at the kernel's linked addresses.
 *
 * Entry: reached only by the absolute jump at the end of _start, with the
 * bootstrap PML4 in CR3 and r15 holding boot_info_t*.  No other register is
 * live and rsp still points at the firmware stack.
 * Exit: does not return.  Installs __stack_top as rsp, zeroes
 * __bss_start..__bss_end, moves boot_info into rdi (SysV arg0) and calls kmain;
 * if kmain ever returns, halts in a cli-less hlt loop.
 *
 * r15 must survive the BSS clear, which is why boot_info is carried in a
 * register and not in a memory slot: a slot inside the zeroed range would be
 * erased here. */
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

/* Bootstrap paging structures, one 4 KiB-aligned table each.  They live in a
 * lower-half nobits section because CR3 and the table entries hold physical
 * addresses and this code runs before any higher-half mapping exists.  The three
 * upper-level tables (pml4, pdpt_low, pdpt_high) are cleared explicitly at the
 * top of _start before their few live entries are written.
 * pd0..pd3 cover the identity-mapped first 4 GB; pd_high covers the 64 MB
 * higher-half alias.  They stay live for the kernel's whole lifetime as the
 * boot-time physical alias, so nothing may reuse this memory. */
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
/* TODO: unreferenced — boot_info travels to kmain in r15, so this slot can go. */
bootstrap_boot_info:
    .quad 0
