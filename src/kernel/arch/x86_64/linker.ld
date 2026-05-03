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

    . = ALIGN(16);
    __stack_top = . + 0x10000;
    __kernel_end = __stack_top;
}
