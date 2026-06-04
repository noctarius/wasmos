/* msr.h - Inline rdmsr/wrmsr helpers for x86_64 Model Specific Registers. */
#pragma once

#include <stdint.h>

static inline uint64_t
x86_read_msr(uint32_t msr)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void
x86_write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
