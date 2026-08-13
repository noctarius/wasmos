/* warp/link.h - WARP host-call registration declarations.
 *
 * C interface: warp_link_init() — call once from kernel_init alongside
 *              wasm3_link_init() (only one will compile depending on
 *              WASMOS_WASM_RUNTIME).
 *
 * C++ interface: warp_wasmos_symbols_ring3() / warp_bind_module() — called from
 *                warp_driver.cpp when compiling and instantiating modules. */
#ifndef WASMOS_WARP_LINK_H
#define WASMOS_WARP_LINK_H

#include "boot.h"

#ifdef __cplusplus
#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"

#ifdef WASMOS_WASM_RUNTIME_WARP
/* The only symbol table: DYNAMIC_LINK, for ring-3 execution.  ptr fields are
 * user-space HC trampoline VAs (WARP_R3_HC_TRAMPOLINE + hc_id × 8) instead of
 * kernel function pointers.  Both module init paths take it — the AOT load and
 * the JIT compile, which finishes through initFromCompiledBinary and so also
 * rejects STATIC linkage.  Ring 3 is not a build-time choice, so there is no
 * kernel-function-pointer variant to pick instead. */
vb::Span<vb::NativeSymbol const> warp_wasmos_symbols_ring3(void);
#endif

/* Binds the compiled WasmModule to the per-PID call context so that V1 host
 * functions can resolve linear-memory offsets via getLinearMemoryRegion. */
void warp_bind_module(vb::WasmModule* module, uint32_t pid);
void* warp_context_for_pid(uint32_t pid);
/* Release the per-process WARP call context for `pid` on process exit. */
void warp_ctx_release_pid(uint32_t pid);

extern "C" {
#endif

void warp_link_init(const boot_info_t* boot_info);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* WASMOS_WARP_LINK_H */
