/* Host shadow of src/kernel/include/io.h for tests/unit/test_pci_config.c: the
 * port accessors are plain functions the test defines over an emulated device,
 * instead of IN/OUT instructions. Only the widths arch/x86_64/pci_cfg_x86_64.c
 * uses exist. Kept
 * out of tests/unit/include so no other suite picks it up in place of the real
 * header. */
#ifndef WASMOS_IO_H
#define WASMOS_IO_H

#include <stdint.h>

void outl(uint16_t port, uint32_t value);
uint32_t inl(uint16_t port);

#endif
