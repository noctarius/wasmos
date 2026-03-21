#include <stdint.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

/*
 * vt — Phase 2 virtual terminal service.
 *
 * Sits between clients and the serial/console path:
 *   - Exposes a VT_IPC_WRITE_REQ endpoint that any process can write text to.
 *   - Strips ANSI/VT100 escape sequences so raw control codes never reach the
 *     serial console ring renderer.
 *   - Forwards printable bytes via wasmos_console_write so serial.c writes
 *     into the shared console ring consumed by the framebuffer driver.
 *   - Subscribes to the keyboard driver (KBD_IPC_SUBSCRIBE_REQ) and echoes
 *     key-down events through the kernel input ring.
 *
 * IPC protocol — VT_IPC_WRITE_REQ:
 *   arg0..arg3: up to four bytes of payload, one per field (low 8 bits).
 *   A 0x00 byte terminates the payload early (Phase 1 — text output only).
 *   Client loops to send longer strings.
 *
 * KBD_IPC_KEY_NOTIFY:
 *   arg0 = PS/2 Set-1 scancode (bit 7 cleared — always key-down from driver).
 *   arg1 = 0 key-down, 1 key-up (always 0 from keyboard.ts).
 *
 * Phase 3 additions (not here yet):
 *   - TTY multiplexing (VT_IPC_SWITCH_TTY) and Alt+Fn key combos.
 *   - SGR color attributes (VT_IPC_SET_ATTR_REQ).
 *   - UTF-8 decoding and wide characters.
 */

/* -------------------------------------------------------------------------
 * Minimal ANSI/VT100 escape-sequence parser.
 * ---------------------------------------------------------------------- */
typedef enum {
    ESC_NORMAL = 0,
    ESC_ESC,
    ESC_CSI,
} esc_state_t;

typedef struct {
    uint16_t cursor_row;
    uint16_t cursor_col;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    esc_state_t esc;
} vt_tty_t;

#define VT_MAX_TTYS 4
#define VT_COLS_DEFAULT 80u
#define VT_ROWS_DEFAULT 25u

static int32_t     g_vt_ep  = -1;
static int32_t     g_kbd_ep = -1;
static vt_tty_t    g_ttys[VT_MAX_TTYS];
static uint32_t    g_active_tty = 0;

static vt_tty_t *
vt_active_tty(void)
{
    if (g_active_tty >= VT_MAX_TTYS) {
        g_active_tty = 0;
    }
    return &g_ttys[g_active_tty];
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
        g_ttys[i].esc = ESC_NORMAL;
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
    /* FIXME: Switching tty state currently does not trigger framebuffer
     * re-render of that tty's saved cells; output path still goes through
     * serial->console-ring until vt owns full screen-state replay. */
    return 0;
}

static void
vt_advance_cursor(vt_tty_t *tty, uint8_t ch)
{
    if (!tty) {
        return;
    }
    if (ch == '\r') {
        tty->cursor_col = 0;
        return;
    }
    if (ch == '\n') {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1 < VT_ROWS_DEFAULT) {
            tty->cursor_row++;
        }
        return;
    }
    if (ch == '\b') {
        if (tty->cursor_col > 0) {
            tty->cursor_col--;
        }
        return;
    }
    if (ch == '\t') {
        uint16_t next = (uint16_t)((tty->cursor_col + 8u) & ~7u);
        if (next >= VT_COLS_DEFAULT) {
            tty->cursor_col = 0;
            if (tty->cursor_row + 1 < VT_ROWS_DEFAULT) {
                tty->cursor_row++;
            }
        } else {
            tty->cursor_col = next;
        }
        return;
    }
    tty->cursor_col++;
    if (tty->cursor_col >= VT_COLS_DEFAULT) {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1 < VT_ROWS_DEFAULT) {
            tty->cursor_row++;
        }
    }
}

static void
vt_put_char(vt_tty_t *tty, uint32_t cp)
{
    char ch = (char)(cp & 0xFFu);
    vt_advance_cursor(tty, (uint8_t)ch);
    (void)wasmos_console_write((int32_t)(uintptr_t)&ch, 1);
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
        } else {
            vt_put_char(tty, (uint32_t)c);
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

/* -------------------------------------------------------------------------
 * PS/2 Set-1 scancode → ASCII table (key-down codes 0x01–0x39).
 * Index = scancode; value = ASCII character (0 = no mapping).
 * ---------------------------------------------------------------------- */
static const uint8_t g_sc_to_ascii[58] = {
    0,    /* 0x00 — unused */
    0x1B, /* 0x01 — Escape */
    '1',  /* 0x02 */
    '2',  /* 0x03 */
    '3',  /* 0x04 */
    '4',  /* 0x05 */
    '5',  /* 0x06 */
    '6',  /* 0x07 */
    '7',  /* 0x08 */
    '8',  /* 0x09 */
    '9',  /* 0x0A */
    '0',  /* 0x0B */
    '-',  /* 0x0C */
    '=',  /* 0x0D */
    '\b', /* 0x0E — Backspace */
    '\t', /* 0x0F — Tab */
    'q',  /* 0x10 */
    'w',  /* 0x11 */
    'e',  /* 0x12 */
    'r',  /* 0x13 */
    't',  /* 0x14 */
    'y',  /* 0x15 */
    'u',  /* 0x16 */
    'i',  /* 0x17 */
    'o',  /* 0x18 */
    'p',  /* 0x19 */
    '[',  /* 0x1A */
    ']',  /* 0x1B */
    '\n', /* 0x1C — Enter */
    0,    /* 0x1D — Left Ctrl */
    'a',  /* 0x1E */
    's',  /* 0x1F */
    'd',  /* 0x20 */
    'f',  /* 0x21 */
    'g',  /* 0x22 */
    'h',  /* 0x23 */
    'j',  /* 0x24 */
    'k',  /* 0x25 */
    'l',  /* 0x26 */
    ';',  /* 0x27 */
    '\'', /* 0x28 */
    '`',  /* 0x29 */
    0,    /* 0x2A — Left Shift */
    '\\', /* 0x2B */
    'z',  /* 0x2C */
    'x',  /* 0x2D */
    'c',  /* 0x2E */
    'v',  /* 0x2F */
    'b',  /* 0x30 */
    'n',  /* 0x31 */
    'm',  /* 0x32 */
    ',',  /* 0x33 */
    '.',  /* 0x34 */
    '/',  /* 0x35 */
    0,    /* 0x36 — Right Shift */
    '*',  /* 0x37 — Keypad * */
    0,    /* 0x38 — Left Alt */
    ' ',  /* 0x39 — Space */
};

static void
vt_handle_key_notify(int32_t scancode, int32_t keyup)
{
    /* Only handle key-down; ignore extended (0xE0 prefix handled separately). */
    if (keyup != 0) {
        return;
    }
    if (scancode <= 0 || scancode >= (int32_t)(sizeof(g_sc_to_ascii))) {
        return;
    }
    uint8_t ch = g_sc_to_ascii[(uint32_t)scancode];
    if (ch == 0) {
        return;
    }
    /* Push to the kernel input ring.  The CLI (or any reader of
     * wasmos_console_read) will echo the character via the output path,
     * which now routes back through vt — single echo, no duplication. */
    wasmos_input_push((int32_t)ch);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

WASMOS_WASM_EXPORT int32_t
initialize(int32_t fb_endpoint, int32_t kbd_endpoint, int32_t arg2, int32_t arg3)
{
    (void)fb_endpoint;
    (void)arg2;
    (void)arg3;

    g_kbd_ep = kbd_endpoint;
    vt_init_ttys();

    g_vt_ep = wasmos_ipc_create_endpoint();
    if (g_vt_ep < 0) {
        return -1;
    }

    /* Subscribe to keyboard events if a keyboard endpoint was provided. */
    if (g_kbd_ep >= 0) {
        wasmos_ipc_send(
            g_kbd_ep,
            g_vt_ep,
            KBD_IPC_SUBSCRIBE_REQ,
            1,
            0, 0, 0, 0);
        /* We fire-and-forget the subscribe — keyboard sends RESP but we don't
         * need to wait for it; the notify messages will arrive when ready. */
    }

    /* IPC server loop. */
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
            for (int i = 0; i < 4; i++) {
                uint8_t b = (uint8_t)(args[i] & 0xFF);
                if (b == 0) {
                    break;
                }
                vt_process_byte(tty, b);
            }
            if (msg.source >= 0) {
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
            if (msg.source >= 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_RESP, msg.request_id, 0, 0);
            }
            break;
        }

        case VT_IPC_SWITCH_TTY: {
            int32_t rc_switch = vt_switch_tty((uint32_t)msg.arg0);
            if (msg.source >= 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 (rc_switch == 0) ? VT_IPC_RESP : VT_IPC_ERROR,
                                 msg.request_id,
                                 rc_switch,
                                 (int32_t)g_active_tty);
            }
            break;
        }

        case KBD_IPC_KEY_NOTIFY:
            vt_handle_key_notify(msg.arg0, msg.arg1);
            break;

        case KBD_IPC_SUBSCRIBE_RESP:
            /* Keyboard acknowledged our subscribe — nothing to do. */
            break;

        default:
            if (msg.source >= 0) {
                wasmos_ipc_reply(msg.source, g_vt_ep,
                                 VT_IPC_ERROR, msg.request_id, -1, 0);
            }
            break;
        }
    }

    return 0;
}
