/* warp/shim.h - Kernel-side ABI shim declarations for the WARP JIT runtime.
 *
 * Mirrors the interface exposed by wasm3/shim.h so callers can be switched
 * between runtimes via WASMOS_WASM_RUNTIME without changing call sites.
 *
 * TODO: implement the freestanding C++ ABI layer (operator new/delete,
 *       __cxa_pure_virtual, __cxa_atexit, .init_array runner) and the
 *       memory-region bindings that back these calls. */
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
