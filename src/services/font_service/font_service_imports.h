/* font_service_imports.h - Native driver ABI includes for the font service.
 * Aggregates the native-driver API table and driver ABI headers so that the
 * font service's native C entry point can call IPC and DMA helpers without
 * depending on the WASM runtime. */
#ifndef WASMOS_FONT_SERVICE_IMPORTS_H
#define WASMOS_FONT_SERVICE_IMPORTS_H

#include "../../drivers/include/wasmos_native_driver.h"
#include "../../drivers/include/wasmos_driver_abi.h"
#include "../../../src/libc/include/wasmos/font_ipc.h"
void wasmos_stbtt_alloc_reset(void);
#include "../../../libs/stb/stb_truetype.h"

#endif
