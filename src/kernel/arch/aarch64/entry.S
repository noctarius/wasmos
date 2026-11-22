/* entry.S - aarch64 kernel entry point (bring-up stub).
 *
 * _start is entered from the UEFI loader after ExitBootServices with
 * x0 = boot_info_t* (AAPCS64 arg0), the MMU on with the firmware's identity
 * map, at EL1 or EL2. This stub masks every exception class and parks the CPU;
 * it neither drops to EL1, installs page tables nor calls kmain.
 *
 * TODO: the EL2->EL1 drop, CPACR_EL1.FPEN (kernel code uses FP/SIMD), stack,
 * BSS and the kmain call (arm64 port, Phase 1). Until then an aarch64 boot
 * stops here silently.
 */
.section .text.start, "ax"
.global _start
_start:
    msr daifset, #0xf
1:
    wfe
    b 1b
