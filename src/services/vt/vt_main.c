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
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t input_echo;
    uint8_t input_canonical;
    esc_state_t esc;
    uint16_t input_q_head;
    uint16_t input_q_tail;
    uint16_t input_line_len;
    uint16_t input_line_cursor;
    uint8_t input_q[256];
    uint8_t input_line[128];
    vt_cell_t cells[80u * 25u];
} vt_tty_t;

#define VT_MAX_TTYS 4u
#define VT_COLS_DEFAULT 80u
#define VT_ROWS_DEFAULT 25u
/* Keep in sync with kernel ipc.h */
#define IPC_ERR_FULL (-3)
#define VT_FB_SEND_RETRIES 1024

static int32_t  g_vt_ep = -1;
static int32_t  g_kbd_ep = -1;
static int32_t  g_fb_ep = -1;
static vt_tty_t g_ttys[VT_MAX_TTYS];
static uint32_t g_active_tty = 0;
static int32_t  g_tty_reader_ep[VT_MAX_TTYS] = { -1, -1, -1, -1 };
static uint8_t  g_ctrl_down = 0;
static uint8_t  g_shift_down = 0;

static uint32_t
vt_cell_index(uint16_t row, uint16_t col)
{
    return (uint32_t)row * VT_COLS_DEFAULT + (uint32_t)col;
}

static vt_tty_t *
vt_active_tty(void)
{
    if (g_active_tty >= VT_MAX_TTYS) {
        g_active_tty = 0;
    }
    return &g_ttys[g_active_tty];
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
    if (!tty) {
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
vt_scroll_up(vt_tty_t *tty)
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

    if (g_active_tty != 0) {
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
            if (tty->cursor_row + 1u < VT_ROWS_DEFAULT) {
                tty->cursor_row++;
            }
        } else if (c == '\b') {
            if (tty->cursor_col > 0) {
                tty->cursor_col--;
            }
        } else {
            tty->cursor_col++;
            if (tty->cursor_col >= VT_COLS_DEFAULT) {
                tty->cursor_col = 0;
                if (tty->cursor_row + 1u < VT_ROWS_DEFAULT) {
                    tty->cursor_row++;
                }
            }
        }
    }
    (void)wasmos_console_write((int32_t)(uintptr_t)&c, 1);
}

static void
vt_put_char_virtual(vt_tty_t *tty, uint8_t ch)
{
    if (!tty) {
        return;
    }

    if (ch == '\r') {
        tty->cursor_col = 0;
        vt_fb_set_cursor(tty);
        return;
    }
    if (ch == '\n') {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1u >= VT_ROWS_DEFAULT) {
            vt_scroll_up(tty);
            tty->cursor_row = (uint16_t)(VT_ROWS_DEFAULT - 1u);
        } else {
            tty->cursor_row++;
        }
        vt_fb_set_cursor(tty);
        return;
    }
    if (ch == '\b') {
        if (tty->cursor_col > 0) {
            tty->cursor_col--;
        }
        vt_store_cell(tty, tty->cursor_row, tty->cursor_col, ' ');
        vt_render_cell(tty, tty->cursor_row, tty->cursor_col);
        vt_fb_set_cursor(tty);
        return;
    }
    if (ch == '\t') {
        uint16_t next = (uint16_t)((tty->cursor_col + 8u) & ~7u);
        while (tty->cursor_col < next) {
            vt_put_char_virtual(tty, ' ');
        }
        return;
    }

    vt_store_cell(tty, tty->cursor_row, tty->cursor_col, (uint32_t)ch);
    vt_render_cell(tty, tty->cursor_row, tty->cursor_col);

    tty->cursor_col++;
    if (tty->cursor_col >= VT_COLS_DEFAULT) {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1u >= VT_ROWS_DEFAULT) {
            vt_scroll_up(tty);
            tty->cursor_row = (uint16_t)(VT_ROWS_DEFAULT - 1u);
        } else {
            tty->cursor_row++;
        }
    }
    vt_fb_set_cursor(tty);
}

static void
vt_process_byte(vt_tty_t *tty, uint8_t c)
{
    if (!tty) {
        return;
    }
    switch (tty->esc) {
    case ESC_NORMAL:
        if (c == 0x1B) {
            tty->esc = ESC_ESC;
        } else if (g_active_tty == 0) {
            vt_put_char_tty0(tty, c);
        } else {
            vt_put_char_virtual(tty, c);
        }
        break;
    case ESC_ESC:
        if (c == '[') {
            tty->esc = ESC_CSI;
        } else {
            tty->esc = ESC_NORMAL;
        }
        break;
    case ESC_CSI:
        if (c >= 0x40 && c <= 0x7E) {
            tty->esc = ESC_NORMAL;
        }
        break;
    }
}

static void
vt_replay_tty(uint32_t tty_index)
{
    if (tty_index == 0 || tty_index >= VT_MAX_TTYS) {
        return;
    }
    vt_tty_t *tty = &g_ttys[tty_index];

    (void)vt_fb_send(FBTEXT_IPC_CLEAR_REQ, 0, 0, 0, 0);

    for (uint16_t row = 0; row < VT_ROWS_DEFAULT; ++row) {
        for (uint16_t col = 0; col < VT_COLS_DEFAULT; ++col) {
            uint32_t idx = vt_cell_index(row, col);
            if (tty->cells[idx].ch == 0) {
                continue;
            }
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
        g_ttys[i].fg = 15;
        g_ttys[i].bg = 0;
        g_ttys[i].attr = 0;
        /* Input is delivered raw to the tty client; CLI owns line editing
         * and user-visible echo so serial and framebuffer behave the same. */
        g_ttys[i].input_echo = 0;
        g_ttys[i].input_canonical = 0;
        g_ttys[i].esc = ESC_NORMAL;
        g_ttys[i].input_q_head = 0;
        g_ttys[i].input_q_tail = 0;
        g_ttys[i].input_line_len = 0;
        g_ttys[i].input_line_cursor = 0;
        for (uint32_t k = 0; k < sizeof(g_ttys[i].input_q); ++k) {
            g_ttys[i].input_q[k] = 0;
        }
        for (uint32_t k = 0; k < sizeof(g_ttys[i].input_line); ++k) {
            g_ttys[i].input_line[k] = 0;
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
}

static int32_t
vt_switch_tty(uint32_t tty_index)
{
    if (tty_index >= VT_MAX_TTYS) {
        return -1;
    }

    g_active_tty = tty_index;

    /* Allow logical tty switching even when framebuffer control is unavailable
     * (startup races/headless mode). This keeps VT/CLI state consistent and
     * avoids reporting spurious switch failures to user-space. */
    if (g_fb_ep < 0) {
        return 0;
    }

    if (tty_index == 0) {
        vt_fb_console_mode(1);
        return 0;
    }

    vt_fb_console_mode(0);
    /* FIXME: replay currently repaints the full 80x25 virtual grid even if
     * only a small number of cells changed. */
    vt_replay_tty(tty_index);
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
    vt_process_byte(tty, ch);
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
}

static void
vt_input_handle_char(uint32_t tty_index, uint8_t ch)
{
    if (tty_index >= VT_MAX_TTYS) {
        return;
    }
    vt_tty_t *tty = &g_ttys[tty_index];
    if (tty->input_canonical) {
        if (ch == '\r' || ch == '\n') {
            if (tty->input_echo) {
                vt_input_echo_char(tty_index, '\n');
            }
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
vt_handle_key_notify(int32_t scancode, int32_t keyup)
{
    if (scancode == 0x1D) { /* Ctrl */
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

    if (g_ctrl_down && g_shift_down) {
        /* Set 1 scancodes: F1..F4 => 0x3B..0x3E map to tty0..tty3. */
        if (scancode >= 0x3B && scancode <= 0x3E) {
            (void)vt_switch_tty((uint32_t)(scancode - 0x3B));
            return;
        }
    }

    if (scancode <= 0 || scancode >= (int32_t)(sizeof(g_sc_to_ascii))) {
        return;
    }
    uint8_t ch = g_sc_to_ascii[(uint32_t)scancode];
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
            vt_tty_t *tty = vt_active_tty();
            int32_t args[4] = { msg.arg0, msg.arg1, msg.arg2, msg.arg3 };
            for (int i = 0; i < 4; ++i) {
                uint8_t b = (uint8_t)(args[i] & 0xFF);
                if (b == 0) {
                    break;
                }
                vt_process_byte(tty, b);
            }
            if (msg.source >= 0 && msg.request_id != 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP, msg.request_id, 0, 0);
            }
            break;
        }

        case VT_IPC_SET_ATTR_REQ: {
            vt_tty_t *tty = vt_active_tty();
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
                                 sw,
                                 (int32_t)g_active_tty);
            }
            break;
        }

        case VT_IPC_GET_ACTIVE_TTY:
            if (msg.source >= 0 && msg.request_id != 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP,
                                 msg.request_id,
                                 0,
                                 (int32_t)g_active_tty);
            }
            break;

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

        case KBD_IPC_KEY_NOTIFY:
            vt_handle_key_notify(msg.arg0, msg.arg1);
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
