/* wasm3_link.h - wasm3 hostcall registration declarations.
 * wasm3_link_all() registers every WASMOS import into a wasm3 runtime before
 * module instantiation; called once per wasm_driver_t startup. */
#ifndef WASMOS_WASM3_LINK_H
#define WASMOS_WASM3_LINK_H

#include "boot.h"
#include "wasm3.h"

void wasm3_link_init(const boot_info_t *boot_info);
int wasm3_link_wasmos(IM3Module module);
int wasm3_link_env(IM3Module module);

#endif
