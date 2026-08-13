/* gfx_compositor_imports.h - single @cImport target for gfx_compositor.zig.
 * Aggregates the native-driver API table (wasmos_driver_api_t) and the GFX/font
 * IPC opcode headers; Zig imports this one header rather than each of them. */
#ifndef WASMOS_GFX_COMPOSITOR_IMPORTS_H
#define WASMOS_GFX_COMPOSITOR_IMPORTS_H

#include "../../drivers/include/wasmos_native_driver.h"
#include "../../drivers/include/wasmos_driver_abi.h"
#include "../../../src/libc/include/wasmos/gfx_ipc.h"
#include "../../../src/libc/include/wasmos/font_ipc.h"

#endif
