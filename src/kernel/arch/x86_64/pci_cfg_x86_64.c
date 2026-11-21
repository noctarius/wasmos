/* pci_cfg_x86_64.c - PCI configuration mechanism #1 (PCI Local Bus 3.0
 * §3.2.2.3.2): the address is latched through port 0xCF8, the dword moves
 * through 0xCFC. Implements arch/arch_pci.h. Kept free of other kernel
 * dependencies so tests/unit/test_pci_config.c can compile it against an
 * emulated port space. */
#include "arch/arch_pci.h"
#include "io.h"

#define PCI_CFG_ADDR_PORT 0xCF8u
#define PCI_CFG_DATA_PORT 0xCFCu

static uint32_t pci_cfg_address(uint16_t bdf, uint8_t reg) {
    return 0x80000000u | ((uint32_t)bdf << 8) | ((uint32_t)reg & 0xFCu);
}

uint32_t arch_pci_cfg_read32(uint16_t bdf, uint8_t reg) {
    outl((uint16_t)PCI_CFG_ADDR_PORT, pci_cfg_address(bdf, reg));
    return inl((uint16_t)PCI_CFG_DATA_PORT);
}

void arch_pci_cfg_write32(uint16_t bdf, uint8_t reg, uint32_t value) {
    outl((uint16_t)PCI_CFG_ADDR_PORT, pci_cfg_address(bdf, reg));
    outl((uint16_t)PCI_CFG_DATA_PORT, value);
}
