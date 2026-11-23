/* linker.ld - aarch64 kernel image layout.
 *
 * The same two-address arrangement as the x86_64 image. The bootstrap is
 * linked and loaded at KERNEL_LOAD_BASE (physical, 2 MiB past the start of
 * QEMU virt's RAM so the device tree QEMU places at the base of RAM stays
 * intact) and holds entry.S's .start.low* sections, which run with the MMU
 * off. The kernel proper is linked at KERNEL_HIGHER_HALF_BASE + its load
 * address and loaded with AT() to that load address, so its program headers
 * carry a higher-half p_vaddr and a physical p_paddr; the loader copies to
 * p_paddr.
 *
 * KERNEL_HIGHER_HALF_BASE must equal the value in
 * include/arch/aarch64/vm_layout.h, and the whole image must lie inside the
 * physical [0, 4 GiB) window entry.S maps.
 *
 * Symbols exported to code: __bootstrap_start/__bootstrap_end bound the
 * bootstrap; __kernel_start/__kernel_end bound the higher-half image (the
 * boot stack included); __bss_start/__bss_end delimit what _start_high zeroes;
 * __stack_top is the initial kernel stack pointer, which grows down into the
 * 64 KiB reserved below it. */
ENTRY(_start)

SECTIONS {
    KERNEL_HIGHER_HALF_BASE = 0xFFFFFF8000000000;
    KERNEL_LOAD_BASE = 0x40200000;

    . = KERNEL_LOAD_BASE;
    __bootstrap_start = .;
    .bootstrap.text : {
        *(.start.low)
    }
    .bootstrap.bss : ALIGN(4096) {
        *(.start.low.bss*)
    }
    __bootstrap_end = .;
    /* 2 MiB-aligned load address for the kernel proper, so no page is shared
     * with the bootstrap. */
    __kernel_lma = (__bootstrap_end + 0x1FFFFF) & ~0x1FFFFF;

    . = KERNEL_HIGHER_HALF_BASE + __kernel_lma;
    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_HIGHER_HALF_BASE) {
        *(.text*)
    }

    . = ALIGN(0x1000);
    .rodata : AT(ADDR(.rodata) - KERNEL_HIGHER_HALF_BASE) {
        *(.rodata*)
    }

    . = ALIGN(0x1000);
    .data : AT(ADDR(.data) - KERNEL_HIGHER_HALF_BASE) {
        *(.data*)
    }

    . = ALIGN(0x1000);
    .bss : AT(ADDR(.bss) - KERNEL_HIGHER_HALF_BASE) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(16);
    __stack_top = . + 0x10000;
    __kernel_end = __stack_top;
}
