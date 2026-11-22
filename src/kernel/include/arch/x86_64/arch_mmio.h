/* arch/x86_64/arch_mmio.h - device-register loads and stores for x86_64.
 * Included only through arch/arch_mmio.h.
 *
 * The page is mapped strong-uncacheable (PCD|PWT under the default PAT), and
 * the processor neither merges, splits, speculates nor reorders UC accesses
 * against other memory accesses (SDM vol. 3, 11.3), so the hardware needs no
 * fence. `volatile` keeps the compiler from eliding, merging or splitting the
 * access, and the "memory" clobber keeps it from moving ordinary loads and
 * stores across it. */
#pragma once

#include <stdint.h>

static inline uint8_t arch_mmio_read8(uintptr_t addr) {
    uint8_t value = *(volatile uint8_t*)addr;
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline uint16_t arch_mmio_read16(uintptr_t addr) {
    uint16_t value = *(volatile uint16_t*)addr;
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline uint32_t arch_mmio_read32(uintptr_t addr) {
    uint32_t value = *(volatile uint32_t*)addr;
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline void arch_mmio_write8(uintptr_t addr, uint8_t value) {
    __asm__ volatile("" ::: "memory");
    *(volatile uint8_t*)addr = value;
}

static inline void arch_mmio_write16(uintptr_t addr, uint16_t value) {
    __asm__ volatile("" ::: "memory");
    *(volatile uint16_t*)addr = value;
}

static inline void arch_mmio_write32(uintptr_t addr, uint32_t value) {
    __asm__ volatile("" ::: "memory");
    *(volatile uint32_t*)addr = value;
}
