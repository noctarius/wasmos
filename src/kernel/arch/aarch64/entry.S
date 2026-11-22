/* entry.S - aarch64 kernel entry point and bootstrap page tables.
 *
 * _start is entered from the UEFI loader after ExitBootServices with
 * x0 = boot_info_t* (AAPCS64 arg0, a physical/identity address), at EL1 or
 * EL2, possibly with the firmware's identity-mapped MMU and caches on. The
 * loader has cleaned every loaded segment to the point of coherency and
 * invalidated the instruction cache, so the image reads correctly with the MMU
 * and caches off.
 *
 * The bootstrap (.start.low*, linked and run at its physical address):
 *   1. masks every exception class;
 *   2. at EL2: turns the EL2 MMU off, makes EL1 AArch64 with the timer and
 *      FP/SIMD untrapped, and drops to EL1h; EL3 entry is not supported;
 *   3. at EL1: turns the firmware's MMU and caches off;
 *   4. enables FP/SIMD at EL1 (CPACR_EL1.FPEN): kernel code uses it;
 *   5. builds one level-1 table mapping physical [0, 4 GiB) as 1 GiB blocks,
 *      the first Device-nGnRE (QEMU virt's devices), the rest Normal
 *      write-back, and installs it as both TTBR0 (identity) and TTBR1 (the
 *      direct map at KERNEL_HIGHER_HALF_BASE, include/arch/aarch64/vm_layout.h);
 *   6. turns the MMU and caches on and jumps to _start_high at its linked
 *      higher-half address.
 * _start_high installs the exception vectors, switches to the kernel stack,
 * zeroes .bss and calls kmain(boot_info). The identity map stays installed as
 * the boot-time physical alias, as on x86_64.
 */
.set SCTLR_EL1_RES1, 0x30d00800
.set SCTLR_M, (1 << 0)
.set SCTLR_C, (1 << 2)
.set SCTLR_I, (1 << 12)
/* MAIR_EL1: index 0 Device-nGnRE, 1 Normal write-back (read/write-allocate),
 * 2 Normal non-cacheable. */
.set MAIR_VALUE, 0x44ff04
/* TCR_EL1: T0SZ = T1SZ = 25 (39-bit VA), 4 KiB granule in both halves,
 * write-back cacheable inner-shareable walks. IPS is filled in from
 * ID_AA64MMFR0_EL1.PARange. */
.set TCR_VALUE, 0xb5193519
/* Level-1 block descriptors: valid block, AF, plus attributes. Device: MAIR
 * index 0, PXN|UXN. Normal: MAIR index 1, inner shareable, UXN. */
.set BLOCK_DEVICE, 0x0060000000000401
.set BLOCK_NORMAL, 0x0040000000000705

.section .start.low, "ax"
.global _start
_start:
    mov x19, x0
    msr daifset, #0xf
    mrs x20, CurrentEL
    lsr x20, x20, #2
    and x20, x20, #3
    cmp x20, #1
    b.eq el1_entry
    cmp x20, #2
    b.eq el2_entry
park_low:
    wfe
    b park_low

el2_entry:
    mrs x0, sctlr_el2
    bic x0, x0, #SCTLR_M
    bic x0, x0, #SCTLR_C
    bic x0, x0, #SCTLR_I
    msr sctlr_el2, x0
    isb
    mov x0, #(1 << 31) /* HCR_EL2.RW: EL1 is AArch64; nothing else traps */
    msr hcr_el2, x0
    mrs x0, cnthctl_el2
    orr x0, x0, #3 /* EL1PCTEN | EL1PCEN: EL1 owns the physical timer */
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    mov x0, #0x33ff /* CPTR_EL2 RES1 bits, TFP clear: no FP/SIMD trap */
    msr cptr_el2, x0
    mrs x0, midr_el1
    msr vpidr_el2, x0
    mrs x0, mpidr_el1
    msr vmpidr_el2, x0
    ldr x0, =SCTLR_EL1_RES1
    msr sctlr_el1, x0
    mov x0, #0x3c5 /* EL1h, DAIF masked */
    msr spsr_el2, x0
    adr x0, el1_entry
    msr elr_el2, x0
    eret

el1_entry:
    mrs x0, sctlr_el1
    bic x0, x0, #SCTLR_M
    bic x0, x0, #SCTLR_C
    bic x0, x0, #SCTLR_I
    msr sctlr_el1, x0
    isb
    ic iallu
    dsb nsh
    isb

    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    /* Fill the level-1 table. The MMU is off, so these stores reach memory
     * directly; the invalidate below drops any stale cached copy of the lines
     * before the table walker, which reads through the cache, sees them. */
    adrp x0, bootstrap_l1
    mov x1, x0
    mov x2, #512
1:
    str xzr, [x1], #8
    subs x2, x2, #1
    b.ne 1b
    ldr x1, =BLOCK_DEVICE
    str x1, [x0]
    ldr x1, =BLOCK_NORMAL
    mov x2, #1
2:
    orr x3, x1, x2, lsl #30
    str x3, [x0, x2, lsl #3]
    add x2, x2, #1
    cmp x2, #4
    b.ne 2b

    mrs x3, ctr_el0
    ubfx x3, x3, #16, #4 /* DminLine: log2(words) of the smallest D-line */
    mov x4, #4
    lsl x3, x4, x3
    add x2, x0, #4096
    mov x1, x0
3:
    dc ivac, x1
    add x1, x1, x3
    cmp x1, x2
    b.lo 3b
    dsb sy

    ldr x1, =MAIR_VALUE
    msr mair_el1, x1
    ldr x1, =TCR_VALUE
    mrs x2, id_aa64mmfr0_el1
    and x2, x2, #0xf
    cmp x2, #5 /* IPS 0b110 (52-bit) needs FEAT_LPA; cap at 48 bits */
    mov x3, #5
    csel x2, x2, x3, ls
    orr x1, x1, x2, lsl #32
    msr tcr_el1, x1
    msr ttbr0_el1, x0
    msr ttbr1_el1, x0
    isb
    tlbi vmalle1
    dsb nsh
    isb

    ldr x0, =(SCTLR_EL1_RES1 | SCTLR_M | SCTLR_C | SCTLR_I)
    msr sctlr_el1, x0
    isb
    ldr x0, =_start_high
    br x0

.section .start.low.bss, "aw", %nobits
.balign 4096
bootstrap_l1:
    .space 4096

.section .text
.global _start_high
.extern kmain
.extern aarch64_vectors
.extern aarch64_boot_el
_start_high:
    ldr x0, =aarch64_vectors
    msr vbar_el1, x0
    isb
    ldr x0, =__stack_top
    mov sp, x0
    mov x29, xzr
    mov x30, xzr
    ldr x0, =__bss_start
    ldr x1, =__bss_end
4:
    cmp x0, x1
    b.hs 5f
    str xzr, [x0], #8
    b 4b
5:
    ldr x0, =aarch64_boot_el
    str w20, [x0]
    mov x0, x19
    bl kmain
6:
    msr daifset, #0xf
    wfi
    b 6b
