/* io.h - x86 I/O port access helpers (in/out byte/word/dword).
 * io_wait() issues a write to port 0x80 as a short delay after PIC/PIT operations. */
#ifndef WASMOS_IO_H
#define WASMOS_IO_H

#include <stdint.h>

/* Direct IN/OUT instructions in 8-, 16- and 32-bit widths.  All are ring-0 only (ring 3
 * is denied by the TSS I/O permission bitmap) and none is serialising, so a device that
 * needs settling time between accesses needs an explicit io_wait.  Port numbers are not
 * validated: these reach whatever device is decoded at that address. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Stall roughly one bus cycle by writing the unused POST-code port 0x80, the traditional
 * delay between two accesses to a slow legacy device (8259, 8253).  The delay is an
 * approximation of the hardware's requirement, not a measured interval. */
static inline void io_wait(void) {
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0x80));
}

#endif
