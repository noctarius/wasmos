/* framebuffer.c - Linear GOP framebuffer initialization and kernel panic rendering.
 * Stores the GOP physical address from boot_info; maps it into higher-half VA
 * after paging is active.  Provides a panic text path that bypasses IPC and VT. */
#include "framebuffer.h"
#include "klog.h"
#include "serial.h"
#include "paging.h"
#include "memory.h"
#include "../drivers/framebuffer/font_8x16.h"

#include <stdint.h>
#include <string.h>

static void framebuffer_draw_char(uint32_t col, uint32_t row, char ch, uint32_t fg, uint32_t bg);
static void framebuffer_panic_newline(void);

static framebuffer_info_t g_framebuffer_info = {0};
static uint64_t g_framebuffer_hi_base = 0;
static uint32_t g_panic_col = 0;
static uint32_t g_panic_row = 0;
static uint32_t g_panic_fg = 0x00FFFFFF;
static uint32_t g_panic_bg = 0x00000000;

#define PANIC_FONT_W 8u
#define PANIC_FONT_H 16u

/* Reach a kernel global through the higher-half alias while the kernel is still
 * executing from its low identity mapping (serial.c owns the same switch). Once
 * serial_enable_high_alias is set, a low-VA global address is stale, so every
 * static below is addressed through a *_slot() accessor rather than directly. */
static inline uintptr_t framebuffer_alias_ptr(uintptr_t p) {
    if (serial_high_alias_enabled() && (uint64_t)p < KERNEL_HIGHER_HALF_BASE) {
        p = (uintptr_t)((uint64_t)p + KERNEL_HIGHER_HALF_BASE);
    }
    return p;
}

static inline framebuffer_info_t* framebuffer_info_slot(void) {
    return (framebuffer_info_t*)(void*)framebuffer_alias_ptr((uintptr_t)&g_framebuffer_info);
}

static inline uint64_t* fb_hi_base_slot(void) {
    return (uint64_t*)(void*)framebuffer_alias_ptr((uintptr_t)&g_framebuffer_hi_base);
}

static inline uint64_t _fb_mmio_va(void) {
    return *fb_hi_base_slot();
}

static inline uint32_t* panic_col_slot(void) {
    return (uint32_t*)(void*)framebuffer_alias_ptr((uintptr_t)&g_panic_col);
}

static inline uint32_t* panic_row_slot(void) {
    return (uint32_t*)(void*)framebuffer_alias_ptr((uintptr_t)&g_panic_row);
}

static inline uint32_t* panic_fg_slot(void) {
    return (uint32_t*)(void*)framebuffer_alias_ptr((uintptr_t)&g_panic_fg);
}

static inline uint32_t* panic_bg_slot(void) {
    return (uint32_t*)(void*)framebuffer_alias_ptr((uintptr_t)&g_panic_bg);
}

/* Records the GOP aperture the bootloader reported.  It stores geometry only —
 * the base kept here is the raw PHYSICAL address and nothing is mapped, so no
 * pixel may be touched until framebuffer_map_high has run.
 *
 * The whole descriptor is REJECTED unless the GOP-present flag is set and the
 * base, size, width and height are all non-zero; a partially valid one leaves
 * the recorded state untouched (all zero on a first call), which every draw path
 * then reads as "no framebuffer".  No failure is reported beyond the log line.
 *
 * info is borrowed for the call: the fields are copied out. */
void framebuffer_init(const boot_info_t* info) {
    framebuffer_info_t* fb = framebuffer_info_slot();
    klog_printf(
        "[framebuffer] init 0x%016llX 0x%016llX 0x%016llX 0x%016llX 0x%016llX flags=0x%016llX\n",
        (unsigned long long)(info ? addr_cast(uint64_t, info->framebuffer_base) : 0),
        (unsigned long long)(info ? (uint64_t)info->framebuffer_size : 0),
        (unsigned long long)(info ? info->framebuffer_width : 0),
        (unsigned long long)(info ? info->framebuffer_height : 0),
        (unsigned long long)(info ? info->framebuffer_pixels_per_scanline : 0),
        (unsigned long long)(info ? info->flags : 0));

    if (!info || !(info->flags & BOOT_INFO_FLAG_GOP_PRESENT) || !info->framebuffer_base ||
        info->framebuffer_size == 0 || info->framebuffer_width == 0 ||
        info->framebuffer_height == 0) {
        return;
    }
    fb->framebuffer_base = addr_cast(uint64_t, info->framebuffer_base);
    fb->framebuffer_size = info->framebuffer_size;
    fb->framebuffer_width = info->framebuffer_width;
    fb->framebuffer_height = info->framebuffer_height;
    fb->framebuffer_stride = info->framebuffer_pixels_per_scanline;
    fb->framebuffer_gop_pixel_format =
        (uint32_t)((info->flags & BOOT_INFO_FLAG_GOP_PIXEL_FORMAT_MASK) >>
                   BOOT_INFO_FLAG_GOP_PIXEL_FORMAT_SHIFT);
    klog_printf("[framebuffer] stride=0x%016llX\n",
                (unsigned long long)info->framebuffer_pixels_per_scanline);
}

/* Copies the recorded geometry into the caller's structure.  framebuffer_base in
 * the result is the PHYSICAL aperture address, not a usable pointer.
 *
 * Returns WASMOS_OK, WASMOS_INVAL for a NULL out, or
 * WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT when no usable framebuffer was recorded.
 * Success says the firmware described one, NOT that it is mapped — that is
 * framebuffer_map_high's business. */
wasmos_error_code_t framebuffer_get_info(framebuffer_info_t* out) {
    framebuffer_info_t* fb = framebuffer_info_slot();
    if (!out) {
        return WASMOS_INVAL;
    }
    if (fb->framebuffer_base == 0 || fb->framebuffer_size == 0 || fb->framebuffer_width == 0 ||
        fb->framebuffer_height == 0 || fb->framebuffer_stride == 0) {
        return WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT;
    }
    memcpy(out, fb, sizeof(framebuffer_info_t));
    return WASMOS_OK;
}

/* Map the GOP aperture at KERNEL_MMIO_FB_VA, page-aligned down from the GOP
 * base and up past its end. Runs after paging is active; until it succeeds
 * _fb_mmio_va() is 0, which is how the draw paths below detect "not mapped
 * yet". Returns 0 on success, -1 if no framebuffer was recorded or a mapping
 * failed. */
int framebuffer_map_high(void) {
    framebuffer_info_t* fb = framebuffer_info_slot();
    if (fb->framebuffer_base == 0 || fb->framebuffer_size == 0) {
        return -1;
    }
    uint64_t phys_base = fb->framebuffer_base & ~0xFFFULL;
    uint64_t phys_end = (fb->framebuffer_base + fb->framebuffer_size + 0xFFFULL) & ~0xFFFULL;
    uint64_t num_pages = (phys_end - phys_base) >> 12;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t virt = KERNEL_MMIO_FB_VA + (i << 12);
        uint64_t phys = phys_base + (i << 12);
        if (paging_map_4k(virt, phys, MEM_REGION_FLAG_WRITE) != 0) {
            return -1;
        }
    }
    *fb_hi_base_slot() = KERNEL_MMIO_FB_VA + (fb->framebuffer_base & 0xFFFULL);
    klog_printf("[framebuffer] hi base=0x%016llx pages=%llu\n",
                (unsigned long long)*fb_hi_base_slot(), (unsigned long long)num_pages);
    return 0;
}

/* All pixel paths here assume a 32-bit-per-pixel GOP mode: stride is counted in
 * pixels and scaled by 4. framebuffer_gop_pixel_format is recorded for callers
 * that care about channel order but is not consulted when writing. */
wasmos_error_code_t framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    framebuffer_info_t* fb = framebuffer_info_slot();
    uint64_t fb_va = _fb_mmio_va();
    if (fb_va == 0 || fb->framebuffer_base == 0 || fb->framebuffer_size == 0 ||
        fb->framebuffer_width == 0 || fb->framebuffer_height == 0) {
        return WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT;
    }
    /* Off-screen coordinates are the caller's mistake and it can correct them,
     * which is a different thing from there being no framebuffer at all. */
    if (x >= fb->framebuffer_width || y >= fb->framebuffer_height) {
        return WASMOS_INVAL;
    }
    uint64_t stride = fb->framebuffer_stride;
    if (stride == 0) {
        return WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT;
    }
    uint64_t index = (uint64_t)y * stride + x;
    uint64_t offset = index * 4;
    if (offset + 4 > fb->framebuffer_size) {
        return WASMOS_INVAL;
    }
    uint32_t* pixel = ptr_cast(uint32_t, (fb_va + offset));
    *pixel = color;
    return WASMOS_OK;
}

/* Fills the visible area with one 32-bit pixel value, writing stride * height
 * pixels and clamping to the aperture size so a stride the firmware overstated
 * cannot run past it.  `color` is stored raw, in whatever channel order the
 * recorded GOP pixel format uses; no conversion happens here.
 *
 * Returns 0 on success and -1 when the aperture is not mapped yet or the
 * recorded geometry is incomplete.  Writes device memory directly and takes no
 * lock, so a concurrent drawer sees a torn result. */
int framebuffer_fill(uint32_t color) {
    framebuffer_info_t* fb_info = framebuffer_info_slot();
    uint64_t fb_va = _fb_mmio_va();
    if (fb_va == 0 || fb_info->framebuffer_base == 0 || fb_info->framebuffer_size == 0 ||
        fb_info->framebuffer_width == 0 || fb_info->framebuffer_height == 0 ||
        fb_info->framebuffer_stride == 0) {
        return -1;
    }

    uint32_t* fb = ptr_cast(uint32_t, fb_va);
    uint64_t stride = fb_info->framebuffer_stride;
    uint64_t height = fb_info->framebuffer_height;
    uint64_t total = stride * height;
    uint64_t max = fb_info->framebuffer_size / sizeof(uint32_t);
    if (total > max) {
        total = max;
    }

    for (uint64_t i = 0; i < total; ++i) {
        fb[i] = color;
    }
    return 0;
}

static void framebuffer_draw_char(uint32_t col, uint32_t row, char ch, uint32_t fg, uint32_t bg) {
    framebuffer_info_t* fb_info = framebuffer_info_slot();
    uint64_t fb_va = _fb_mmio_va();
    if (fb_va == 0 || fb_info->framebuffer_base == 0 || fb_info->framebuffer_width == 0 ||
        fb_info->framebuffer_height == 0 || fb_info->framebuffer_stride == 0) {
        return;
    }

    uint32_t max_cols = fb_info->framebuffer_width / PANIC_FONT_W;
    uint32_t max_rows = fb_info->framebuffer_height / PANIC_FONT_H;
    if (col >= max_cols || row >= max_rows) {
        return;
    }

    uint8_t glyph_index = (uint8_t)ch;
    if (glyph_index < 0x20 || glyph_index > 0x7E) {
        glyph_index = '?';
    }
    const uint8_t* glyph = font_8x16[glyph_index - 0x20];

    uint32_t x0 = col * PANIC_FONT_W;
    uint32_t y0 = row * PANIC_FONT_H;
    uint32_t* fb = ptr_cast(uint32_t, fb_va);
    uint32_t stride = fb_info->framebuffer_stride;

    for (uint32_t y = 0; y < PANIC_FONT_H; ++y) {
        uint8_t bits = glyph[y];
        uint32_t* line = fb + (y0 + y) * stride + x0;
        for (uint32_t x = 0; x < PANIC_FONT_W; ++x) {
            line[x] = (bits & (0x80u >> x)) ? fg : bg;
        }
    }
}

static void framebuffer_panic_newline(void) {
    framebuffer_info_t* fb_info = framebuffer_info_slot();
    uint32_t* panic_col = panic_col_slot();
    uint32_t* panic_row = panic_row_slot();
    *panic_col = 0;
    (*panic_row)++;
    uint32_t max_rows = fb_info->framebuffer_height / PANIC_FONT_H;
    /* FIXME: Panic text currently clips at bottom instead of scrolling. */
    if (max_rows == 0 || *panic_row >= max_rows) {
        *panic_row = max_rows ? (max_rows - 1) : 0;
    }
}

/* Takes the screen over for a panic dump: clears it to black and resets the
 * panic text cursor to row 1, column 1 with white on black.  If the fill fails —
 * no mapped framebuffer — the cursor is left as it was and subsequent
 * framebuffer_panic_write calls have nothing to draw on.
 *
 * Intended for the panic path only: it takes no lock and does not coordinate
 * with the compositor or any driver that owns the framebuffer. */
void framebuffer_panic_begin(void) {
    uint32_t* panic_col = panic_col_slot();
    uint32_t* panic_row = panic_row_slot();
    uint32_t* panic_fg = panic_fg_slot();
    uint32_t* panic_bg = panic_bg_slot();
    if (framebuffer_fill(0x00000000) != 0) {
        return;
    }
    *panic_col = 1;
    *panic_row = 1;
    *panic_fg = 0x00FFFFFF;
    *panic_bg = 0x00000000;
}

/* Draws a string with the built-in panic font, advancing the persistent panic
 * cursor.  '\r' returns to column 0 and '\n' starts a new row; the line wraps at
 * the screen edge.  Text that reaches the bottom row CLIPS — the last row is
 * overdrawn rather than scrolled (see the FIXME in framebuffer_panic_newline).
 *
 * A NULL string, an unrecorded framebuffer, and a screen too small for one
 * character cell are all silent no-ops.  Lock-free, for the same panic-context
 * reason as the unlocked serial writers. */
void framebuffer_panic_write(const char* text) {
    framebuffer_info_t* fb_info = framebuffer_info_slot();
    uint32_t* panic_col = panic_col_slot();
    uint32_t* panic_row = panic_row_slot();
    uint32_t* panic_fg = panic_fg_slot();
    uint32_t* panic_bg = panic_bg_slot();
    if (!text || fb_info->framebuffer_base == 0) {
        return;
    }

    uint32_t max_cols = fb_info->framebuffer_width / PANIC_FONT_W;
    uint32_t max_rows = fb_info->framebuffer_height / PANIC_FONT_H;
    if (max_cols == 0 || max_rows == 0) {
        return;
    }

    while (*text) {
        char ch = *text++;
        if (ch == '\r') {
            *panic_col = 0;
            continue;
        }
        if (ch == '\n') {
            framebuffer_panic_newline();
            continue;
        }
        if (*panic_col >= max_cols) {
            framebuffer_panic_newline();
        }
        if (*panic_row >= max_rows) {
            return;
        }
        framebuffer_draw_char(*panic_col, *panic_row, ch, *panic_fg, *panic_bg);
        (*panic_col)++;
    }
}
