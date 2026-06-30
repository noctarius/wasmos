/* warp/link.h - WARP host-call registration declarations.
 *
 * C interface: warp_link_init() — call once from kernel_init alongside
 *              wasm3_link_init() (only one will compile depending on
 *              WASMOS_WASM_RUNTIME).
 *
 * C++ interface: warp_wasmos_symbols() / warp_bind_module() — called from
 *                warp_driver.cpp when compiling and instantiating modules. */
#ifndef WASMOS_WARP_LINK_H
#define WASMOS_WARP_LINK_H

#include "boot.h"

#ifdef __cplusplus
#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"

/* STATIC_LINK symbol table — pass to initFromBytecode (JIT path).
 * Bakes function pointers into call stubs at compile time; no basedata overhead. */
vb::Span<vb::NativeSymbol const> warp_wasmos_symbols(void);

/* DYNAMIC_LINK symbol table — pass to initFromCompiledBinary (AOT load path).
 * initFromCompiledBinary() throws Wrong_type for any STATIC symbol. */
vb::Span<vb::NativeSymbol const> warp_wasmos_symbols_for_aot_load(void);

#ifdef WASMOS_WARP_RING3
/* DYNAMIC_LINK symbol table for ring-3 execution; ptr fields are user-space
 * HC trampoline VAs (WARP_R3_HC_TRAMPOLINE + hc_id × 8) instead of kernel
 * function pointers.  Pass to initFromBytecode on the ring-3 compile path. */
vb::Span<vb::NativeSymbol const> warp_wasmos_symbols_ring3(void);
#endif

/* Binds the compiled WasmModule to the per-PID call context so that V1 host
 * functions can resolve linear-memory offsets via getLinearMemoryRegion. */
void warp_bind_module(vb::WasmModule *module, uint32_t pid);
void *warp_context_for_pid(uint32_t pid);
/* Release the per-process WARP call context for `pid` on process exit. */
void warp_ctx_release_pid(uint32_t pid);

extern "C" {
#endif

void warp_link_init(const boot_info_t *boot_info);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* WASMOS_WARP_LINK_H */
