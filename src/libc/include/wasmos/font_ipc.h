/* font_ipc.h - IPC message types and structs for the font-rasterizer service.
 * Defines FONT_IPC_MEASURE_GLYPH_REQ, FONT_IPC_RASTER_GLYPH_INTO_REQ, and
 * the associated response fields used by libui to request glyph rendering. */
#ifndef WASMOS_FONT_IPC_H
#define WASMOS_FONT_IPC_H

#include <stdint.h>

/* FONT_IPC_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../../../abi/generated/c/wasmos_opcodes.h"
#include "../../../../abi/generated/c/wasmos_status.h"

enum { FONT_ID_ROBOTO = 1, FONT_ID_ROBOTO_MONO = 2, FONT_ID_NOTO_SERIF = 3 };

/* Status codes are the packed font domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_FONT_*. */

#endif
