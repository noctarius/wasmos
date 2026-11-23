/* arch/aarch64/arch_io.h - aarch64 has no I/O-port space. Included only
 * through arch/arch_io.h. Every accessor touches nothing and returns non-zero,
 * which callers report as WASMOS_ERR_IO_NO_PORT_SPACE; a device on this
 * architecture is reached through an MMIO region instead. */
#pragma once

#include <stdint.h>

static inline int arch_port_in8(uint16_t port, uint8_t* out) {
    (void)port;
    *out = 0;
    return 1;
}

static inline int arch_port_in16(uint16_t port, uint16_t* out) {
    (void)port;
    *out = 0;
    return 1;
}

static inline int arch_port_in32(uint16_t port, uint32_t* out) {
    (void)port;
    *out = 0;
    return 1;
}

static inline int arch_port_out8(uint16_t port, uint8_t value) {
    (void)port;
    (void)value;
    return 1;
}

static inline int arch_port_out16(uint16_t port, uint16_t value) {
    (void)port;
    (void)value;
    return 1;
}

static inline int arch_port_out32(uint16_t port, uint32_t value) {
    (void)port;
    (void)value;
    return 1;
}
