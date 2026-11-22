/* arch/x86_64/arch_io.h - I/O-port access for x86_64, which always has a port
 * space. Included only through arch/arch_io.h; each accessor is one IN or OUT
 * instruction (arch/x86_64/io.h) and always succeeds. */
#pragma once

#include <stdint.h>

#include "arch/x86_64/io.h"

static inline int arch_port_in8(uint16_t port, uint8_t* out) {
    *out = inb(port);
    return 0;
}

static inline int arch_port_in16(uint16_t port, uint16_t* out) {
    *out = inw(port);
    return 0;
}

static inline int arch_port_in32(uint16_t port, uint32_t* out) {
    *out = inl(port);
    return 0;
}

static inline int arch_port_out8(uint16_t port, uint8_t value) {
    outb(port, value);
    return 0;
}

static inline int arch_port_out16(uint16_t port, uint16_t value) {
    outw(port, value);
    return 0;
}

static inline int arch_port_out32(uint16_t port, uint32_t value) {
    outl(port, value);
    return 0;
}
