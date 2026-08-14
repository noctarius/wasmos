#ifndef FBTEXT_INTERNAL_H
#define FBTEXT_INTERNAL_H

#include <stdint.h>

/*
 * Internal types for the framebuffer text layer.
 * Not part of the public driver ABI; used only inside the framebuffer driver.
 */

#define FONT_W 8
#define FONT_H 16
/* Cell-to-glyph size ratio. Must stay 1: render.c positions cells by CELL_W/
 * CELL_H but blits exactly FONT_W x FONT_H pixels, so a larger scale would draw
 * unscaled glyphs into oversized cells. */
#define FONT_SCALE 1
#define CELL_W (FONT_W * FONT_SCALE)
#define CELL_H (FONT_H * FONT_SCALE)

/* Maximum grid dimensions.  Sized for 1280×1024 / 8×16 = 160×64. */
#define FBTEXT_MAX_COLS 160
#define FBTEXT_MAX_ROWS 64

/* CGA-style default colors. */
#define FBTEXT_DEFAULT_FG 15 /* bright white */
#define FBTEXT_DEFAULT_BG 0  /* black        */

typedef struct {
    /* Unicode codepoint, 0 = empty. The built-in font covers 0x20-0x7E only;
     * render.c draws anything outside that range as a space. */
    uint32_t ch;
    uint8_t fg;   /* 4-bit foreground palette index */
    uint8_t bg;   /* 4-bit background palette index */
    uint8_t attr; /* reserved for bold/underline/blink */
    uint8_t _pad;
} fbtext_cell_t; /* 8 bytes */

typedef struct {
    uint16_t col;
    uint16_t row;
} fbtext_cursor_t;

typedef struct {
    uint32_t* fb;       /* mapped framebuffer base pointer */
    uint32_t fb_stride; /* pixels per scanline */
    uint16_t cols;      /* actual grid width  */
    uint16_t rows;      /* actual grid height */
    fbtext_cursor_t cursor;
    uint8_t cur_fg;          /* current foreground palette index */
    uint8_t cur_bg;          /* current background palette index */
    uint8_t suppress_render; /* skip fbtext_render_cell; caller must fbtext_render_all when done */
    uint8_t _pad;
    uint32_t palette[16];
    fbtext_cell_t cells[FBTEXT_MAX_ROWS * FBTEXT_MAX_COLS];
} fbtext_state_t;

/* render.c API.
 *
 * Every call writes pixels through `s->fb` directly and makes no kernel call, so
 * the caller must have a valid framebuffer mapping installed before the first
 * one. None of them validates `s`, and none allocates. They are not re-entrant
 * and assume a single caller (the framebuffer driver's own request loop).
 *
 * `s->suppress_render` gates all painting: while it is set, the cell grid is
 * still updated but nothing reaches the framebuffer, so a caller batching many
 * updates sets it, does the work, clears it, and calls fbtext_render_all. */

/* Bind `s` to a framebuffer and reset it: derive the grid from the pixel
 * geometry (`width`/`height` in pixels, `stride` in PIXELS per scanline, which
 * may exceed the width), install the CGA palette and default colors, home the
 * cursor, and blank every cell. The grid is CLAMPED to FBTEXT_MAX_COLS x
 * FBTEXT_MAX_ROWS, so a display larger than the built-in maximum is used only in
 * part rather than overflowing the cell array. `fb` is borrowed and must stay
 * mapped for as long as `s` is used. Paints nothing -- the framebuffer keeps
 * whatever was on it until a render call. */
void fbtext_render_init(fbtext_state_t* s, uint32_t* fb, uint32_t stride, uint32_t width,
                        uint32_t height);
/* Blit one cell from the grid to the framebuffer. A col/row outside the grid is
 * ignored rather than being an error, as is any call while suppress_render is
 * set. A cell whose codepoint is outside the font's 0x20-0x7E range is drawn as
 * a SPACE -- note fbtext_put_char instead substitutes '?' when it stores such a
 * codepoint, so the two disagree only for cells written by some other path. */
void fbtext_render_cell(fbtext_state_t* s, uint16_t col, uint16_t row);
/* Repaint every cell. Cost is the whole grid, so it is the batch-flush partner
 * of suppress_render, not something to call per update. Honours suppress_render
 * (through fbtext_render_cell), so it paints nothing while that is set. */
void fbtext_render_all(fbtext_state_t* s);
/* Blank every cell to a space in the DEFAULT colors -- not the current cur_fg /
 * cur_bg -- home the cursor, and repaint. */
void fbtext_clear(fbtext_state_t* s);
/* Scroll the grid up by `n` rows, blanking the vacated bottom rows in the
 * default colors and leaving the cursor where it was. n == 0 does nothing; n >=
 * rows degenerates to fbtext_clear, which also homes the cursor. Only the
 * vacated rows are re-rendered: the pixels above them are moved with a straight
 * copy, which is why this is cheaper than a full repaint. */
void fbtext_scroll_up(fbtext_state_t* s, uint16_t n);
/* Write one character at the cursor in the current cur_fg/cur_bg, advancing the
 * cursor and scrolling by one row when it runs past the last row.
 *
 * Interprets four control codes and nothing else: CR homes the column, LF starts
 * a new line, BS erases the cell to its left (stopping at column 0), and TAB
 * fills spaces to the next 8-column stop, clamped to the last column. Any other
 * codepoint outside the font's 0x20-0x7E range is stored as '?', so the
 * substitution is visible in the grid rather than only on screen. */
void fbtext_put_char(fbtext_state_t* s, uint32_t ch);

#endif /* FBTEXT_INTERNAL_H */