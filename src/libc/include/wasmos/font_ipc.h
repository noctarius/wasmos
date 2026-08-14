/* font_ipc.h - client surface for the font-rasterizer service: the FONT_ID_*
 * selectors a caller names a font by, plus the generated FONT_IPC_* opcodes and
 * packed status codes the protocol uses. */
#ifndef WASMOS_FONT_IPC_H
#define WASMOS_FONT_IPC_H

#include <stdint.h>

/* FONT_IPC_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../../../abi/generated/c/wasmos_opcodes.h"
#include "../../../../abi/generated/c/wasmos_status.h"

/* Selectors for the faces the font service loads at startup from
 * /boot/system/fonts: roboto.ttf (proportional sans), roboto_mono.ttf (fixed
 * pitch) and roboto_serif.ttf (serif — the id name predates the file). A face
 * whose file is missing stays unavailable and FONT_IPC_OPEN_FONT_REQ for it
 * fails; there is no fallback to another id. */
enum { FONT_ID_ROBOTO = 1, FONT_ID_ROBOTO_MONO = 2, FONT_ID_NOTO_SERIF = 3 };

/* Status codes are the packed font domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_FONT_*. */

#endif
