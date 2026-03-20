#ifndef FBTEXT_INTERNAL_H
#define FBTEXT_INTERNAL_H

#include <stdint.h>

/*
 * Internal types for the framebuffer text layer.
 * Not part of the public driver ABI; used only inside the framebuffer driver.
 */

#define FONT_W     8
#define FONT_H     16
#define FONT_SCALE 1                    /* pixel scale: each font pixel → NxN screen pixels */
#define CELL_W     (FONT_W * FONT_SCALE)
#define CELL_H     (FONT_H * FONT_SCALE)

/* Maximum grid dimensions.  Sized for 1280×1024 / 8×16 = 160×64. */
#define FBTEXT_MAX_COLS 160
#define FBTEXT_MAX_ROWS 64

/* CGA-style default colors. */
#define FBTEXT_DEFAULT_FG 15   /* bright white */
#define FBTEXT_DEFAULT_BG  0   /* black        */

typedef struct {
    uint32_t ch;    /* Unicode codepoint; Phase 1: ASCII only; 0 = empty */
    uint8_t  fg;    /* 4-bit foreground palette index */
    uint8_t  bg;    /* 4-bit background palette index */
    uint8_t  attr;  /* reserved for bold/underline/blink */
    uint8_t  _pad;
} fbtext_cell_t;    /* 8 bytes */

typedef struct {
    uint16_t col;
    uint16_t row;
} fbtext_cursor_t;

typedef struct {
    uint32_t       *fb;         /* mapped framebuffer base pointer */
    uint32_t        fb_stride;  /* pixels per scanline */
    uint16_t        cols;       /* actual grid width  */
    uint16_t        rows;       /* actual grid height */
    fbtext_cursor_t cursor;
    uint8_t         cur_fg;     /* current foreground palette index */
    uint8_t         cur_bg;     /* current background palette index */
    uint32_t        palette[16];
    fbtext_cell_t   cells[FBTEXT_MAX_ROWS * FBTEXT_MAX_COLS];
} fbtext_state_t;

/* render.c API */
void fbtext_render_init(fbtext_state_t *s,
                        uint32_t *fb, uint32_t stride,
                        uint32_t width, uint32_t height);
void fbtext_render_cell(fbtext_state_t *s, uint16_t col, uint16_t row);
void fbtext_render_all(fbtext_state_t *s);
void fbtext_clear(fbtext_state_t *s);
void fbtext_scroll_up(fbtext_state_t *s, uint16_t n);
void fbtext_put_char(fbtext_state_t *s, uint32_t ch);

#endif /* FBTEXT_INTERNAL_H */