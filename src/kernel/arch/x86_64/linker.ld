/* linker.ld - kernel image layout.
 *
 * The image is split in two. The bootstrap part is linked and loaded at
 * KERNEL_LOAD_BASE (1 MiB physical) and holds entry.S's .start.low* sections,
 * which must run identity-mapped because they build the first page tables. The
 * kernel proper is linked at KERNEL_HIGHER_HALF_BASE + its load address and
 * loaded with AT() to that load address, so the ELF program headers carry a
 * higher-half p_vaddr and a low p_paddr — and the bootloader copies to p_paddr,
 * which is what makes this two-address arrangement work before paging exists.
 *
 * The two bases must stay in step with entry.S: it maps the low 4 GiB identity
 * plus a 64 MiB window at KERNEL_HIGHER_HALF_BASE, so the whole kernel image has
 * to fit inside that window, and KERNEL_HIGHER_HALF_BASE in paging.h must be the
 * same constant.
 *
 * Symbols exported to code: __bootstrap_start/__bootstrap_end bound the
 * identity-mapped bootstrap; __kernel_start/__kernel_end bound the higher-half
 * image and are used to decide whether an address is kernel text;
 * __bss_start/__bss_end delimit what _start_high zeroes; __stack_top is the
 * initial kernel stack pointer, which grows down into the 64 KiB reserved just
 * below it. */
ENTRY(_start)

SECTIONS {
    KERNEL_HIGHER_HALF_BASE = 0xFFFFFFFF80000000;
    KERNEL_LOAD_BASE = 0x100000;

    . = KERNEL_LOAD_BASE;
    __bootstrap_start = .;
    .bootstrap.text : {
        *(.start.low*)
    }
    .bootstrap.data : {
        *(.start.low.data*)
    }
    .bootstrap.bss : {
        *(.start.low.bss*)
    }
    __bootstrap_end = .;
    /* Round the kernel's load address up to 2 MiB so it starts on a large-page
     * boundary of the identity map entry.S installs, and so no page is shared
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
        __bss_end = .;
    }

    /* 64 KiB of boot stack carved out past .bss. It is reserved by address
     * only: no section covers it, so nothing zeroes or allocates it, and
     * __kernel_end deliberately includes it so the range checks that use
     * __kernel_start.. __kernel_end treat the stack as part of the image. */
    . = ALIGN(16);
    __stack_top = . + 0x10000;
    __kernel_end = __stack_top;
}
