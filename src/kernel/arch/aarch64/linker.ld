/* linker.ld - aarch64 kernel image layout (bring-up stub).
 *
 * The image is linked and loaded at KERNEL_LOAD_BASE, 2 MiB past the start of
 * QEMU virt's RAM (0x40000000), which keeps the device tree QEMU places at the
 * base of RAM intact. The bootloader copies each segment to its p_paddr, so
 * virtual and load addresses are equal here.
 *
 * TODO: link the kernel proper in the TTBR1 half and keep only the bootstrap
 * identity-mapped (arm64 port, Phase 1; see the x86_64 linker.ld for the
 * two-address arrangement the loader already supports).
 *
 * Symbols exported to code: __kernel_start/__kernel_end bound the image;
 * __bss_start/__bss_end delimit what entry zeroes; __stack_top is the initial
 * kernel stack pointer, which grows down into the 64 KiB reserved below it. */
ENTRY(_start)

SECTIONS {
    KERNEL_LOAD_BASE = 0x40200000;

    . = KERNEL_LOAD_BASE;
    __kernel_start = .;

    .text : {
        *(.text.start)
        *(.text*)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
    }

    . = ALIGN(0x1000);
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __stack_top = . + 0x10000;
    __kernel_end = __stack_top;
}
