#ifndef WASMOS_WASM3_PROBE_H
#define WASMOS_WASM3_PROBE_H

#include <stdint.h>
#include "boot.h"

int wasm3_probe_run(const boot_info_t *info, uint32_t module_index);

#endif
