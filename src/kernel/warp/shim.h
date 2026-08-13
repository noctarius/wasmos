/* warp/shim.h - Kernel-side ABI shim declarations for the WARP JIT runtime.
 *
 * Mirrors the interface exposed by wasm3/shim.h so callers can be switched
 * between runtimes via WASMOS_WASM_RUNTIME without changing call sites.
 * The bodies live in warp/shim.cpp; the C++ ABI they depend on (operator
 * new/delete in warp/shim.cpp, the __cxa_* stubs in warp/cxx_abi.cpp) is part
 * of the same runtime block. */
#ifndef WASMOS_WARP_SHIM_H
#define WASMOS_WARP_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void warp_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size);
uint32_t warp_heap_bind_pid(uint32_t pid);
void warp_heap_restore_pid(uint32_t previous_pid);
uint32_t warp_runtime_enter(uint32_t pid);
void warp_runtime_leave(uint32_t previous_pid);
void warp_heap_release(uint32_t pid);
uint64_t warp_heap_committed_bytes(uint32_t pid);
int warp_heap_probe_growth(size_t size);

/* Dedicated-VA linmem slot: arm the one-shot pid hint before the module's first
 * linmem-allocating probe; read the reserved capacity back to bound map_auto. */
void warp_linmem_reserve_hint(uint32_t pid, uint64_t reserve_bytes);
uint64_t warp_linmem_reserved_bytes(uint32_t pid);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
