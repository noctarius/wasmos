#include "fbtext_internal.h"
#include "font_8x16.h"

/* Freestanding native driver: no libc to link against.  Provide the two
 * string primitives we need inline rather than pulling in a full string.h. */
static void *
nd_memset(void *dst, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) { *p++ = (unsigned char)c; }
    return dst;
}

static void *
nd_memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) { *d++ = *s++; }
    } else if (d > s) {
        d += n; s += n;
        while (n--) { *--d = *--s; }
    }
    return dst;
}

/*
 * Framebuffer text rendering — cell grid, font blitting, scroll.
 *
 * All pixel writes go directly to the mapped framebuffer.  No kernel calls
 * are made from this file; it only operates on the fbtext_state_t passed in.
 */

/* Standard CGA 16-color palette in 0x00RRGGBB order. */
static const uint32_t cga_palette[16] = {
    0x00000000, /* 0  black          */
    0x000000AA, /* 1  blue           */
    0x0000AA00, /* 2  green          */
    0x0000AAAA, /* 3  cyan           */
    0x00AA0000, /* 4  red            */
    0x00AA00AA, /* 5  magenta        */
    0x00AA5500, /* 6  brown          */
    0x00AAAAAA, /* 7  light grey     */
    0x00555555, /* 8  dark grey      */
    0x005555FF, /* 9  light blue     */
    0x0055FF55, /* 10 light green    */
    0x0055FFFF, /* 11 light cyan     */
    0x00FF5555, /* 12 light red      */
    0x00FF55FF, /* 13 light magenta  */
    0x00FFFF55, /* 14 yellow         */
    0x00FFFFFF, /* 15 white          */
};

void
fbtext_render_init(fbtext_state_t *s,
                   uint32_t *fb, uint32_t stride,
                   uint32_t width, uint32_t height)
{
    s->fb        = fb;
    s->fb_stride = stride;
    s->cols      = (uint16_t)(width  / FONT_W);
    s->rows      = (uint16_t)(height / FONT_H);
    if (s->cols > FBTEXT_MAX_COLS) { s->cols = FBTEXT_MAX_COLS; }
    if (s->rows > FBTEXT_MAX_ROWS) { s->rows = FBTEXT_MAX_ROWS; }
    s->cursor.col = 0;
    s->cursor.row = 0;
    s->cur_fg     = FBTEXT_DEFAULT_FG;
    s->cur_bg     = FBTEXT_DEFAULT_BG;
    for (int i = 0; i < 16; i++) {
        s->palette[i] = cga_palette[i];
    }
    nd_memset(s->cells, 0, sizeof(s->cells));
}

/* Blit a single cell to the framebuffer. */
void
fbtext_render_cell(fbtext_state_t *s, uint16_t col, uint16_t row)
{
    if (col >= s->cols || row >= s->rows) {
        return;
    }
    const fbtext_cell_t *cell = &s->cells[row * s->cols + col];
    uint32_t fg = s->palette[cell->fg & 0xF];
    uint32_t bg = s->palette[cell->bg & 0xF];

    uint32_t ch = cell->ch;
    if (ch < 0x20 || ch > 0x7E) {
        ch = ' ';
    }
    const uint8_t *glyph = font_8x16[ch - 0x20];

    uint32_t x0 = (uint32_t)col * FONT_W;
    uint32_t y0 = (uint32_t)row * FONT_H;

    for (int y = 0; y < FONT_H; y++) {
        uint8_t bits = glyph[y];
        uint32_t *line = s->fb + (y0 + (uint32_t)y) * s->fb_stride + x0;
        for (int x = 0; x < FONT_W; x++) {
            line[x] = (bits & (0x80u >> x)) ? fg : bg;
        }
    }
}

/* Repaint the entire grid. */
void
fbtext_render_all(fbtext_state_t *s)
{
    for (uint16_t r = 0; r < s->rows; r++) {
        for (uint16_t c = 0; c < s->cols; c++) {
            fbtext_render_cell(s, c, r);
        }
    }
}

/* Clear all cells and repaint. */
void
fbtext_clear(fbtext_state_t *s)
{
    for (int i = 0; i < s->rows * s->cols; i++) {
        s->cells[i].ch   = ' ';
        s->cells[i].fg   = FBTEXT_DEFAULT_FG;
        s->cells[i].bg   = FBTEXT_DEFAULT_BG;
        s->cells[i].attr = 0;
    }
    s->cursor.col = 0;
    s->cursor.row = 0;
    fbtext_render_all(s);
}

/* Scroll up by n rows: shift cell buffer, clear bottom n rows.
 *
 * Uses a pixel-level memmove on the framebuffer so only the vacated bottom
 * rows need to be re-rendered, rather than repainting the entire grid. */
void
fbtext_scroll_up(fbtext_state_t *s, uint16_t n)
{
    if (n == 0) { return; }
    if (n >= s->rows) {
        fbtext_clear(s);
        return;
    }
    /* Shift cell buffer up. */
    nd_memmove(&s->cells[0],
               &s->cells[(int)n * s->cols],
               sizeof(fbtext_cell_t) * (unsigned long)(s->rows - n) * s->cols);
    /* Clear the vacated bottom rows in the cell buffer. */
    for (int i = (s->rows - n) * s->cols; i < s->rows * s->cols; i++) {
        s->cells[i].ch   = ' ';
        s->cells[i].fg   = FBTEXT_DEFAULT_FG;
        s->cells[i].bg   = FBTEXT_DEFAULT_BG;
        s->cells[i].attr = 0;
    }
    /* Pixel-level: shift framebuffer rows up by n*FONT_H scan lines. */
    unsigned long move_lines  = (unsigned long)(s->rows - n) * FONT_H;
    unsigned long clear_lines = (unsigned long)n * FONT_H;
    nd_memmove(s->fb,
               s->fb + (unsigned long)n * FONT_H * s->fb_stride,
               move_lines * s->fb_stride * sizeof(uint32_t));
    /* Re-render only the vacated bottom n rows. */
    for (uint16_t r = (uint16_t)(s->rows - n); r < s->rows; r++) {
        for (uint16_t c = 0; c < s->cols; c++) {
            fbtext_render_cell(s, c, r);
        }
    }
    (void)clear_lines;
}

/*
 * Write one character at the current cursor position, handling control codes.
 * Advances the cursor and scrolls as needed.
 */
void
fbtext_put_char(fbtext_state_t *s, uint32_t ch)
{
    switch (ch) {
    case '\r':
        s->cursor.col = 0;
        return;
    case '\n':
        s->cursor.col = 0;
        s->cursor.row++;
        if (s->cursor.row >= s->rows) {
            fbtext_scroll_up(s, 1);
            s->cursor.row = s->rows - 1;
        }
        return;
    case '\b':
        if (s->cursor.col > 0) {
            s->cursor.col--;
            s->cells[s->cursor.row * s->cols + s->cursor.col].ch   = ' ';
            s->cells[s->cursor.row * s->cols + s->cursor.col].fg   = s->cur_fg;
            s->cells[s->cursor.row * s->cols + s->cursor.col].bg   = s->cur_bg;
            fbtext_render_cell(s, s->cursor.col, s->cursor.row);
        }
        return;
    case '\t': {
        /* Advance to next 8-column tab stop. */
        uint16_t next = (uint16_t)((s->cursor.col + 8) & ~7u);
        if (next >= s->cols) { next = s->cols - 1; }
        while (s->cursor.col < next) {
            fbtext_cell_t *cell = &s->cells[s->cursor.row * s->cols + s->cursor.col];
            cell->ch  = ' ';
            cell->fg  = s->cur_fg;
            cell->bg  = s->cur_bg;
            fbtext_render_cell(s, s->cursor.col, s->cursor.row);
            s->cursor.col++;
        }
        return;
    }
    default:
        break;
    }

    /* Printable (or replacement) character. */
    if (ch < 0x20 || ch > 0x7E) {
        ch = '?';
    }
    fbtext_cell_t *cell = &s->cells[s->cursor.row * s->cols + s->cursor.col];
    cell->ch   = ch;
    cell->fg   = s->cur_fg;
    cell->bg   = s->cur_bg;
    cell->attr = 0;
    fbtext_render_cell(s, s->cursor.col, s->cursor.row);

    s->cursor.col++;
    if (s->cursor.col >= s->cols) {
        s->cursor.col = 0;
        s->cursor.row++;
        if (s->cursor.row >= s->rows) {
            fbtext_scroll_up(s, 1);
            s->cursor.row = s->rows - 1;
        }
    }
}