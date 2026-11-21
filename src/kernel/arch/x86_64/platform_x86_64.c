/* platform_x86_64.c - x86_64 PC platform: the 16550 console UART (COM1), the
 * kernel-log UART (COM2), and poweroff/reboot through the legacy control ports.
 * Implements arch/arch_platform.h and system_control.h. */
#include "arch/arch_platform.h"
#include "arch/arch_cpu.h"
#include "io.h"
#include "system_control.h"
#include "wasmos_status.h"

#define COM1_PORT 0x3F8u
#define COM2_PORT 0x2F8u

/* 16550 register offsets and the line-status bits used here. */
#define UART_DATA 0u
#define UART_IER 1u
#define UART_FCR 2u
#define UART_LCR 3u
#define UART_MCR 4u
#define UART_LSR 5u
#define UART_SCRATCH 7u
#define UART_LSR_RX_READY 0x01u
#define UART_LSR_THR_EMPTY 0x20u

void arch_console_uart_init(void) {
    outb(COM1_PORT + UART_IER, 0x00);  /* interrupts off while programming */
    outb(COM1_PORT + UART_LCR, 0x80);  /* DLAB */
    outb(COM1_PORT + UART_DATA, 0x01); /* divisor low: 115200 baud */
    outb(COM1_PORT + UART_IER, 0x00);  /* divisor high */
    outb(COM1_PORT + UART_LCR, 0x03);  /* 8N1 */
    outb(COM1_PORT + UART_FCR, 0xC7);  /* FIFO on, cleared, 14-byte threshold */
    outb(COM1_PORT + UART_MCR, 0x0B);  /* DTR + RTS, OUT2: IRQ line enabled */
}

int arch_console_uart_tx_ready(void) {
    return (inb(COM1_PORT + UART_LSR) & UART_LSR_THR_EMPTY) != 0;
}

int arch_console_uart_rx_ready(void) {
    return (inb(COM1_PORT + UART_LSR) & UART_LSR_RX_READY) != 0;
}

void arch_console_uart_write(uint8_t byte) {
    outb(COM1_PORT + UART_DATA, byte);
}

uint8_t arch_console_uart_read(void) {
    return inb(COM1_PORT + UART_DATA);
}

/* A UART is present when its scratch register holds a written value. */
int32_t arch_log_uart_enable(void) {
    outb(COM2_PORT + UART_SCRATCH, 0x5A);
    if (inb(COM2_PORT + UART_SCRATCH) != 0x5A) {
        return WASMOS_NOENT;
    }
    outb(COM2_PORT + UART_IER, 0x00);  /* interrupts off */
    outb(COM2_PORT + UART_LCR, 0x80);  /* DLAB */
    outb(COM2_PORT + UART_DATA, 0x01); /* divisor low: 115200 baud */
    outb(COM2_PORT + UART_IER, 0x00);  /* divisor high */
    outb(COM2_PORT + UART_LCR, 0x03);  /* 8N1 */
    outb(COM2_PORT + UART_FCR, 0xC7);  /* FIFO on, cleared, 14-byte threshold */
    outb(COM2_PORT + UART_MCR, 0x03);  /* DTR + RTS, OUT2 clear: no IRQ */
    return 0;
}

void arch_log_uart_write(uint8_t byte) {
    for (uint32_t spin = 0; spin < 100000u && (inb(COM2_PORT + UART_LSR) & UART_LSR_THR_EMPTY) == 0;
         ++spin) {
    }
    outb(COM2_PORT + UART_DATA, byte);
}

/* Powers the machine off and does not return.  Interrupts are masked first, then
 * the three ACPI-ish shutdown ports are written in turn — QEMU's newer 0x604,
 * its older 0xB004, and VirtualBox's 0x4004 — because the platform is not probed
 * and only one of them will be listened to.  On hardware that answers none of
 * them the CPU ends up halted with interrupts off instead. */
void kernel_system_poweroff(void) {
    arch_irq_disable();

    outw(0x604u, 0x2000u);
    io_wait();
    outw(0xB004u, 0x2000u);
    io_wait();
    outw(0x4004u, 0x3400u);
    io_wait();

    arch_halt_forever();
}

/* Resets the machine and does not return.  Tries the keyboard-controller pulse
 * (0x64) first and then both PCI reset-control forms (0xCF9), for the same
 * reason poweroff writes several ports.  Falls back to a halt with interrupts
 * masked when none of them takes effect. */
void kernel_system_reboot(void) {
    arch_irq_disable();

    outb(0x64u, 0xFEu);
    io_wait();
    outb(0xCF9u, 0x06u);
    io_wait();
    outb(0xCF9u, 0x0Eu);
    io_wait();

    arch_halt_forever();
}
