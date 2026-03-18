#ifndef WASMOS_KERNEL_NATIVE_DRIVER_H
#define WASMOS_KERNEL_NATIVE_DRIVER_H

#include "wasmos_native_driver.h"

/*
 * Load the ELF binary from elf_data/elf_size into context_id's address space,
 * build the driver API struct, and call the driver's initialize() entry point.
 * Returns the value returned by initialize(), or -1 on load failure.
 */
int native_driver_start(uint32_t context_id,
                        const uint8_t *elf_data, uint32_t elf_size,
                        const char *name,
                        const uint32_t *init_argv, uint32_t init_argc);

#endif