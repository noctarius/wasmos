/* arch/aarch64/arch_mmio.h - device-register loads and stores for aarch64.
 * Included only through arch/arch_mmio.h.
 *
 * The page is mapped Device-nGnRE, which keeps accesses to it unmerged and in
 * order with each other, but not with Normal-memory accesses (ARM ARM B2.8).
 * A DMB OSH on each side orders the access against every other memory access
 * of the calling CPU, so a doorbell write cannot overtake the descriptor
 * stores before it and a status read cannot be satisfied before the stores
 * that preceded it in program order.
 *
 * Each access is one plain LDR/STR with a base-register address, written as
 * inline assembly rather than a volatile dereference: a hypervisor emulating
 * the register (HVF, KVM) decodes the access from the fault syndrome, which is
 * only valid for single-register loads and stores without writeback, and the
 * compiler is free to pick a pair or post-indexed form for a plain pointer. */
#pragma once

#include <stdint.h>

static inline uint8_t arch_mmio_read8(uintptr_t addr) {
    uint32_t value;
    __asm__ volatile("dmb osh\n"
                     "ldrb %w0, [%1]\n"
                     "dmb osh"
                     : "=r"(value)
                     : "r"(addr)
                     : "memory");
    return (uint8_t)value;
}

static inline uint16_t arch_mmio_read16(uintptr_t addr) {
    uint32_t value;
    __asm__ volatile("dmb osh\n"
                     "ldrh %w0, [%1]\n"
                     "dmb osh"
                     : "=r"(value)
                     : "r"(addr)
                     : "memory");
    return (uint16_t)value;
}

static inline uint32_t arch_mmio_read32(uintptr_t addr) {
    uint32_t value;
    __asm__ volatile("dmb osh\n"
                     "ldr %w0, [%1]\n"
                     "dmb osh"
                     : "=r"(value)
                     : "r"(addr)
                     : "memory");
    return value;
}

static inline void arch_mmio_write8(uintptr_t addr, uint8_t value) {
    __asm__ volatile("dmb osh\n"
                     "strb %w0, [%1]\n"
                     "dmb osh" ::"r"((uint32_t)value),
                     "r"(addr)
                     : "memory");
}

static inline void arch_mmio_write16(uintptr_t addr, uint16_t value) {
    __asm__ volatile("dmb osh\n"
                     "strh %w0, [%1]\n"
                     "dmb osh" ::"r"((uint32_t)value),
                     "r"(addr)
                     : "memory");
}

static inline void arch_mmio_write32(uintptr_t addr, uint32_t value) {
    __asm__ volatile("dmb osh\n"
                     "str %w0, [%1]\n"
                     "dmb osh" ::"r"(value),
                     "r"(addr)
                     : "memory");
}
