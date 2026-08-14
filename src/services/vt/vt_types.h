/* vt_types.h - structs, enums, and constants for the VT (virtual terminal) service */
#ifndef WASMOS_VT_TYPES_H
#define WASMOS_VT_TYPES_H

#include <stdint.h>

/* VT100/ANSI escape-sequence parser state. */
typedef enum {
    ESC_NORMAL = 0,
    ESC_ESC, /* saw 0x1B, waiting for '[' or other escape char */
    ESC_CSI, /* inside CSI sequence (ESC '[' ...) */
} esc_state_t;

/* One character cell in the framebuffer cell grid. */
typedef struct {
    uint32_t ch; /* Unicode code point */
    uint8_t fg;
    uint8_t bg;
    uint8_t attr; /* bold/underline/blink bits */
    uint8_t _pad;
} vt_cell_t;

/* Full state for one virtual TTY.
 * input_q is a circular byte queue of decoded input bytes (keyboard characters
 * and serial RX), drained by the slot's registered reader.
 * input_line/input_history hold canonical-mode line editing state.
 * csi_params[] holds up to 8 numeric CSI parameters. */
typedef struct {
    uint16_t cursor_row;
    uint16_t cursor_col;
    uint16_t cursor_saved_row; /* saved by ESC 7 / CSI s */
    uint16_t cursor_saved_col;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t cursor_visible;
    uint8_t cursor_saved_valid; /* non-zero if saved position is valid */
    uint8_t input_echo;
    uint8_t input_canonical;
    esc_state_t esc;
    uint16_t input_q_head;
    uint16_t input_q_tail;
    uint16_t input_line_len;
    uint16_t input_line_cursor; /* insertion point within input_line */
    uint8_t input_history_count;
    uint8_t input_history_head; /* ring-buffer head index */
    int8_t input_history_nav;   /* -1 = not navigating; >=0 = history offset */
    uint8_t input_q[256];       /* raw input ring buffer */
    uint8_t input_line[128];    /* current canonical line being edited */
    uint8_t input_history[8][128];
    uint8_t input_history_len[8];
    uint16_t csi_params[8];
    uint8_t csi_count;
    uint16_t csi_current; /* parameter accumulator */
    uint8_t csi_have_current;
    uint8_t csi_private; /* non-zero if CSI had a '?' prefix */
    vt_cell_t* cells;    /* pointer into the flat cell arena */
} vt_tty_t;

/* The tty slot model.
 *
 * The vt owns VT_MAX_TTYS slots, indexed 0..VT_MAX_TTYS-1 and selected by the
 * user with Ctrl+Shift+F1..F4 or by a client sending VT_IPC_SWITCH_TTY.  Exactly
 * one slot is *visible* (rendered to the framebuffer) at a time; every slot
 * keeps a full cell grid and a live input queue whether or not it is visible.
 *
 * Slots are not interchangeable:
 *   - slot 0 is the graphical slot.  The vt paints no text for it.  The first
 *     client to send VT_IPC_SWITCH_TTY(0) — the gfx compositor at startup —
 *     becomes slot 0's key/visibility sink and thereafter receives decoded keys
 *     as VT_IPC_KEY_FORWARD and every visibility change as VT_IPC_VIS_NOTIFY.
 *     A later switch to slot 0 does not reassign that sink.  The compositor owns
 *     the framebuffer only while slot 0 is visible, and the vt tells it so.
 *   - slot 1 is the system console: it is the default serial-bound slot, so COM1
 *     RX (VT_IPC_SERIAL_INPUT_REQ) is injected into its input queue no matter
 *     which slot is visible, and the kernel log ring is drained into it.
 *   - the remaining slots are plain text ttys.
 *
 * Per slot the vt tracks at most one registered writer endpoint
 * (VT_IPC_REGISTER_WRITER; a new registration replaces a stale one) and at most
 * one reader endpoint, claimed by the first VT_IPC_READ_REQ and exclusive
 * afterwards (WASMOS_ERR_VT_READER_BUSY for anyone else).  A write or a mode
 * change is routed to the slot whose reader or writer is the message source, so
 * an endpoint that is neither is dropped.
 *
 * The vt is the system's only keymap decoder: it consumes raw set-1 scancodes
 * from the `kbd` driver and everything downstream — text slots and the
 * compositor alike — sees decoded characters. */
#define VT_MAX_TTYS 4u
/* Text geometry used when the framebuffer geometry query fails, and the
 * fallback the service retries with when the queried geometry's cell grids do
 * not fit in its heap. */
#define VT_COLS_DEFAULT 80u
#define VT_ROWS_DEFAULT 25u
/* Hard ceiling on a queried geometry.  These bound the per-slot cell arena
 * (VT_MAX_TTYS * VT_MAX_COLS * VT_MAX_ROWS * sizeof(vt_cell_t)), so raising them
 * raises the service's memory floor. */
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

/* Reason recorded in g_alloc_failure when a cell-grid allocation fails; logged
 * by vt_log_alloc_failure. */
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
    VT_TRACE_DROP_UNOWNED = 0xA4, /* write from an endpoint not registered for the active TTY */
    VT_TRACE_DROP_STALE = 0xA5    /* write's generation < g_switch_generation */
};

/* VT_IPC_SWITCH_TTY reply codes are the packed vt domain in abi/errors.yaml
 * (WASMOS_ERR_VT_BAD_TTY_ID, WASMOS_ERR_VT_SWITCH_*). */

#endif
