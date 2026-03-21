#include "framebuffer.h"
#include "serial.h"
#include "../drivers/framebuffer/font_8x16.h"

#include <stdint.h>
#include <string.h>

static void framebuffer_log_hex64(uint64_t value);
static void framebuffer_draw_char(uint32_t col, uint32_t row, char ch, uint32_t fg, uint32_t bg);
static void framebuffer_panic_newline(void);

static framebuffer_info_t g_framebuffer_info = {0};
static uint32_t g_panic_col = 0;
static uint32_t g_panic_row = 0;
static uint32_t g_panic_fg = 0x00FFFFFF;
static uint32_t g_panic_bg = 0x00000000;

#define PANIC_FONT_W 8u
#define PANIC_FONT_H 16u

static void framebuffer_log_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';
    serial_write(buf);
}

void framebuffer_init(const boot_info_t *info)
{
    serial_write("[framebuffer] init ");
    framebuffer_log_hex64(info ? (uint64_t)(uintptr_t)info->framebuffer_base : 0);
    serial_write(" ");
    framebuffer_log_hex64(info ? (uint64_t)info->framebuffer_size : 0);
    serial_write(" ");
    framebuffer_log_hex64(info ? info->framebuffer_width : 0);
    serial_write(" ");
    framebuffer_log_hex64(info ? info->framebuffer_height : 0);
    serial_write(" ");
    framebuffer_log_hex64(info ? info->framebuffer_pixels_per_scanline : 0);
    serial_write(" flags=");
    framebuffer_log_hex64(info ? info->flags : 0);
    serial_write("\n");

    if (!info || !(info->flags & BOOT_INFO_FLAG_GOP_PRESENT) ||
        !info->framebuffer_base || info->framebuffer_size == 0 ||
        info->framebuffer_width == 0 || info->framebuffer_height == 0) {
        return;
    }
    g_framebuffer_info.framebuffer_base = (uint64_t)(uintptr_t)info->framebuffer_base;
    g_framebuffer_info.framebuffer_size = info->framebuffer_size;
    g_framebuffer_info.framebuffer_width = info->framebuffer_width;
    g_framebuffer_info.framebuffer_height = info->framebuffer_height;
    g_framebuffer_info.framebuffer_stride = info->framebuffer_pixels_per_scanline;
    serial_write("[framebuffer] stride=");
    framebuffer_log_hex64(info->framebuffer_pixels_per_scanline);
    serial_write("\n");
}

int framebuffer_get_info(framebuffer_info_t *out)
{
    if (!out || g_framebuffer_info.framebuffer_base == 0 ||
        g_framebuffer_info.framebuffer_size == 0 ||
        g_framebuffer_info.framebuffer_width == 0 ||
        g_framebuffer_info.framebuffer_height == 0 ||
        g_framebuffer_info.framebuffer_stride == 0) {
        return -1;
    }
    memcpy(out, &g_framebuffer_info, sizeof(framebuffer_info_t));
    return 0;
}

int framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (g_framebuffer_info.framebuffer_base == 0 ||
        g_framebuffer_info.framebuffer_size == 0 ||
        g_framebuffer_info.framebuffer_width == 0 ||
        g_framebuffer_info.framebuffer_height == 0) {
        return -1;
    }
    if (x >= g_framebuffer_info.framebuffer_width ||
        y >= g_framebuffer_info.framebuffer_height) {
        return -1;
    }
    uint64_t stride = g_framebuffer_info.framebuffer_stride;
    if (stride == 0) {
        return -1;
    }
    uint64_t index = (uint64_t)y * stride + x;
    uint64_t offset = index * 4;
    if (offset + 4 > g_framebuffer_info.framebuffer_size) {
        return -1;
    }
    uint32_t *pixel = (uint32_t *)(uintptr_t)(g_framebuffer_info.framebuffer_base + offset);
    *pixel = color;
    return 0;
}

int framebuffer_fill(uint32_t color)
{
    if (g_framebuffer_info.framebuffer_base == 0 ||
        g_framebuffer_info.framebuffer_size == 0 ||
        g_framebuffer_info.framebuffer_width == 0 ||
        g_framebuffer_info.framebuffer_height == 0 ||
        g_framebuffer_info.framebuffer_stride == 0) {
        return -1;
    }

    uint32_t *fb = (uint32_t *)(uintptr_t)g_framebuffer_info.framebuffer_base;
    uint64_t stride = g_framebuffer_info.framebuffer_stride;
    uint64_t height = g_framebuffer_info.framebuffer_height;
    uint64_t total = stride * height;
    uint64_t max = g_framebuffer_info.framebuffer_size / sizeof(uint32_t);
    if (total > max) {
        total = max;
    }

    for (uint64_t i = 0; i < total; ++i) {
        fb[i] = color;
    }
    return 0;
}

static void framebuffer_draw_char(uint32_t col, uint32_t row, char ch, uint32_t fg, uint32_t bg)
{
    if (g_framebuffer_info.framebuffer_base == 0 ||
        g_framebuffer_info.framebuffer_width == 0 ||
        g_framebuffer_info.framebuffer_height == 0 ||
        g_framebuffer_info.framebuffer_stride == 0) {
        return;
    }

    uint32_t max_cols = g_framebuffer_info.framebuffer_width / PANIC_FONT_W;
    uint32_t max_rows = g_framebuffer_info.framebuffer_height / PANIC_FONT_H;
    if (col >= max_cols || row >= max_rows) {
        return;
    }

    uint8_t glyph_index = (uint8_t)ch;
    if (glyph_index < 0x20 || glyph_index > 0x7E) {
        glyph_index = '?';
    }
    const uint8_t *glyph = font_8x16[glyph_index - 0x20];

    uint32_t x0 = col * PANIC_FONT_W;
    uint32_t y0 = row * PANIC_FONT_H;
    uint32_t *fb = (uint32_t *)(uintptr_t)g_framebuffer_info.framebuffer_base;
    uint32_t stride = g_framebuffer_info.framebuffer_stride;

    for (uint32_t y = 0; y < PANIC_FONT_H; ++y) {
        uint8_t bits = glyph[y];
        uint32_t *line = fb + (y0 + y) * stride + x0;
        for (uint32_t x = 0; x < PANIC_FONT_W; ++x) {
            line[x] = (bits & (0x80u >> x)) ? fg : bg;
        }
    }
}

static void framebuffer_panic_newline(void)
{
    g_panic_col = 0;
    g_panic_row++;
    uint32_t max_rows = g_framebuffer_info.framebuffer_height / PANIC_FONT_H;
    /* FIXME: Panic text currently clips at bottom instead of scrolling. */
    if (max_rows == 0 || g_panic_row >= max_rows) {
        g_panic_row = max_rows ? (max_rows - 1) : 0;
    }
}

void framebuffer_panic_begin(void)
{
    if (framebuffer_fill(0x00000000) != 0) {
        return;
    }
    g_panic_col = 1;
    g_panic_row = 1;
    g_panic_fg = 0x00FFFFFF;
    g_panic_bg = 0x00000000;
}

void framebuffer_panic_write(const char *text)
{
    if (!text || g_framebuffer_info.framebuffer_base == 0) {
        return;
    }

    uint32_t max_cols = g_framebuffer_info.framebuffer_width / PANIC_FONT_W;
    uint32_t max_rows = g_framebuffer_info.framebuffer_height / PANIC_FONT_H;
    if (max_cols == 0 || max_rows == 0) {
        return;
    }

    while (*text) {
        char ch = *text++;
        if (ch == '\r') {
            g_panic_col = 0;
            continue;
        }
        if (ch == '\n') {
            framebuffer_panic_newline();
            continue;
        }
        if (g_panic_col >= max_cols) {
            framebuffer_panic_newline();
        }
        if (g_panic_row >= max_rows) {
            return;
        }
        framebuffer_draw_char(g_panic_col, g_panic_row, ch, g_panic_fg, g_panic_bg);
        g_panic_col++;
    }
}
