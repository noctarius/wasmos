/* warp/link.h - WARP host-call registration declarations.
 *
 * Mirrors the interface exposed by wasm3/link.h so callers can be switched
 * between runtimes via WASMOS_WASM_RUNTIME without changing call sites.
 *
 * TODO: implement host-call registration against the WARP WasmModule API
 *       (vb::WasmModule::addNativeSymbol / GlobalSymbol). */
#ifndef WASMOS_WARP_LINK_H
#define WASMOS_WARP_LINK_H

#include "boot.h"

void warp_link_init(const boot_info_t *boot_info);

#endif
