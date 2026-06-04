/* native_driver.h - Kernel-side loader for native (ring-0 ELF) driver payloads.
 * Native drivers run privileged code directly in kernel address space and use a
 * thin wasmos_native_driver_api_t struct for kernel service access instead of
 * wasm3 hostcalls.  Only WASMOS_APP_FLAG_NATIVE | WASMOS_APP_FLAG_NEEDS_PRIV
 * payloads are loaded through this path. */
#ifndef WASMOS_KERNEL_NATIVE_DRIVER_H
#define WASMOS_KERNEL_NATIVE_DRIVER_H

#include "wasmos_native_driver.h"

/* Load the ELF binary from elf_data/elf_size into context_id's address space,
 * build the driver API struct, and call the driver's initialize() entry point.
 * Returns the value returned by initialize(), or -1 on load failure. */
int native_driver_start(uint32_t context_id,
                        const uint8_t *elf_data, uint32_t elf_size,
                        const char *name,
                        const uint32_t *init_argv, uint32_t init_argc);

/* Return the number of heap bytes committed by the native driver in pid. */
uint64_t native_driver_heap_committed_bytes(uint32_t pid);

/* Release the native driver heap for pid (called on process exit). */
void native_driver_heap_release(uint32_t pid);

#endif
