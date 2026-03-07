#include "serial.h"

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static int serial_tx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); // Disable interrupts
    outb(COM1_PORT + 3, 0x80); // Enable DLAB
    outb(COM1_PORT + 0, 0x01); // Divisor low (115200 baud)
    outb(COM1_PORT + 1, 0x00); // Divisor high
    outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop
    outb(COM1_PORT + 2, 0xC7); // Enable FIFO, clear, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

void serial_write(const char *s) {
    if (!s) {
        return;
    }
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            while (!serial_tx_ready()) {
            }
            outb(COM1_PORT, '\r');
        }
        while (!serial_tx_ready()) {
        }
        outb(COM1_PORT, (uint8_t)*p);
    }
}
