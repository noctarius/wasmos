#include <stdint.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

typedef enum {
    ESC_NORMAL = 0,
    ESC_ESC,
    ESC_CSI,
} esc_state_t;

typedef struct {
    uint32_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t _pad;
} vt_cell_t;

typedef struct {
    uint16_t cursor_row;
    uint16_t cursor_col;
    uint16_t cursor_saved_row;
    uint16_t cursor_saved_col;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t cursor_visible;
    uint8_t cursor_saved_valid;
    uint8_t input_echo;
    uint8_t input_canonical;
    esc_state_t esc;
    uint16_t input_q_head;
    uint16_t input_q_tail;
    uint16_t input_line_len;
    uint16_t input_line_cursor;
    uint8_t input_history_count;
    uint8_t input_history_head;
    int8_t input_history_nav;
    uint8_t input_q[256];
    uint8_t input_line[128];
    uint8_t input_history[8][128];
    uint8_t input_history_len[8];
    uint16_t csi_params[8];
    uint8_t csi_count;
    uint16_t csi_current;
    uint8_t csi_have_current;
    uint8_t csi_private;
    vt_cell_t cells[80u * 25u];
} vt_tty_t;

#define VT_MAX_TTYS 4u
#define VT_COLS_DEFAULT 80u
#define VT_ROWS_DEFAULT 25u
/* Keep in sync with kernel ipc.h */
#define IPC_ERR_FULL (-3)
#define VT_FB_SEND_RETRIES 1024

#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

static int32_t  g_vt_ep = -1;
static int32_t  g_kbd_ep = -1;
static int32_t  g_fb_ep = -1;
static vt_tty_t g_ttys[VT_MAX_TTYS];
static uint32_t g_active_tty = 0;
static int32_t  g_tty_reader_ep[VT_MAX_TTYS] = { -1, -1, -1, -1 };
static int32_t  g_tty_writer_ep[VT_MAX_TTYS] = { -1, -1, -1, -1 };
static uint32_t g_switch_generation = 1;
static uint8_t  g_switch_barrier = 0;
static uint8_t  g_ctrl_down = 0;
static uint8_t  g_shift_down = 0;

enum {
    VT_TRACE_SWITCH = 0xA1,
    VT_TRACE_WRITER_OK = 0xA2,
    VT_TRACE_WRITER_CONFLICT = 0xA3,
    VT_TRACE_DROP_UNOWNED = 0xA4,
    VT_TRACE_DROP_STALE = 0xA5
};

static void
vt_trace_mark(uint8_t event, uint16_t a, uint16_t b)
{
#if WASMOS_TRACE
    uint32_t tag = ((uint32_t)event << 24) |
                   (((uint32_t)(a & 0x0FFFu)) << 12) |
                   (uint32_t)(b & 0x0FFFu);
    (void)wasmos_debug_mark((int32_t)tag);
#else
    (void)event;
    (void)a;
    (void)b;
#endif
}

static uint32_t
vt_cell_index(uint16_t row, uint16_t col)
{
    return (uint32_t)row * VT_COLS_DEFAULT + (uint32_t)col;
}

static void
vt_render_cell(const vt_tty_t *tty, uint16_t row, uint16_t col);

static int
vt_bytes_equal(const uint8_t *a, const uint8_t *b, uint16_t len)
{
    if (!a || !b) {
        return 0;
    }
    for (uint16_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint16_t
vt_input_q_next(uint16_t v)
{
    return (uint16_t)((v + 1u) & 0xFFu);
}

static int
vt_input_q_push(vt_tty_t *tty, uint8_t ch)
{
    if (!tty) {
        return -1;
    }
    uint16_t next = vt_input_q_next(tty->input_q_tail);
    if (next == tty->input_q_head) {
        return -1;
    }
    tty->input_q[tty->input_q_tail] = ch;
    tty->input_q_tail = next;
    return 0;
}

static int
vt_input_q_pop(vt_tty_t *tty, uint8_t *out_ch)
{
    if (!tty || !out_ch) {
        return -1;
    }
    if (tty->input_q_head == tty->input_q_tail) {
        return -1;
    }
    *out_ch = tty->input_q[tty->input_q_head];
    tty->input_q_head = vt_input_q_next(tty->input_q_head);
    return 0;
}

static void
vt_input_q_push_escape(vt_tty_t *tty, uint8_t final)
{
    if (!tty) {
        return;
    }
    (void)vt_input_q_push(tty, 0x1B); /* ESC */
    (void)vt_input_q_push(tty, '[');
    (void)vt_input_q_push(tty, final);
}

static int
vt_fb_send(uint32_t type,
           int32_t arg0,
           int32_t arg1,
           int32_t arg2,
           int32_t arg3)
{
    if (g_fb_ep < 0 || g_vt_ep < 0) {
        return -1;
    }
    uint32_t tries = 0;
    for (;;) {
        int32_t rc = wasmos_ipc_send(g_fb_ep, g_vt_ep, (int32_t)type, 0,
                                     arg0, arg1, arg2, arg3);
        if (rc == 0) {
            return 0;
        }
        if (rc != IPC_ERR_FULL) {
            return rc;
        }
        /* FIXME: persistent queue-full means framebuffer service is stalled;
         * drop this update so VT input handling stays responsive. */
        if (++tries >= VT_FB_SEND_RETRIES) {
            return IPC_ERR_FULL;
        }
        (void)wasmos_sched_yield();
    }
}

static void
vt_fb_set_cursor(const vt_tty_t *tty)
{
    if (!tty || !tty->cursor_visible) {
        return;
    }
    (void)vt_fb_send(FBTEXT_IPC_CURSOR_SET_REQ,
                     (int32_t)tty->cursor_col,
                     (int32_t)tty->cursor_row,
                     0,
                     0);
}

static void
vt_fb_console_mode(uint8_t enabled)
{
    (void)vt_fb_send(FBTEXT_IPC_CONSOLE_MODE_REQ,
                     enabled ? 1 : 0,
                     0,
                     0,
                     0);
}

static void
vt_store_cell(vt_tty_t *tty, uint16_t row, uint16_t col, uint32_t ch)
{
    if (!tty || row >= VT_ROWS_DEFAULT || col >= VT_COLS_DEFAULT) {
        return;
    }
    uint32_t idx = vt_cell_index(row, col);
    tty->cells[idx].ch = ch;
    tty->cells[idx].fg = tty->fg;
    tty->cells[idx].bg = tty->bg;
    tty->cells[idx].attr = tty->attr;
}

static uint16_t
vt_clamp_row(int32_t row)
{
    if (row < 0) {
        return 0;
    }
    if (row >= (int32_t)VT_ROWS_DEFAULT) {
        return (uint16_t)(VT_ROWS_DEFAULT - 1u);
    }
    return (uint16_t)row;
}

static uint16_t
vt_clamp_col(int32_t col)
{
    if (col < 0) {
        return 0;
    }
    if (col >= (int32_t)VT_COLS_DEFAULT) {
        return (uint16_t)(VT_COLS_DEFAULT - 1u);
    }
    return (uint16_t)col;
}

static void
vt_csi_reset(vt_tty_t *tty)
{
    if (!tty) {
        return;
    }
    tty->csi_count = 0;
    tty->csi_current = 0;
    tty->csi_have_current = 0;
    tty->csi_private = 0;
}

static void
vt_csi_push_param(vt_tty_t *tty)
{
    if (!tty) {
        return;
    }
    if (tty->csi_count >= sizeof(tty->csi_params) / sizeof(tty->csi_params[0])) {
        tty->csi_current = 0;
        tty->csi_have_current = 0;
        return;
    }
    tty->csi_params[tty->csi_count++] = tty->csi_current;
    tty->csi_current = 0;
    tty->csi_have_current = 0;
}

static uint16_t
vt_csi_param(vt_tty_t *tty, uint8_t index, uint16_t def)
{
    if (!tty) {
        return def;
    }
    if (index < tty->csi_count) {
        uint16_t v = tty->csi_params[index];
        return v == 0 ? def : v;
    }
    return def;
}

static void
vt_clear_cell(vt_tty_t *tty, uint16_t row, uint16_t col, uint8_t render_now)
{
    if (!tty || row >= VT_ROWS_DEFAULT || col >= VT_COLS_DEFAULT) {
        return;
    }
    vt_store_cell(tty, row, col, ' ');
    if (render_now) {
        vt_render_cell(tty, row, col);
    }
}

static void
vt_apply_sgr(vt_tty_t *tty, uint16_t code)
{
    if (!tty) {
        return;
    }
    if (code == 0) {
        tty->fg = 15;
        tty->bg = 0;
        tty->attr = 0;
        return;
    }
    if (code == 1) {
        tty->attr |= 0x01u;
        return;
    }
    if (code == 22) {
        tty->attr &= (uint8_t)~0x01u;
        return;
    }
    if (code >= 30 && code <= 37) {
        tty->fg = (uint8_t)(code - 30);
        return;
    }
    if (code >= 90 && code <= 97) {
        tty->fg = (uint8_t)(8 + (code - 90));
        return;
    }
    if (code == 39) {
        tty->fg = 15;
        return;
    }
    if (code >= 40 && code <= 47) {
        tty->bg = (uint8_t)(code - 40);
        return;
    }
    if (code >= 100 && code <= 107) {
        tty->bg = (uint8_t)(8 + (code - 100));
        return;
    }
    if (code == 49) {
        tty->bg = 0;
        return;
    }
}

static void
vt_apply_private_csi(uint32_t tty_index, vt_tty_t *tty, uint8_t final)
{
    if (!tty || (final != 'h' && final != 'l')) {
        return;
    }

    uint8_t has_cursor_param = 0;
    for (uint8_t i = 0; i < tty->csi_count; ++i) {
        if (tty->csi_params[i] == 25u) {
            has_cursor_param = 1u;
            break;
        }
    }
    if (!has_cursor_param) {
        return;
    }

    tty->cursor_visible = (final == 'h') ? 1u : 0u;
    if ((tty_index != 0u) &&
        (tty_index == g_active_tty) &&
        !g_switch_barrier &&
        tty->cursor_visible) {
        vt_fb_set_cursor(tty);
    }
}

static void
vt_apply_csi(uint32_t tty_index, vt_tty_t *tty, uint8_t final)
{
    if (!tty) {
        return;
    }
    uint8_t render_now = (tty_index != 0) &&
                         (tty_index == g_active_tty) &&
                         !g_switch_barrier;
    switch (final) {
    case 'A': {
        uint16_t n = vt_csi_param(tty, 0, 1);
        tty->cursor_row = vt_clamp_row((int32_t)tty->cursor_row - (int32_t)n);
        break;
    }
    case 'B': {
        uint16_t n = vt_csi_param(tty, 0, 1);
        tty->cursor_row = vt_clamp_row((int32_t)tty->cursor_row + (int32_t)n);
        break;
    }
    case 'C': {
        uint16_t n = vt_csi_param(tty, 0, 1);
        tty->cursor_col = vt_clamp_col((int32_t)tty->cursor_col + (int32_t)n);
        break;
    }
    case 'D': {
        uint16_t n = vt_csi_param(tty, 0, 1);
        tty->cursor_col = vt_clamp_col((int32_t)tty->cursor_col - (int32_t)n);
        break;
    }
    case 'H':
    case 'f': {
        uint16_t row = vt_csi_param(tty, 0, 1);
        uint16_t col = vt_csi_param(tty, 1, 1);
        tty->cursor_row = vt_clamp_row((int32_t)row - 1);
        tty->cursor_col = vt_clamp_col((int32_t)col - 1);
        break;
    }
    case 'J': {
        uint16_t mode = (tty->csi_count > 0) ? tty->csi_params[0] : 0;
        if (mode == 2) {
            for (uint16_t r = 0; r < VT_ROWS_DEFAULT; ++r) {
                for (uint16_t c = 0; c < VT_COLS_DEFAULT; ++c) {
                    vt_clear_cell(tty, r, c, render_now);
                }
            }
        } else if (mode == 1) {
            for (uint16_t r = 0; r <= tty->cursor_row; ++r) {
                uint16_t max_col = (r == tty->cursor_row) ? tty->cursor_col : (uint16_t)(VT_COLS_DEFAULT - 1u);
                for (uint16_t c = 0; c <= max_col; ++c) {
                    vt_clear_cell(tty, r, c, render_now);
                }
            }
        } else {
            for (uint16_t r = tty->cursor_row; r < VT_ROWS_DEFAULT; ++r) {
                uint16_t start_col = (r == tty->cursor_row) ? tty->cursor_col : 0;
                for (uint16_t c = start_col; c < VT_COLS_DEFAULT; ++c) {
                    vt_clear_cell(tty, r, c, render_now);
                }
            }
        }
        break;
    }
    case 'K': {
        uint16_t mode = (tty->csi_count > 0) ? tty->csi_params[0] : 0;
        if (mode == 2) {
            for (uint16_t c = 0; c < VT_COLS_DEFAULT; ++c) {
                vt_clear_cell(tty, tty->cursor_row, c, render_now);
            }
        } else if (mode == 1) {
            for (uint16_t c = 0; c <= tty->cursor_col; ++c) {
                vt_clear_cell(tty, tty->cursor_row, c, render_now);
            }
        } else {
            for (uint16_t c = tty->cursor_col; c < VT_COLS_DEFAULT; ++c) {
                vt_clear_cell(tty, tty->cursor_row, c, render_now);
            }
        }
        break;
    }
    case 'm': {
        if (tty->csi_count == 0) {
            vt_apply_sgr(tty, 0);
            break;
        }
        for (uint8_t i = 0; i < tty->csi_count; ++i) {
            vt_apply_sgr(tty, tty->csi_params[i]);
        }
        break;
    }
    case 's':
        tty->cursor_saved_row = tty->cursor_row;
        tty->cursor_saved_col = tty->cursor_col;
        tty->cursor_saved_valid = 1u;
        break;
    case 'u':
        if (tty->cursor_saved_valid) {
            tty->cursor_row = tty->cursor_saved_row;
            tty->cursor_col = tty->cursor_saved_col;
        }
        break;
    default:
        break;
    }

    if (render_now) {
        vt_fb_set_cursor(tty);
    }
}

static void
vt_render_cell(const vt_tty_t *tty, uint16_t row, uint16_t col)
{
    if (!tty || row >= VT_ROWS_DEFAULT || col >= VT_COLS_DEFAULT) {
        return;
    }
    uint32_t idx = vt_cell_index(row, col);
    const vt_cell_t *cell = &tty->cells[idx];
    uint32_t packed = ((uint32_t)(cell->fg & 0x0Fu) << 8) |
                      (uint32_t)(cell->bg & 0x0Fu);
    (void)vt_fb_send(FBTEXT_IPC_CELL_WRITE_REQ,
                     (int32_t)col,
                     (int32_t)row,
                     (int32_t)cell->ch,
                     (int32_t)packed);
}

static void
vt_scroll_up(vt_tty_t *tty, uint8_t render_now)
{
    if (!tty) {
        return;
    }

    for (uint16_t row = 1; row < VT_ROWS_DEFAULT; ++row) {
        for (uint16_t col = 0; col < VT_COLS_DEFAULT; ++col) {
            uint32_t dst = vt_cell_index((uint16_t)(row - 1u), col);
            uint32_t src = vt_cell_index(row, col);
            tty->cells[dst] = tty->cells[src];
        }
    }
    for (uint16_t col = 0; col < VT_COLS_DEFAULT; ++col) {
        uint32_t idx = vt_cell_index((uint16_t)(VT_ROWS_DEFAULT - 1u), col);
        tty->cells[idx].ch = ' ';
        tty->cells[idx].fg = tty->fg;
        tty->cells[idx].bg = tty->bg;
        tty->cells[idx].attr = tty->attr;
    }

    if (render_now) {
        (void)vt_fb_send(FBTEXT_IPC_SCROLL_REQ, 1, 0, 0, 0);
    }
}

static void
vt_put_char_tty0(vt_tty_t *tty, uint8_t ch)
{
    char c = (char)ch;
    if (tty) {
        if (c == '\r') {
            tty->cursor_col = 0;
        } else if (c == '\n') {
            tty->cursor_col = 0;
            if (tty->cursor_row + 1u >= VT_ROWS_DEFAULT) {
                vt_scroll_up(tty, 0);
                tty->cursor_row = (uint16_t)(VT_ROWS_DEFAULT - 1u);
            } else {
                tty->cursor_row++;
            }
        } else if (c == '\b') {
            if (tty->cursor_col > 0) {
                tty->cursor_col--;
            }
            vt_store_cell(tty, tty->cursor_row, tty->cursor_col, ' ');
        } else {
            vt_store_cell(tty, tty->cursor_row, tty->cursor_col, (uint32_t)c);
            tty->cursor_col++;
            if (tty->cursor_col >= VT_COLS_DEFAULT) {
                tty->cursor_col = 0;
                if (tty->cursor_row + 1u >= VT_ROWS_DEFAULT) {
                    vt_scroll_up(tty, 0);
                    tty->cursor_row = (uint16_t)(VT_ROWS_DEFAULT - 1u);
                } else {
                    tty->cursor_row++;
                }
            }
        }
    }
    (void)wasmos_console_write((int32_t)(uintptr_t)&c, 1);
}

static void
vt_put_char_virtual(vt_tty_t *tty, uint32_t tty_index, uint8_t ch)
{
    if (!tty) {
        return;
    }
    uint8_t render_now = (tty_index == g_active_tty) && !g_switch_barrier;

    if (ch == '\r') {
        tty->cursor_col = 0;
        if (render_now) {
            vt_fb_set_cursor(tty);
        }
        return;
    }
    if (ch == '\n') {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1u >= VT_ROWS_DEFAULT) {
            vt_scroll_up(tty, render_now);
            tty->cursor_row = (uint16_t)(VT_ROWS_DEFAULT - 1u);
        } else {
            tty->cursor_row++;
        }
        if (render_now) {
            vt_fb_set_cursor(tty);
        }
        return;
    }
    if (ch == '\b') {
        if (tty->cursor_col > 0) {
            tty->cursor_col--;
        }
        vt_store_cell(tty, tty->cursor_row, tty->cursor_col, ' ');
        if (render_now) {
            vt_render_cell(tty, tty->cursor_row, tty->cursor_col);
            vt_fb_set_cursor(tty);
        }
        return;
    }
    if (ch == '\t') {
        uint16_t next = (uint16_t)((tty->cursor_col + 8u) & ~7u);
        while (tty->cursor_col < next) {
            vt_put_char_virtual(tty, tty_index, ' ');
        }
        return;
    }

    vt_store_cell(tty, tty->cursor_row, tty->cursor_col, (uint32_t)ch);
    if (render_now) {
        vt_render_cell(tty, tty->cursor_row, tty->cursor_col);
    }

    tty->cursor_col++;
    if (tty->cursor_col >= VT_COLS_DEFAULT) {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1u >= VT_ROWS_DEFAULT) {
            vt_scroll_up(tty, render_now);
            tty->cursor_row = (uint16_t)(VT_ROWS_DEFAULT - 1u);
        } else {
            tty->cursor_row++;
        }
    }
    if (render_now) {
        vt_fb_set_cursor(tty);
    }
}

static void
vt_process_byte(uint32_t tty_index, vt_tty_t *tty, uint8_t c)
{
    if (!tty) {
        return;
    }
    switch (tty->esc) {
    case ESC_NORMAL:
        if (c == 0x1B) {
            tty->esc = ESC_ESC;
        } else if (tty_index == 0) {
            vt_put_char_tty0(tty, c);
        } else {
            vt_put_char_virtual(tty, tty_index, c);
        }
        break;
    case ESC_ESC:
        if (c == '[') {
            tty->esc = ESC_CSI;
            vt_csi_reset(tty);
        } else {
            tty->esc = ESC_NORMAL;
        }
        break;
    case ESC_CSI:
        if (c == '?') {
            tty->csi_private = 1;
            break;
        }
        if (c >= '0' && c <= '9') {
            tty->csi_current = (uint16_t)(tty->csi_current * 10u + (uint16_t)(c - '0'));
            tty->csi_have_current = 1;
            break;
        }
        if (c == ';') {
            vt_csi_push_param(tty);
            break;
        }
        if (c >= 0x40 && c <= 0x7E) {
            if (tty->csi_have_current || tty->csi_count > 0) {
                vt_csi_push_param(tty);
            }
            if (tty->csi_private) {
                vt_apply_private_csi(tty_index, tty, c);
            } else {
                vt_apply_csi(tty_index, tty, c);
            }
            tty->esc = ESC_NORMAL;
            vt_csi_reset(tty);
        }
        break;
    }
}

static int32_t
vt_tty_index_for_source(int32_t source_ep)
{
    if (source_ep < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < VT_MAX_TTYS; ++i) {
        if (g_tty_writer_ep[i] == source_ep) {
            return (int32_t)i;
        }
        if (g_tty_reader_ep[i] == source_ep) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void
vt_replay_tty(uint32_t tty_index)
{
    if (tty_index >= VT_MAX_TTYS) {
        return;
    }
    vt_tty_t *tty = &g_ttys[tty_index];

    for (uint16_t row = 0; row < VT_ROWS_DEFAULT; ++row) {
        for (uint16_t col = 0; col < VT_COLS_DEFAULT; ++col) {
            vt_render_cell(tty, row, col);
        }
    }

    vt_fb_set_cursor(tty);
}

static void
vt_init_ttys(void)
{
    for (uint32_t i = 0; i < VT_MAX_TTYS; ++i) {
        g_ttys[i].cursor_row = 0;
        g_ttys[i].cursor_col = 0;
        g_ttys[i].cursor_saved_row = 0;
        g_ttys[i].cursor_saved_col = 0;
        g_ttys[i].fg = 15;
        g_ttys[i].bg = 0;
        g_ttys[i].attr = 0;
        g_ttys[i].cursor_visible = 1;
        g_ttys[i].cursor_saved_valid = 0;
        /* Input is delivered raw to the tty client; CLI owns line editing
         * and user-visible echo so serial and framebuffer behave the same. */
        g_ttys[i].input_echo = 0;
        g_ttys[i].input_canonical = 0;
        g_ttys[i].esc = ESC_NORMAL;
        g_ttys[i].input_q_head = 0;
        g_ttys[i].input_q_tail = 0;
        g_ttys[i].input_line_len = 0;
        g_ttys[i].input_line_cursor = 0;
        g_ttys[i].input_history_count = 0;
        g_ttys[i].input_history_head = 0;
        g_ttys[i].input_history_nav = -1;
        g_ttys[i].csi_count = 0;
        g_ttys[i].csi_current = 0;
        g_ttys[i].csi_have_current = 0;
        g_ttys[i].csi_private = 0;
        for (uint32_t k = 0; k < sizeof(g_ttys[i].input_q); ++k) {
            g_ttys[i].input_q[k] = 0;
        }
        for (uint32_t k = 0; k < sizeof(g_ttys[i].input_line); ++k) {
            g_ttys[i].input_line[k] = 0;
        }
        for (uint32_t h = 0; h < 8u; ++h) {
            g_ttys[i].input_history_len[h] = 0;
            for (uint32_t k = 0; k < sizeof(g_ttys[i].input_history[h]); ++k) {
                g_ttys[i].input_history[h][k] = 0;
            }
        }
        for (uint32_t j = 0; j < VT_COLS_DEFAULT * VT_ROWS_DEFAULT; ++j) {
            g_ttys[i].cells[j].ch = 0;
            g_ttys[i].cells[j].fg = 15;
            g_ttys[i].cells[j].bg = 0;
            g_ttys[i].cells[j].attr = 0;
            g_ttys[i].cells[j]._pad = 0;
        }
    }
    g_active_tty = 0;
    g_switch_generation = 1;
    g_switch_barrier = 0;
}

static int32_t
vt_switch_tty(uint32_t tty_index)
{
    if (tty_index >= VT_MAX_TTYS) {
        return -1;
    }

    g_switch_barrier = 1;
    g_switch_generation++;
    g_active_tty = tty_index;
    vt_trace_mark(VT_TRACE_SWITCH,
                  (uint16_t)(tty_index & 0x0FFFu),
                  (uint16_t)(g_switch_generation & 0x0FFFu));

    /* Allow logical tty switching even when framebuffer control is unavailable
     * (startup races/headless mode). This keeps VT/CLI state consistent and
     * avoids reporting spurious switch failures to user-space. */
    if (g_fb_ep < 0) {
        g_switch_barrier = 0;
        return 0;
    }

    if (tty_index == 0) {
        /* Keep ring output paused until replay is complete. */
        vt_fb_console_mode(0);
        (void)vt_fb_send(FBTEXT_IPC_CLEAR_REQ, 0, 0, 0, 0);
        vt_replay_tty(tty_index);
        vt_fb_console_mode(1);
        g_switch_barrier = 0;
        return 0;
    }

    /* Disable ring first to avoid immediate repaint races from tty0 output. */
    vt_fb_console_mode(0);
    (void)vt_fb_send(FBTEXT_IPC_CLEAR_REQ, 0, 0, 0, 0);
    /* FIXME: replay currently repaints the full 80x25 virtual grid even if
     * only a small number of cells changed. */
    vt_replay_tty(tty_index);
    g_switch_barrier = 0;
    return 0;
}

static const uint8_t g_sc_to_ascii[58] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0, '*', 0, ' '
};

static void
vt_input_echo_char(uint32_t tty_index, uint8_t ch)
{
    if (tty_index >= VT_MAX_TTYS || tty_index != g_active_tty) {
        return;
    }
    vt_tty_t *tty = &g_ttys[tty_index];
    vt_process_byte(tty_index, tty, ch);
}

static void
vt_input_commit_line(vt_tty_t *tty)
{
    if (!tty) {
        return;
    }
    for (uint16_t i = 0; i < tty->input_line_len; ++i) {
        (void)vt_input_q_push(tty, tty->input_line[i]);
    }
    (void)vt_input_q_push(tty, '\n');
    tty->input_line_len = 0;
    tty->input_line_cursor = 0;
    tty->input_history_nav = -1;
}

static void
vt_input_history_store(vt_tty_t *tty)
{
    if (!tty || tty->input_line_len == 0) {
        return;
    }

    if (tty->input_history_count > 0) {
        uint8_t newest = (uint8_t)((tty->input_history_head + 8u - 1u) % 8u);
        uint8_t newest_len = tty->input_history_len[newest];
        if (newest_len == (uint8_t)tty->input_line_len &&
            vt_bytes_equal(tty->input_history[newest], tty->input_line, tty->input_line_len)) {
            return;
        }
    }

    uint8_t slot = tty->input_history_head;
    tty->input_history_len[slot] = (uint8_t)tty->input_line_len;
    for (uint16_t i = 0; i < tty->input_line_len; ++i) {
        tty->input_history[slot][i] = tty->input_line[i];
    }
    tty->input_history_head = (uint8_t)((tty->input_history_head + 1u) % 8u);
    if (tty->input_history_count < 8u) {
        tty->input_history_count++;
    }
}

static void
vt_input_replace_line(uint32_t tty_index, vt_tty_t *tty, const uint8_t *line, uint16_t len)
{
    if (!tty) {
        return;
    }

    while (tty->input_line_len > 0) {
        tty->input_line_len--;
        tty->input_line_cursor = tty->input_line_len;
        if (tty->input_echo) {
            vt_input_echo_char(tty_index, '\b');
        }
    }
    for (uint16_t i = 0; i < len; ++i) {
        tty->input_line[i] = line[i];
        tty->input_line_len++;
        tty->input_line_cursor = tty->input_line_len;
        if (tty->input_echo) {
            vt_input_echo_char(tty_index, line[i]);
        }
    }
}

static void
vt_input_history_nav(uint32_t tty_index, vt_tty_t *tty, uint8_t older)
{
    if (!tty || tty->input_history_count == 0) {
        return;
    }

    int16_t nav = tty->input_history_nav;
    if (older) {
        if (nav + 1 >= (int16_t)tty->input_history_count) {
            return;
        }
        nav++;
    } else {
        if (nav < 0) {
            return;
        }
        nav--;
    }

    if (nav < 0) {
        tty->input_history_nav = -1;
        vt_input_replace_line(tty_index, tty, 0, 0);
        return;
    }

    tty->input_history_nav = (int8_t)nav;
    uint8_t newest = (uint8_t)((tty->input_history_head + 8u - 1u) % 8u);
    uint8_t slot = (uint8_t)((newest + 8u - (uint8_t)nav) % 8u);
    uint16_t len = tty->input_history_len[slot];
    vt_input_replace_line(tty_index, tty, tty->input_history[slot], len);
}

static void
vt_input_handle_char(uint32_t tty_index, uint8_t ch)
{
    if (tty_index >= VT_MAX_TTYS) {
        return;
    }
    vt_tty_t *tty = &g_ttys[tty_index];
    if (tty->input_canonical) {
        if (ch == 0x03) { /* Ctrl+C */
            tty->input_line_len = 0;
            tty->input_line_cursor = 0;
            tty->input_history_nav = -1;
            if (tty->input_echo) {
                vt_input_echo_char(tty_index, '^');
                vt_input_echo_char(tty_index, 'C');
                vt_input_echo_char(tty_index, '\n');
            }
            (void)vt_input_q_push(tty, ch);
            return;
        }
        if (ch == 0x15) { /* Ctrl+U */
            while (tty->input_line_len > 0) {
                tty->input_line_len--;
                tty->input_line_cursor = tty->input_line_len;
                if (tty->input_echo) {
                    vt_input_echo_char(tty_index, '\b');
                }
            }
            tty->input_history_nav = -1;
            return;
        }
        if (ch == 0x10) { /* Ctrl+P => previous history */
            vt_input_history_nav(tty_index, tty, 1);
            return;
        }
        if (ch == 0x0E) { /* Ctrl+N => next history */
            vt_input_history_nav(tty_index, tty, 0);
            return;
        }
        if (ch == '\r' || ch == '\n') {
            if (tty->input_echo) {
                vt_input_echo_char(tty_index, '\n');
            }
            vt_input_history_store(tty);
            vt_input_commit_line(tty);
            return;
        }
        if (ch == '\b' || ch == 0x7F) {
            if (tty->input_line_len > 0) {
                tty->input_line_len--;
                tty->input_line_cursor = tty->input_line_len;
                if (tty->input_echo) {
                    vt_input_echo_char(tty_index, '\b');
                }
            }
            tty->input_history_nav = -1;
            return;
        }
        if (ch < 0x20 || ch > 0x7E) {
            return;
        }
        if (tty->input_line_len + 1u >= (uint16_t)sizeof(tty->input_line)) {
            return;
        }
        tty->input_line[tty->input_line_len++] = ch;
        tty->input_line_cursor = tty->input_line_len;
        tty->input_history_nav = -1;
        if (tty->input_echo) {
            vt_input_echo_char(tty_index, ch);
        }
        return;
    }
    (void)vt_input_q_push(tty, ch);
    if (tty->input_echo) {
        vt_input_echo_char(tty_index, ch);
    }
}

static void
vt_set_input_mode(vt_tty_t *tty, uint8_t mode)
{
    if (!tty) {
        return;
    }
    tty->input_canonical = (mode & VT_INPUT_MODE_CANONICAL) ? 1 : 0;
    tty->input_echo = (mode & VT_INPUT_MODE_ECHO) ? 1 : 0;
    if (!tty->input_canonical) {
        tty->input_line_len = 0;
        tty->input_line_cursor = 0;
        tty->input_history_nav = -1;
    }
}

static void
vt_handle_key_notify(int32_t scancode, int32_t keyup, int32_t extended)
{
    if (scancode == 0x1D) { /* Ctrl (left + extended right) */
        g_ctrl_down = keyup ? 0 : 1;
        return;
    }
    if (scancode == 0x2A || scancode == 0x36) { /* Left/Right Shift */
        g_shift_down = keyup ? 0 : 1;
        return;
    }

    if (keyup != 0) {
        return;
    }

    if (extended && g_active_tty != 0) {
        vt_tty_t *tty = &g_ttys[g_active_tty];
        /* Extended set-1 arrows: Up=0x48, Down=0x50. */
        if (tty->input_canonical) {
            if (scancode == 0x48) {
                vt_input_handle_char(g_active_tty, 0x10); /* Ctrl+P semantic */
                return;
            }
            if (scancode == 0x50) {
                vt_input_handle_char(g_active_tty, 0x0E); /* Ctrl+N semantic */
                return;
            }
            return;
        }

        if (scancode == 0x48) {       /* Up */
            vt_input_q_push_escape(tty, 'A');
            return;
        } else if (scancode == 0x50) {/* Down */
            vt_input_q_push_escape(tty, 'B');
            return;
        } else if (scancode == 0x4D) {/* Right */
            vt_input_q_push_escape(tty, 'C');
            return;
        } else if (scancode == 0x4B) {/* Left */
            vt_input_q_push_escape(tty, 'D');
            return;
        }
    }

    if (g_ctrl_down && g_shift_down) {
        /* Set 1 scancodes: F1..F4 => 0x3B..0x3E map to tty0..tty3. */
        if (scancode >= 0x3B && scancode <= 0x3E) {
            (void)vt_switch_tty((uint32_t)(scancode - 0x3B));
            return;
        }
    }

    uint8_t ch = 0;
    if (g_ctrl_down) {
        /* Minimal cooked-mode control set for line discipline. */
        if (scancode == 0x16) {       /* U */
            ch = 0x15;                /* NAK / Ctrl+U */
        } else if (scancode == 0x2E) {/* C */
            ch = 0x03;                /* ETX / Ctrl+C */
        } else if (scancode == 0x19) {/* P */
            ch = 0x10;                /* DLE / Ctrl+P */
        } else if (scancode == 0x31) {/* N */
            ch = 0x0E;                /* SO / Ctrl+N */
        }
    }
    if (ch == 0) {
        if (scancode <= 0 || scancode >= (int32_t)(sizeof(g_sc_to_ascii))) {
            return;
        }
        ch = g_sc_to_ascii[(uint32_t)scancode];
    }
    if (ch == 0) {
        return;
    }
    /* tty0 is the system console mirror; CLI sessions are tty1+. */
    if (g_active_tty == 0) {
        return;
    }
    vt_input_handle_char(g_active_tty, ch);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t fb_endpoint, int32_t kbd_endpoint, int32_t arg2, int32_t arg3)
{
    (void)arg3;

    g_fb_ep = fb_endpoint;
    g_kbd_ep = kbd_endpoint;
    vt_init_ttys();

    if (arg2 >= 0) {
        g_vt_ep = arg2;
    } else {
        g_vt_ep = wasmos_ipc_create_endpoint();
        if (g_vt_ep < 0) {
            return -1;
        }
    }

    if (g_kbd_ep >= 0) {
        (void)wasmos_ipc_send(g_kbd_ep, g_vt_ep, KBD_IPC_SUBSCRIBE_REQ,
                              1, 0, 0, 0, 0);
    }

    if (g_fb_ep >= 0) {
        vt_fb_console_mode(1);
    }

    for (;;) {
        int32_t rc = wasmos_ipc_recv(g_vt_ep);
        if (rc < 0) {
            wasmos_sched_yield();
            continue;
        }

        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);

        switch ((uint32_t)msg.type) {
        case VT_IPC_WRITE_REQ: {
            int32_t tty_index = vt_tty_index_for_source(msg.source);
            if (tty_index < 0 || tty_index >= (int32_t)VT_MAX_TTYS) {
                vt_trace_mark(VT_TRACE_DROP_UNOWNED,
                              (uint16_t)(msg.source < 0 ? 0x0FFFu : ((uint32_t)msg.source & 0x0FFFu)),
                              0);
                break;
            }
            if ((uint32_t)msg.request_id != g_switch_generation) {
                /* Drop stale write chunks queued before the last tty switch. */
                vt_trace_mark(VT_TRACE_DROP_STALE,
                              (uint16_t)((uint32_t)tty_index & 0x0FFFu),
                              (uint16_t)(((uint32_t)msg.request_id) & 0x0FFFu));
                break;
            }
            /* FIXME: Deferred follow-up. We have seen an intermittent
             * framebuffer-only artifact where rapid Ctrl+Shift+Fn switching
             * can still show duplicated/misaligned prompts. This has not
             * reproduced again in recent runs, but keep the VT trace markers
             * (switch/write-drop/register events) enabled for future captures
             * and revisit once a reliable repro sequence exists. */
            vt_tty_t *tty = &g_ttys[(uint32_t)tty_index];
            int32_t args[4] = { msg.arg0, msg.arg1, msg.arg2, msg.arg3 };
            for (int i = 0; i < 4; ++i) {
                uint8_t b = (uint8_t)(args[i] & 0xFF);
                if (b == 0) {
                    break;
                }
                vt_process_byte((uint32_t)tty_index, tty, b);
            }
            break;
        }

        case VT_IPC_SET_ATTR_REQ: {
            int32_t tty_index = vt_tty_index_for_source(msg.source);
            if (tty_index < 0 || tty_index >= (int32_t)VT_MAX_TTYS) {
                if (msg.source >= 0 && msg.request_id != 0) {
                    wasmos_ipc_reply(msg.source, g_vt_ep,
                                     VT_IPC_ERROR, msg.request_id, -1, 0);
                }
                break;
            }
            vt_tty_t *tty = &g_ttys[(uint32_t)tty_index];
            uint8_t fg = (uint8_t)(msg.arg0 & 0xFF);
            uint8_t bg = (uint8_t)(msg.arg1 & 0xFF);
            uint8_t attr = (uint8_t)(msg.arg2 & 0xFF);
            if (fg <= 15u) {
                tty->fg = fg;
            }
            if (bg <= 15u) {
                tty->bg = bg;
            }
            tty->attr = attr;
            if (msg.source >= 0 && msg.request_id != 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP, msg.request_id, 0, 0);
            }
            break;
        }

        case VT_IPC_SWITCH_TTY: {
            int32_t sw = vt_switch_tty((uint32_t)msg.arg0);
            if (msg.source >= 0 && msg.request_id != 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 (sw == 0) ? VT_IPC_RESP : VT_IPC_ERROR,
                                 msg.request_id,
                                 (sw == 0) ? (int32_t)g_switch_generation : sw,
                                 (int32_t)g_active_tty);
            }
            break;
        }

        case VT_IPC_GET_ACTIVE_TTY:
            if (msg.source >= 0 && msg.request_id != 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP,
                                 msg.request_id,
                                 (int32_t)g_switch_generation,
                                 (int32_t)g_active_tty);
            }
            break;

        case VT_IPC_REGISTER_WRITER: {
            if (msg.source < 0 || msg.request_id == 0) {
                break;
            }
            int32_t tty_id = msg.arg0;
            if (tty_id < 0 || tty_id >= (int32_t)VT_MAX_TTYS) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
                break;
            }
            uint32_t idx = (uint32_t)tty_id;
            if (g_tty_writer_ep[idx] >= 0 && g_tty_writer_ep[idx] != msg.source) {
                vt_trace_mark(VT_TRACE_WRITER_CONFLICT,
                              (uint16_t)(idx & 0x0FFFu),
                              (uint16_t)((uint32_t)msg.source & 0x0FFFu));
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
                break;
            }
            g_tty_writer_ep[idx] = msg.source;
            vt_trace_mark(VT_TRACE_WRITER_OK,
                          (uint16_t)(idx & 0x0FFFu),
                          (uint16_t)((uint32_t)msg.source & 0x0FFFu));
            wasmos_ipc_reply(msg.source, g_vt_ep,
                             VT_IPC_RESP, msg.request_id,
                             (int32_t)g_switch_generation, tty_id);
            break;
        }

        case VT_IPC_READ_REQ: {
            if (msg.source < 0 || msg.request_id == 0) {
                break;
            }
            int32_t tty_id = msg.arg0;
            if (tty_id < 0 || tty_id >= (int32_t)VT_MAX_TTYS) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
                break;
            }
            if (g_tty_reader_ep[(uint32_t)tty_id] < 0) {
                g_tty_reader_ep[(uint32_t)tty_id] = msg.source;
            } else if (g_tty_reader_ep[(uint32_t)tty_id] != msg.source) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
                break;
            }
            uint8_t ch = 0;
            if (vt_input_q_pop(&g_ttys[(uint32_t)tty_id], &ch) == 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP, msg.request_id, 0, (int32_t)ch);
            } else {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP, msg.request_id, 1, 0);
            }
            break;
        }

        case VT_IPC_SET_MODE_REQ: {
            if (msg.source < 0 || msg.request_id == 0) {
                break;
            }
            int32_t tty_index = vt_tty_index_for_source(msg.source);
            if (tty_index < 0 || tty_index >= (int32_t)VT_MAX_TTYS) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
                break;
            }
            uint8_t mode = (uint8_t)(msg.arg0 &
                                     (VT_INPUT_MODE_CANONICAL | VT_INPUT_MODE_ECHO));
            vt_set_input_mode(&g_ttys[(uint32_t)tty_index], mode);
            wasmos_ipc_reply(msg.source, g_vt_ep,
                             VT_IPC_RESP, msg.request_id,
                             (int32_t)mode, tty_index);
            break;
        }

        case KBD_IPC_KEY_NOTIFY:
            vt_handle_key_notify(msg.arg0, msg.arg1, msg.arg2);
            break;

        case KBD_IPC_SUBSCRIBE_RESP:
            break;

        default:
            if (msg.source >= 0 && msg.request_id != 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
            }
            break;
        }
    }

    return 0;
}
