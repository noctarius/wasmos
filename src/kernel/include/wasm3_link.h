#ifndef WASMOS_WASM3_LINK_H
#define WASMOS_WASM3_LINK_H

#include "boot.h"
#include "wasm3.h"

void wasm3_link_init(const boot_info_t *boot_info);
int wasm3_link_wasmos(IM3Module module);
int wasm3_link_env(IM3Module module);

#endif
