#ifndef WASMOS_WASM3_SHIM_H
#define WASMOS_WASM3_SHIM_H

#include <stdint.h>

uint32_t wasm3_heap_bind_pid(uint32_t pid);
void wasm3_heap_restore_pid(uint32_t previous_pid);
void wasm3_heap_release(uint32_t pid);

#endif
