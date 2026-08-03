/* font_ipc.h - IPC message types and structs for the font-rasterizer service.
 * Defines FONT_IPC_MEASURE_GLYPH_REQ, FONT_IPC_RASTER_GLYPH_INTO_REQ, and
 * the associated response fields used by libui to request glyph rendering. */
#ifndef WASMOS_FONT_IPC_H
#define WASMOS_FONT_IPC_H

#include <stdint.h>

/* FONT_IPC_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../../../abi/generated/c/wasmos_opcodes.h"

enum { FONT_ID_ROBOTO = 1, FONT_ID_ROBOTO_MONO = 2, FONT_ID_NOTO_SERIF = 3 };

enum {
    FONT_STATUS_OK = 0,
    FONT_STATUS_INVALID = -1,
    FONT_STATUS_PERMISSION = -2,
    FONT_STATUS_UNSUPPORTED = -3,
    FONT_STATUS_IO = -4,
    FONT_STATUS_BUSY = -5
};

#endif
