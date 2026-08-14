/* framebuffer.h - Linear framebuffer setup from UEFI GOP.
 *
 * The bootloader passes the GOP framebuffer physical address and dimensions in
 * boot_info_t.  framebuffer_init() records these; framebuffer_map_high() remaps
 * the physical range into the kernel higher-half VA space after paging is active.
 * Direct pixel write functions are mainly used for panic output and early boot visuals. */
#ifndef WASMOS_FRAMEBUFFER_H
#define WASMOS_FRAMEBUFFER_H

#include <stdint.h>

#include "wasmos_status.h"
#include "boot.h"

/* Describes the active framebuffer geometry and pixel format. */
typedef struct framebuffer_info {
    uint64_t framebuffer_base; /* GOP PHYSICAL base; the kernel VA alias installed
                                * by framebuffer_map_high is held privately and is
                                * not reported here */
    uint64_t framebuffer_size; /* bytes */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;           /* PIXELS per scanline (>= width), not bytes */
    uint32_t framebuffer_gop_pixel_format; /* EFI_GRAPHICS_PIXEL_FORMAT value */
} framebuffer_info_t;

/* Record GOP info from boot_info; does NOT map the framebuffer yet. A boot_info
 * without BOOT_INFO_FLAG_GOP_PRESENT, or with a zero base/size/width/height,
 * leaves the framebuffer unrecorded and every accessor below reporting
 * "not present". */
void framebuffer_init(const boot_info_t* info);

/* Map the physical framebuffer into kernel virtual space; call after paging
 * init. Until it succeeds the pixel writers below are no-ops or report
 * not-present. Returns 0 on success, -1 if no framebuffer was recorded or a
 * page mapping failed. */
int framebuffer_map_high(void);

/* Copy the active geometry into *out. WASMOS_OK, WASMOS_INVAL for a NULL out,
 * or WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT when no usable framebuffer is recorded. */
wasmos_error_code_t framebuffer_get_info(framebuffer_info_t* out);

/* WASMOS_OK, WASMOS_INVAL when (x, y) is off-screen -- a caller mistake it can
 * correct, distinct from there being no framebuffer -- or
 * WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT when none is mapped. */
wasmos_error_code_t framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Fill the whole visible area with one color. Returns 0, or -1 when no
 * framebuffer is mapped. */
int framebuffer_fill(uint32_t color);

/* Switch the framebuffer to a panic-safe rendering mode (no IPC or scheduler): clear the
 * screen to black and reset the text cursor to the top-left with white on black.  Does
 * nothing when no framebuffer is mapped, in which case framebuffer_panic_write also
 * produces nothing.  Safe from a panic context — no locks, no allocation. */
void framebuffer_panic_begin(void);

/* Write a text string to the framebuffer during a kernel panic, using a built-in bitmap
 * font.  Handles '\r' and '\n'; every other byte is drawn as a glyph and the cursor wraps
 * at the right edge.  There is no scrolling: once the cursor passes the bottom row further
 * text is dropped.  A NULL string or an unmapped framebuffer is ignored. */
void framebuffer_panic_write(const char* text);

#endif
