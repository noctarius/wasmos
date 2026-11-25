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
 *      write-back, and hangs it below two level-0 tables: TTBR0 slot 0 (the
 *      identity map) and TTBR1 slot 511 (the direct map at
 *      KERNEL_HIGHER_HALF_BASE, include/arch/aarch64/vm_layout.h);
 *   6. turns the MMU and caches on and jumps to _start_high at its linked
 *      higher-half address.
 * _start_high installs the exception vectors, switches to the kernel stack,
 * zeroes .bss and calls kmain(boot_info). The identity map stays installed as
 * the boot-time physical alias, as on x86_64.
 *
 * _start_secondary is where PSCI CPU_ON starts a secondary CPU (WASMOS_SMP):
 * MMU off, at its physical address, x0 = the physical address of the
 * smp_boot_block_t smp_aarch64.c filled and cleaned to the point of coherency.
 * It takes the same exception-level path, loads the kernel's own TTBR0/TTBR1
 * (built by paging_init, not the bootstrap tables) with the same MAIR and TCR,
 * turns the MMU on and calls smp_ap_c_entry(cpu_id) on its own stack.
 */
.set SCTLR_EL1_RES1, 0x30d00800
.set SCTLR_M, (1 << 0)
.set SCTLR_C, (1 << 2)
.set SCTLR_I, (1 << 12)
/* MAIR_EL1: index 0 Device-nGnRE, 1 Normal write-back (read/write-allocate),
 * 2 Normal non-cacheable. */
.set MAIR_VALUE, 0x44ff04
/* TCR_EL1: T0SZ = T1SZ = 16 (48-bit VA, four levels from level 0), 4 KiB
 * granule in both halves, write-back cacheable inner-shareable walks. IPS is
 * filled in from ID_AA64MMFR0_EL1.PARange. */
.set TCR_VALUE, 0xb5103510
.set TABLE_DESC, 0x3
/* Level-1 block descriptors: valid block, AF, plus attributes. Device: MAIR
 * index 0, PXN|UXN. Normal: MAIR index 1, inner shareable, UXN. */
.set BLOCK_DEVICE, 0x0060000000000401
.set BLOCK_NORMAL, 0x0040000000000705

/* From EL2: EL2 MMU off, EL1 AArch64 with the physical timer and FP/SIMD
 * untrapped, then ERET to \target at EL1h with DAIF masked. Clobbers x0. */
.macro drop_el2_to_el1 target
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
    adr x0, \target
    msr elr_el2, x0
    eret
.endm

/* At EL1: the firmware's MMU and caches off, stale instructions dropped, and
 * FP/SIMD enabled (kernel code uses it). Clobbers x0. */
.macro el1_mmu_off_fp_on
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
.endm

/* MAIR_EL1 and TCR_EL1 (IPS from ID_AA64MMFR0_EL1.PARange, capped at 48 bits).
 * Clobbers x1-x3. */
.macro set_mair_tcr
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
.endm

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
    drop_el2_to_el1 el1_entry

el1_entry:
    el1_mmu_off_fp_on

    /* Fill the three tables (level-0 low, level-0 high, level-1; contiguous).
     * The MMU is off, so these stores reach memory directly; the invalidate
     * below drops any stale cached copy of the lines before the table walker,
     * which reads through the cache, sees them. */
    adrp x5, bootstrap_l0_lo
    mov x1, x5
    mov x2, #(3 * 512)
1:
    str xzr, [x1], #8
    subs x2, x2, #1
    b.ne 1b
    add x0, x5, #8192 /* bootstrap_l1 */
    orr x1, x0, #TABLE_DESC
    str x1, [x5]               /* TTBR0 L0[0] -> L1: identity */
    add x6, x5, #4096          /* bootstrap_l0_hi */
    str x1, [x6, #(511 * 8)]   /* TTBR1 L0[511] -> L1: direct map */
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
    add x2, x5, #(3 * 4096)
    mov x1, x5
3:
    dc ivac, x1
    add x1, x1, x3
    cmp x1, x2
    b.lo 3b
    dsb sy

    set_mair_tcr
    msr ttbr0_el1, x5
    msr ttbr1_el1, x6
    isb
    tlbi vmalle1
    dsb nsh
    isb

    ldr x0, =(SCTLR_EL1_RES1 | SCTLR_M | SCTLR_C | SCTLR_I)
    msr sctlr_el1, x0
    isb
    ldr x0, =_start_high
    br x0

#if WASMOS_SMP
/* Secondary CPU entry (PSCI CPU_ON). x0 = physical smp_boot_block_t:
 *   +0 TTBR0 (kernel root, identity map of the low 4 GiB)
 *   +8 TTBR1 (kernel higher half)
 *  +16 initial SP (kernel virtual address)
 *  +24 cpu_id
 * The block stays reachable after the MMU is on through TTBR0's identity map. */
.global _start_secondary
_start_secondary:
    mov x19, x0
    msr daifset, #0xf
    mrs x20, CurrentEL
    lsr x20, x20, #2
    and x20, x20, #3
    cmp x20, #1
    b.eq secondary_el1
    cmp x20, #2
    b.eq secondary_el2
    b park_low

secondary_el2:
    drop_el2_to_el1 secondary_el1

secondary_el1:
    el1_mmu_off_fp_on
    set_mair_tcr
    ldr x1, [x19]
    ldr x2, [x19, #8]
    msr ttbr0_el1, x1
    msr ttbr1_el1, x2
    isb
    tlbi vmalle1
    dsb nsh
    isb
    ldr x0, =(SCTLR_EL1_RES1 | SCTLR_M | SCTLR_C | SCTLR_I)
    msr sctlr_el1, x0
    isb
    ldr x0, =_secondary_high
    br x0
#endif /* WASMOS_SMP */

.section .start.low.bss, "aw", %nobits
.balign 4096
bootstrap_l0_lo:
    .space 4096
bootstrap_l0_hi:
    .space 4096
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

#if WASMOS_SMP
/* _start_secondary's physical (link) address as a 64-bit word: higher-half C
 * cannot form the address of a low symbol with adrp, which reaches +/-4 GiB. */
.section .rodata
.balign 8
.global aarch64_secondary_entry_phys
aarch64_secondary_entry_phys:
    .quad _start_secondary

.section .text
.extern smp_ap_c_entry
_secondary_high:
    ldr x0, =aarch64_vectors
    msr vbar_el1, x0
    isb
    ldr x1, [x19, #16]
    mov sp, x1
    mov x29, xzr
    mov x30, xzr
    ldr x0, [x19, #24]
    bl smp_ap_c_entry
7:
    msr daifset, #0xf
    wfi
    b 7b
#endif /* WASMOS_SMP */
