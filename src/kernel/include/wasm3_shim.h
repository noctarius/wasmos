#ifndef WASMOS_WASM3_SHIM_H
#define WASMOS_WASM3_SHIM_H

#include <stddef.h>
#include <stdint.h>

void wasm3_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size);
uint32_t wasm3_heap_bind_pid(uint32_t pid);
void wasm3_heap_restore_pid(uint32_t previous_pid);
void wasm3_heap_release(uint32_t pid);
int wasm3_heap_probe_growth(size_t size);

#endif
