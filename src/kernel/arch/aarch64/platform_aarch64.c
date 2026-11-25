/* platform_aarch64.c - QEMU virt platform: the PL011 console UART, and
 * poweroff/reboot through PSCI. Implements arch/arch_platform.h and
 * system_control.h. */
#include <stdint.h>

#include "arch/arch_cpu.h"
#include "arch/arch_mmio.h"
#include "arch/arch_platform.h"
#include "arch/vm_layout.h"
#include "system_control.h"
#include "wasmos_status.h"

/* PL011 at QEMU virt's fixed address, reached through the direct map (the
 * first GiB is mapped Device-nGnRE from entry.S on).
 * TODO: discover the console UART from the ACPI SPCR table. */
#define PL011_PHYS 0x09000000ULL
#define PL011_BASE (KERNEL_HIGHER_HALF_BASE | PL011_PHYS)

/* PL011 register offsets and bits (ARM DDI 0183). */
#define PL011_DR 0x00u
#define PL011_FR 0x18u
#define PL011_IBRD 0x24u
#define PL011_FBRD 0x28u
#define PL011_LCR_H 0x2Cu
#define PL011_CR 0x30u
#define PL011_IMSC 0x38u
#define PL011_ICR 0x44u
#define PL011_FR_RXFE (1u << 4)
#define PL011_FR_TXFF (1u << 5)
#define PL011_LCR_H_FEN (1u << 4)
#define PL011_LCR_H_WLEN8 (3u << 5)
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE (1u << 8)
#define PL011_CR_RXE (1u << 9)
#define PL011_IMSC_RXIM (1u << 4)
#define PL011_IMSC_RTIM (1u << 6)

/* PSCI 0.2 function IDs (ARM DEN 0022, SMC32 convention; CPU_ON is the SMC64
 * form, which takes 64-bit addresses). */
#define PSCI_SYSTEM_OFF 0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u
#define PSCI_CPU_ON_64 0xC4000003u

extern uint32_t aarch64_boot_el;

static uint32_t pl011_read(uint32_t reg) {
    return arch_mmio_read32((uintptr_t)(PL011_BASE + reg));
}

static void pl011_write(uint32_t reg, uint32_t value) {
    arch_mmio_write32((uintptr_t)(PL011_BASE + reg), value);
}

/* 115200 8N1 from QEMU's 24 MHz UART clock (IBRD 13, FBRD 1), FIFOs on,
 * receive and receive-timeout interrupts unmasked. */
void arch_console_uart_init(void) {
    pl011_write(PL011_CR, 0);
    pl011_write(PL011_ICR, 0x7FFu);
    pl011_write(PL011_IBRD, 13u);
    pl011_write(PL011_FBRD, 1u);
    pl011_write(PL011_LCR_H, PL011_LCR_H_FEN | PL011_LCR_H_WLEN8);
    pl011_write(PL011_IMSC, PL011_IMSC_RXIM | PL011_IMSC_RTIM);
    pl011_write(PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
}

int arch_console_uart_tx_ready(void) {
    return (pl011_read(PL011_FR) & PL011_FR_TXFF) == 0;
}

int arch_console_uart_rx_ready(void) {
    return (pl011_read(PL011_FR) & PL011_FR_RXFE) == 0;
}

void arch_console_uart_write(uint8_t byte) {
    pl011_write(PL011_DR, byte);
}

uint8_t arch_console_uart_read(void) {
    return (uint8_t)(pl011_read(PL011_DR) & 0xFFu);
}

/* QEMU virt has a single UART; the kernel log goes to the virtio console. */
int32_t arch_log_uart_enable(void) {
    return WASMOS_NOENT;
}

void arch_log_uart_write(uint8_t byte) {
    (void)byte;
}

/* Issue a PSCI call through the conduit the boot exception level implies and
 * return x0 (a PSCI status: 0 success, negative errors). SMCCC lets the callee
 * clobber x0-x17. A SYSTEM_OFF or SYSTEM_RESET that succeeds does not return. */
static int64_t psci_call(uint32_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x3 __asm__("x3") = arg3;
    if (aarch64_boot_el == 2u) {
        __asm__ volatile("smc #0"
                         : "+r"(x0),
                           "+r"(x1),
                           "+r"(x2),
                           "+r"(x3)::"x4",
                           "x5",
                           "x6",
                           "x7",
                           "x8",
                           "x9",
                           "x10",
                           "x11",
                           "x12",
                           "x13",
                           "x14",
                           "x15",
                           "x16",
                           "x17",
                           "memory");
    } else {
        __asm__ volatile("hvc #0"
                         : "+r"(x0),
                           "+r"(x1),
                           "+r"(x2),
                           "+r"(x3)::"x4",
                           "x5",
                           "x6",
                           "x7",
                           "x8",
                           "x9",
                           "x10",
                           "x11",
                           "x12",
                           "x13",
                           "x14",
                           "x15",
                           "x16",
                           "x17",
                           "memory");
    }
    return (int64_t)x0;
}

/* PSCI CPU_ON: start the CPU with affinity `mpidr` at physical `entry`, MMU
 * off, with `context` in x0. Returns the PSCI status (0 on success). */
int64_t aarch64_psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context) {
    return psci_call(PSCI_CPU_ON_64, mpidr, entry, context);
}

void kernel_system_poweroff(void) {
    arch_irq_disable();
    (void)psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    arch_halt_forever();
}

void kernel_system_reboot(void) {
    arch_irq_disable();
    (void)psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
    arch_halt_forever();
}
