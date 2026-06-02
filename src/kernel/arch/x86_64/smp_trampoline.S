/*
 * AP startup trampoline — executed by each Application Processor immediately
 * after receiving a SIPI (Startup IPI).
 *
 * The BSP copies this code to physical address 0x1000 before sending SIPI
 * (SIPI vector 0x01 → startup address 0x1000).  The BSP also fills a set of
 * data slots at physical 0x500–0x521 that the trampoline reads en route to
 * long mode.
 *
 * Data slot layout (physical addresses, pre-filled by smp_trampoline_setup()):
 *   0x500  uint64  AP_CR3      — physical address of the kernel PML4
 *   0x508  uint64  AP_ENTRY    — virtual address of smp_ap_c_entry()
 *   0x510  uint64  AP_RSP      — virtual address of the AP's initial stack top
 *   0x518  uint16  AP_GDTR_LIM — temporary GDT limit (5*8-1 = 39)
 *   0x51A  uint32  AP_GDTR_BASE— physical address of smp_trampoline_gdt
 *   0x51E  uint32  AP_CPU_ID   — logical CPU ID for this AP
 *
 * Transition sequence:
 *   16-bit real mode  → 32-bit protected mode (via far jmp after LGDT+CR0.PE)
 *   32-bit PM         → 64-bit long mode       (via far jmp after CR3+PAE+EFER+PG)
 *   64-bit LM         → call smp_ap_c_entry(cpu_id)
 *
 * All physical addresses in the data slots and trampoline fit in 32 bits
 * (trampoline is at 0x1000, kernel PML4 is in low physical memory).
 * After enabling long mode the AP uses the BSP's page table which maps
 * the identity-mapped low pages (set up by smp_trampoline_setup) as well
 * as the full kernel higher-half.
 */

#define TRAMP_BASE      0x1000

/* Data slot physical addresses. */
#define AP_CR3          0x500
#define AP_ENTRY        0x508
#define AP_RSP          0x510
#define AP_GDTR         0x518   /* 6-byte GDTR descriptor */
#define AP_CPU_ID       0x51E

/* Temporary GDT selector assignments. */
#define SEL_KCODE32     0x08
#define SEL_KDATA32     0x10
#define SEL_KCODE64     0x18
#define SEL_KDATA64     0x20

.section .text.smp_trampoline, "ax", @progbits

.global smp_trampoline_start
.global smp_trampoline_gdt
.global smp_trampoline_end

/* ---------------------------------------------------------------- real mode */
.code16
smp_trampoline_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* Load the temporary GDT (base+limit pre-written to AP_GDTR by BSP). */
    lgdt    AP_GDTR

    /* Enable protected mode (CR0.PE). */
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    /* Far jump: flush pipeline and enter 32-bit protected mode.
     * Target physical address = TRAMP_BASE + offset-of-tramp_pm32. */
    .byte 0xea                                          /* far jmp opcode */
    .word (tramp_pm32 - smp_trampoline_start + TRAMP_BASE)
    .word SEL_KCODE32

/* --------------------------------------------------------- 32-bit prot mode */
.align 4
.code32
tramp_pm32:
    movw    $SEL_KDATA32, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* Load CR3 from data slot (paging off — absolute physical access). */
    movl    AP_CR3, %eax
    movl    %eax, %cr3

    /* Enable PAE (CR4.PAE = bit 5). */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    /* Enable IA32_EFER.LME (bit 8) and .NXE (bit 11). */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $((1 << 8) | (1 << 11)), %eax
    wrmsr

    /* Enable paging (CR0.PG = bit 31). */
    movl    %cr0, %eax
    orl     $(1 << 31), %eax
    movl    %eax, %cr0

    /* Far jump to 64-bit long mode.
     * AP_GDTR_BASE + (tramp_lm64 - smp_trampoline_start) would also work,
     * but since the trampoline is at TRAMP_BASE we can use the same formula. */
    .byte 0xea
    .long (tramp_lm64 - smp_trampoline_start + TRAMP_BASE)
    .word SEL_KCODE64

/* ---------------------------------------------------------- 64-bit long mode */
.align 8
.code64
tramp_lm64:
    /* Set up 64-bit data segments. */
    movw    $SEL_KDATA64, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /*
     * Data slots at 0x500 are accessible here because smp_trampoline_setup()
     * identity-mapped physical page 0x0000 in the kernel's page table before
     * sending SIPI.  Use register-relative addressing to avoid ambiguous
     * encoding of small absolute addresses in 64-bit mode.
     */
    movq    $AP_RSP,   %r10
    movq    (%r10),    %rsp        /* load the AP's initial stack pointer */

    movq    $AP_CPU_ID, %r10
    movl    (%r10),    %edi        /* first argument: logical cpu_id (uint32_t) */

    movq    $AP_ENTRY, %r10
    movq    (%r10),    %rax        /* load smp_ap_c_entry function pointer */

    xorq    %rbp, %rbp
    call    *%rax                  /* call smp_ap_c_entry(cpu_id) */

    /* Should never return; halt if it does. */
0:  cli
    hlt
    jmp     0b

/* ---- Temporary GDT: 5 entries, 40 bytes --------------------------------- */
.align 16
smp_trampoline_gdt:
    .quad 0x0000000000000000   /* [0] null */
    .quad 0x00CF9A000000FFFF   /* [1] kcode32: 32-bit DPL=0 code */
    .quad 0x00CF92000000FFFF   /* [2] kdata32: 32-bit DPL=0 data */
    .quad 0x00AF9A000000FFFF   /* [3] kcode64: 64-bit DPL=0 code */
    .quad 0x00AF92000000FFFF   /* [4] kdata64: 64-bit DPL=0 data */
smp_trampoline_gdt_end:

smp_trampoline_end:
