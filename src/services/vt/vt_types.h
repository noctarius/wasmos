/* vt_types.h - structs, enums, and constants for the VT (virtual terminal) service */
#ifndef WASMOS_VT_TYPES_H
#define WASMOS_VT_TYPES_H

#include <stdint.h>

/* VT100/ANSI escape-sequence parser state. */
typedef enum {
    ESC_NORMAL = 0,
    ESC_ESC,   /* saw 0x1B, waiting for '[' or other escape char */
    ESC_CSI,   /* inside CSI sequence (ESC '[' ...) */
} esc_state_t;

/* One character cell in the framebuffer cell grid. */
typedef struct {
    uint32_t ch;    /* Unicode code point */
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;   /* bold/underline/blink bits */
    uint8_t _pad;
} vt_cell_t;

/* Full state for one virtual TTY.
 * input_q is a circular byte queue for raw keyboard events.
 * input_line/input_history hold canonical-mode line editing state.
 * csi_params[] holds up to 8 numeric CSI parameters. */
typedef struct {
    uint16_t cursor_row;
    uint16_t cursor_col;
    uint16_t cursor_saved_row;     /* saved by ESC 7 / CSI s */
    uint16_t cursor_saved_col;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t cursor_visible;
    uint8_t cursor_saved_valid;    /* non-zero if saved position is valid */
    uint8_t input_echo;
    uint8_t input_canonical;
    esc_state_t esc;
    uint16_t input_q_head;
    uint16_t input_q_tail;
    uint16_t input_line_len;
    uint16_t input_line_cursor;    /* insertion point within input_line */
    uint8_t input_history_count;
    uint8_t input_history_head;    /* ring-buffer head index */
    int8_t input_history_nav;      /* -1 = not navigating; >=0 = history offset */
    uint8_t input_q[256];          /* raw input ring buffer */
    uint8_t input_line[128];       /* current canonical line being edited */
    uint8_t input_history[8][128];
    uint8_t input_history_len[8];
    uint16_t csi_params[8];
    uint8_t csi_count;
    uint16_t csi_current;          /* parameter accumulator */
    uint8_t csi_have_current;
    uint8_t csi_private;           /* non-zero if CSI had a '?' prefix */
    vt_cell_t *cells;              /* pointer into the flat cell arena */
} vt_tty_t;

#define VT_MAX_TTYS 4u
#define VT_COLS_DEFAULT 80u
#define VT_ROWS_DEFAULT 25u
#define VT_MAX_COLS 160u
#define VT_MAX_ROWS 64u

/* Keep in sync with kernel ipc.h */
#define IPC_ERR_FULL (-3)

/* Retry budgets for framebuffer and IPC operations. */
#define VT_FB_SEND_RETRIES 1024u
#define VT_FB_SWITCH_CTRL_RETRIES 8192u
#define VT_FB_SWITCH_CELL_RETRIES 4096u
#define VT_IPC_REPLY_RETRIES 1024u
#define VT_GEOMETRY_QUERY_RETRIES 2048

/* Codes passed to wasmos_debug_mark for allocation failure tracing. */
enum {
    VT_ALLOC_FAIL_NONE = 0,
    VT_ALLOC_FAIL_ALIGN = 1,
    VT_ALLOC_FAIL_OVERFLOW = 2,
    VT_ALLOC_FAIL_GROW = 3,
    VT_ALLOC_FAIL_CAPACITY = 4
};

/* Codes passed to wasmos_debug_mark for TTY-switch and write-drop tracing. */
enum {
    VT_TRACE_SWITCH = 0xA1,
    VT_TRACE_WRITER_OK = 0xA2,
    VT_TRACE_WRITER_CONFLICT = 0xA3,
    VT_TRACE_DROP_UNOWNED = 0xA4,  /* write from an endpoint not registered for the active TTY */
    VT_TRACE_DROP_STALE = 0xA5     /* write's generation < g_switch_generation */
};

/* Error codes returned in VT_IPC_SWITCH_TTY replies. */
enum {
    VT_SWITCH_ERR_INVALID_TTY = -1,
    VT_SWITCH_ERR_MODE_OFF = -11,   /* failed to disable framebuffer rendering */
    VT_SWITCH_ERR_CLEAR = -12,      /* failed to clear the screen */
    VT_SWITCH_ERR_REPLAY = -13,     /* failed to replay cell buffer */
    VT_SWITCH_ERR_MODE_ON = -14     /* failed to re-enable framebuffer rendering */
};

#endif
