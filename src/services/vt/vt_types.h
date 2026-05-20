#ifndef WASMOS_VT_TYPES_H
#define WASMOS_VT_TYPES_H

#include <stdint.h>

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
    vt_cell_t *cells;
} vt_tty_t;

#define VT_MAX_TTYS 4u
#define VT_COLS_DEFAULT 80u
#define VT_ROWS_DEFAULT 25u
#define VT_MAX_COLS 160u
#define VT_MAX_ROWS 64u

/* Keep in sync with kernel ipc.h */
#define IPC_ERR_FULL (-3)

#define VT_FB_SEND_RETRIES 1024u
#define VT_FB_SWITCH_CTRL_RETRIES 8192u
#define VT_FB_SWITCH_CELL_RETRIES 4096u
#define VT_IPC_REPLY_RETRIES 1024u
#define VT_GEOMETRY_QUERY_RETRIES 2048

enum {
    VT_ALLOC_FAIL_NONE = 0,
    VT_ALLOC_FAIL_ALIGN = 1,
    VT_ALLOC_FAIL_OVERFLOW = 2,
    VT_ALLOC_FAIL_GROW = 3,
    VT_ALLOC_FAIL_CAPACITY = 4
};

enum {
    VT_TRACE_SWITCH = 0xA1,
    VT_TRACE_WRITER_OK = 0xA2,
    VT_TRACE_WRITER_CONFLICT = 0xA3,
    VT_TRACE_DROP_UNOWNED = 0xA4,
    VT_TRACE_DROP_STALE = 0xA5
};

enum {
    VT_SWITCH_ERR_INVALID_TTY = -1,
    VT_SWITCH_ERR_MODE_OFF = -11,
    VT_SWITCH_ERR_CLEAR = -12,
    VT_SWITCH_ERR_REPLAY = -13,
    VT_SWITCH_ERR_MODE_ON = -14
};

#endif
