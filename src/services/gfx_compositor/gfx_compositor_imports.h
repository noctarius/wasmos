/* gfx_compositor_imports.h - Native driver ABI includes for the GFX compositor.
 * Aggregates the native-driver API table and GFX IPC headers so that the
 * compositor's native C entry point can send framebuffer draw commands via
 * the standard wasmos_driver_api_t function-pointer table. */
#ifndef WASMOS_GFX_COMPOSITOR_IMPORTS_H
#define WASMOS_GFX_COMPOSITOR_IMPORTS_H

#include "../../drivers/include/wasmos_native_driver.h"
#include "../../drivers/include/wasmos_driver_abi.h"
#include "../../../src/libc/include/wasmos/gfx_ipc.h"
#include "../../../src/libc/include/wasmos/font_ipc.h"

#endif
