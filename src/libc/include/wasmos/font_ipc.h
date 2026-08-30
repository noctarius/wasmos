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

/* Request descriptor for FONT_IPC_RASTER_GLYPH_INTO_REQ.
 *
 * The call carries six independent values -- a handle, the text and its length,
 * and the destination buffer plus its borrow -- which is past the four opcode
 * words ipc_message_t provides, so it travels as a descriptor in a transfer
 * buffer the CLIENT owns (skills/wasmos-add-opcode, Step 0). The message names
 * that buffer: arg0=buffer_id, arg1=byte_offset, arg2=size, arg3=borrow_id.
 *
 * The UTF-8 run follows this struct in the same buffer, `text_len` bytes at
 * `byte_offset + sizeof(font_raster_request_t)`, so one buffer and one borrow
 * carry the whole request. The destination mask is a SECOND buffer the client
 * owns and lends WRITE, named here because the service cannot derive either id:
 * xfer_buffer_borrow returns the borrow to the owner. */
typedef struct {
    int32_t handle_id;
    int32_t text_len;
    int32_t mask_buffer_id;
    int32_t mask_borrow_id;
} font_raster_request_t;

#endif
