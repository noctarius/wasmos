/* warp/link.cpp - WARP host-call registration (wasmos imports → vb::NativeSymbol).
 *
 * TODO: implement warp_link_init and per-module registration using
 *       vb::WasmModule::addNativeSymbol / GlobalSymbol, mirroring the
 *       full import table registered in wasm3/link.c. */

#include "link.h"

void
warp_link_init(const boot_info_t * /*boot_info*/)
{
    /* TODO: store boot_info for use in host-call implementations */
}
