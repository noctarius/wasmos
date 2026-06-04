/* framebuffer.h - Linear framebuffer setup from UEFI GOP.
 *
 * The bootloader passes the GOP framebuffer physical address and dimensions in
 * boot_info_t.  framebuffer_init() records these; framebuffer_map_high() remaps
 * the physical range into the kernel higher-half VA space after paging is active.
 * Direct pixel write functions are mainly used for panic output and early boot visuals. */
#ifndef WASMOS_FRAMEBUFFER_H
#define WASMOS_FRAMEBUFFER_H

#include <stdint.h>
#include "boot.h"

/* Describes the active framebuffer geometry and pixel format. */
typedef struct framebuffer_info {
    uint64_t framebuffer_base;           /* kernel virtual address after map_high */
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;         /* bytes per scanline (>= width * 4) */
    uint32_t framebuffer_gop_pixel_format; /* EFI_GRAPHICS_PIXEL_FORMAT value */
} framebuffer_info_t;

/* Record GOP info from boot_info; does NOT map the framebuffer yet. */
void framebuffer_init(const boot_info_t *info);

/* Map the physical framebuffer into kernel virtual space; call after paging init. */
int framebuffer_map_high(void);

int framebuffer_get_info(framebuffer_info_t *out);
int framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color);
int framebuffer_fill(uint32_t color);

/* Switch the framebuffer to a panic-safe rendering mode (no IPC or scheduler). */
void framebuffer_panic_begin(void);

/* Write a text string to the framebuffer during a kernel panic. */
void framebuffer_panic_write(const char *text);

#endif
