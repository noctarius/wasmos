/* arch_pci.h - the platform's PCI configuration-space access mechanism.
 * Implemented per architecture (x86_64: arch/x86_64/pci_cfg_x86_64.c, PCI
 * configuration mechanism #1 through ports 0xCF8/0xCFC). pci_config.c owns the
 * locking and everything built on top. */
#pragma once

#include <stdint.h>

/* Read / write the aligned 32-bit configuration register containing `reg`
 * (the low two bits are ignored) of function `bdf` (bus << 8 | device << 3 |
 * function). The caller serialises every access: on x86_64 an access is an
 * address write followed by a data access, and two interleaved accesses read
 * or write the wrong register. */
uint32_t arch_pci_cfg_read32(uint16_t bdf, uint8_t reg);
void arch_pci_cfg_write32(uint16_t bdf, uint8_t reg, uint32_t value);
