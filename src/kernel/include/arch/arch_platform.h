/* arch_platform.h - the platform's console and kernel-log UARTs. Implemented
 * per architecture (x86_64: arch/x86_64/platform_x86_64.c, 16550s at COM1 and
 * COM2). The platform also implements system_control.h (poweroff/reboot).
 *
 * The console UART carries the kernel console and serial input; serial.c owns
 * everything above the byte level. None of these take a lock. */
#pragma once

#include <stdint.h>

/* Program the console UART: 115200 8N1, FIFO on, receive interrupts enabled.
 * Safe to call again; it only rewrites the UART registers. */
void arch_console_uart_init(void);

/* Non-zero when the console UART can accept a byte / has a byte to read. */
int arch_console_uart_tx_ready(void);
int arch_console_uart_rx_ready(void);

/* Write one byte (the caller has seen tx_ready) / read one byte (the caller
 * has seen rx_ready). */
void arch_console_uart_write(uint8_t byte);
uint8_t arch_console_uart_read(void);

/* Probe and program the kernel-log UART (115200 8N1, FIFO on, interrupts off).
 * Returns 0, or WASMOS_NOENT when no UART answers, leaving it untouched. */
int32_t arch_log_uart_enable(void);

/* Write one byte to the kernel-log UART. The wait for transmit-ready is
 * bounded: a UART that stops draining drops bytes rather than wedging every
 * kernel-log writer behind it. Only after arch_log_uart_enable returned 0. */
void arch_log_uart_write(uint8_t byte);
