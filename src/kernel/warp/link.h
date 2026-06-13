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

/* Returns the NativeSymbol table for the wasmos import module.
 * Pass this span to module.initFromBytecode(bytecode, warp_wasmos_symbols(), true). */
vb::Span<vb::NativeSymbol const> warp_wasmos_symbols(void);

/* Binds the compiled WasmModule to the per-PID call context so that V1 host
 * functions can resolve linear-memory offsets via getLinearMemoryRegion. */
void warp_bind_module(vb::WasmModule *module, uint32_t pid);

extern "C" {
#endif

void warp_link_init(const boot_info_t *boot_info);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* WASMOS_WARP_LINK_H */
