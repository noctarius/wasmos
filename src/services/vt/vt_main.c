/* vt_main.c - virtual terminal service: manages 4 TTYs with VT100/ANSI emulation,
 * routes keyboard events, and renders to the framebuffer compositor service */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"
#include "vt_types.h"

#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

static int32_t g_vt_ep = -1;
static int32_t g_kbd_ep = -1;
static int32_t g_fb_ep = -1;
static vt_tty_t g_ttys[VT_MAX_TTYS];
static uint32_t g_active_tty = 0;
/* Slot bound to the serial console: RX from the serial driver is injected here,
 * regardless of which slot is visible.  Default vt-1 (the system console). */
static uint32_t g_serial_tty = 1;
static int32_t g_serial_in_ep = -1;
static int32_t g_fs_ep = -1;
static int32_t g_tty_reader_ep[VT_MAX_TTYS] = {-1, -1, -1, -1};
static int32_t g_tty_writer_ep[VT_MAX_TTYS] = {-1, -1, -1, -1};
static uint32_t g_switch_generation = 1;
static uint8_t g_switch_barrier = 0;
static uint8_t g_ctrl_down = 0;
static uint8_t g_shift_down = 0;
static uint8_t g_altgr_down = 0;
static uint16_t g_vt_cols = VT_COLS_DEFAULT;
static uint16_t g_vt_rows = VT_ROWS_DEFAULT;
static uint32_t g_heap_cursor = 0;
static uint32_t g_heap_limit = 0;
static int32_t g_alloc_failure = 0;

/* klog ring: the VT owns an SPSC byte ring in shared memory; the
 * kernel publishes klog text into it (serial_write) and the VT drains it into
 * vt-1 (the system console).  The ring carries no IPC wake, so the main loop
 * polls it on a bounded timed wait — klog latency into an off-screen slot is
 * not critical, and interactive input still wakes the endpoint immediately. */
#define VT_KLOG_TTY 1u
#define VT_KLOG_RING_CAPACITY 4096u /* SPSC data capacity (power of two) */
static wasmos_ringbuf_t g_klog_ring;
static int32_t g_klog_ring_ready = 0;

/* Bulk grid blit: a shared xfer-buffer holding the visible slot's cell grid,
 * granted READ to the framebuffer driver.  A tty switch repaints the whole
 * screen with a single FBTEXT_IPC_BLIT_GRID_REQ; one cell-write IPC per cell
 * storms the driver's queue and wedges switching under SMP.  Sized for the
 * maximum grid so it survives any geometry. */
#define VT_BLIT_MAX_CELLS ((uint32_t)VT_MAX_COLS * (uint32_t)VT_MAX_ROWS)
static fbtext_blit_cell_t* g_blit_grid = 0; /* VT-side mapped write pointer */
static int32_t g_blit_ready = 0;
_Static_assert(sizeof(fbtext_blit_cell_t) == sizeof(vt_cell_t),
               "blit cell must match vt_cell_t so the grid copies without conversion");

extern uint8_t __heap_base;

/* Emit a 32-bit debug_mark tag encoding event (8 bits) and two 12-bit values.
 * Compiled out when WASMOS_TRACE == 0. */
static void vt_trace_mark(uint8_t event, uint16_t a, uint16_t b) {
#if WASMOS_TRACE
    uint32_t tag =
        ((uint32_t)event << 24) | (((uint32_t)(a & 0x0FFFu)) << 12) | (uint32_t)(b & 0x0FFFu);
    (void)wasmos_debug_mark((int32_t)tag);
#else
    (void)event;
    (void)a;
    (void)b;
#endif
}

static uint32_t vt_pack_cell_colors(const vt_cell_t* cell) {
    if (!cell) {
        return 0u;
    }
    return ((uint32_t)(cell->fg & 0x0Fu) << 8) | (uint32_t)(cell->bg & 0x0Fu);
}

static uint32_t vt_cell_index(uint16_t row, uint16_t col) {
    return (uint32_t)row * g_vt_cols + (uint32_t)col;
}

static uint32_t vt_cell_capacity(void) {
    return (uint32_t)g_vt_cols * (uint32_t)g_vt_rows;
}

/* Initialise the custom bump heap at &__heap_base for cell-grid allocation. */
static void vt_heap_init(void) {
    g_heap_cursor = addr_cast(uint32_t, &__heap_base);
    g_heap_limit = (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
    if (g_heap_cursor > g_heap_limit) {
        g_heap_cursor = g_heap_limit;
    }
    g_alloc_failure = VT_ALLOC_FAIL_NONE;
}

static void vt_log_alloc_failure(const char* tag, int32_t code) {
    printf("[vt] alloc failed: %s code=%d\n", tag ? tag : "unknown", (int)code);
}

static void* vt_alloc(uint32_t size, uint32_t align) {
    if (size == 0u) {
        return ptr_cast(void, g_heap_cursor);
    }
    if (align == 0u) {
        align = 1u;
    }
    if ((align & (align - 1u)) != 0u) {
        g_alloc_failure = VT_ALLOC_FAIL_ALIGN;
        return 0;
    }
    uint32_t aligned = (g_heap_cursor + (align - 1u)) & ~(align - 1u);
    uint32_t end = aligned + size;
    if (end < aligned) {
        g_alloc_failure = VT_ALLOC_FAIL_OVERFLOW;
        return 0;
    }
    while (end > g_heap_limit) {
        int32_t grown = (int32_t)__builtin_wasm_memory_grow(0, 1);
        if (grown < 0) {
            g_alloc_failure = VT_ALLOC_FAIL_GROW;
            return 0;
        }
        g_heap_limit += 65536u;
    }
    g_heap_cursor = end;
    return ptr_cast(void, aligned);
}

static void vt_reset_tty_cells(void) {
    for (uint32_t i = 0; i < VT_MAX_TTYS; ++i) {
        g_ttys[i].cells = 0;
    }
}

/* Allocate one cell grid per TTY from the bump heap; sets cells pointer. */
static int vt_alloc_tty_cells(void) {
    uint32_t cells = vt_cell_capacity();
    if (cells == 0u || cells > (uint32_t)VT_MAX_COLS * (uint32_t)VT_MAX_ROWS) {
        g_alloc_failure = VT_ALLOC_FAIL_CAPACITY;
        return -1;
    }
    uint32_t bytes = cells * (uint32_t)sizeof(vt_cell_t);
    for (uint32_t i = 0; i < VT_MAX_TTYS; ++i) {
        vt_cell_t* buf = (vt_cell_t*)vt_alloc(bytes, 8u);
        if (!buf) {
            return -1;
        }
        g_ttys[i].cells = buf;
    }
    return 0;
}

static void vt_render_cell(const vt_tty_t* tty, uint16_t row, uint16_t col);
static void vt_draw_tty0_hint(void);

static int vt_bytes_equal(const uint8_t* a, const uint8_t* b, uint16_t len) {
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

static uint16_t vt_input_q_next(uint16_t v) {
    return (uint16_t)((v + 1u) & 0xFFu);
}

/* Wake a slot's registered reader so it drains the input queue (push model),
 * instead of the reader polling VT_IPC_READ_REQ. */
static void vt_notify_reader(uint32_t tty_index) {
    if (tty_index >= VT_MAX_TTYS) {
        return;
    }
    int32_t reader = g_tty_reader_ep[tty_index];
    if (reader >= 0) {
        (void)wasmos_ipc_send(reader, g_vt_ep, VT_IPC_INPUT_NOTIFY, 0, (int32_t)tty_index, 0, 0, 0);
    }
}

static int vt_input_q_push(vt_tty_t* tty, uint8_t ch) {
    if (!tty) {
        return -1;
    }
    uint16_t next = vt_input_q_next(tty->input_q_tail);
    if (next == tty->input_q_head) {
        return -1;
    }
    /* Edge-triggered: only notify on the empty -> non-empty transition.  The
     * reader drains fully before blocking again, so one wake per burst suffices;
     * a later arrival after the queue drains re-triggers. */
    int was_empty = (tty->input_q_head == tty->input_q_tail);
    tty->input_q[tty->input_q_tail] = ch;
    tty->input_q_tail = next;
    if (was_empty) {
        vt_notify_reader((uint32_t)(tty - g_ttys));
    }
    return 0;
}

static int vt_input_q_pop(vt_tty_t* tty, uint8_t* out_ch) {
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

static void vt_input_q_push_escape(vt_tty_t* tty, uint8_t final) {
    if (!tty) {
        return;
    }
    (void)vt_input_q_push(tty, 0x1B); /* ESC */
    (void)vt_input_q_push(tty, '[');
    (void)vt_input_q_push(tty, final);
}

static int vt_fb_send(uint32_t type, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    if (g_fb_ep < 0 || g_vt_ep < 0) {
        return -1;
    }
    uint32_t tries = 0;
    for (;;) {
        int32_t rc = wasmos_ipc_send(g_fb_ep, g_vt_ep, (int32_t)type, 0, arg0, arg1, arg2, arg3);
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

static int vt_fb_send_switch(uint32_t type, int32_t arg0, int32_t arg1, int32_t arg2,
                             int32_t arg3) {
    if (g_fb_ep < 0 || g_vt_ep < 0) {
        return -1;
    }
    uint32_t tries = 0;
    uint32_t max_tries =
        (type == FBTEXT_IPC_CELL_WRITE_REQ) ? VT_FB_SWITCH_CELL_RETRIES : VT_FB_SWITCH_CTRL_RETRIES;
    for (;;) {
        int32_t rc = wasmos_ipc_send(g_fb_ep, g_vt_ep, (int32_t)type, 0, arg0, arg1, arg2, arg3);
        if (rc == 0) {
            return 0;
        }
        if (rc != IPC_ERR_FULL || ++tries >= max_tries) {
            return rc;
        }
        (void)wasmos_sched_yield();
    }
}

static int vt_ipc_reply_retry(int32_t reply_endpoint, int32_t type, int32_t request_id,
                              int32_t arg0, int32_t arg1) {
    if (reply_endpoint < 0 || g_vt_ep < 0 || request_id == 0) {
        return -1;
    }
    uint32_t tries = 0;
    for (;;) {
        int32_t rc = wasmos_ipc_reply(reply_endpoint, g_vt_ep, type, request_id, arg0, arg1);
        if (rc == 0) {
            return 0;
        }
        if (rc != IPC_ERR_FULL) {
            return rc;
        }
        /* FIXME: if client queues stay saturated, drop the reply so VT keeps
         * servicing input/output rather than blocking indefinitely. */
        if (++tries >= VT_IPC_REPLY_RETRIES) {
            return IPC_ERR_FULL;
        }
        (void)wasmos_sched_yield();
    }
}

static void vt_query_geometry(void) {
    if (g_fb_ep < 0 || g_vt_ep < 0) {
        return;
    }
    int32_t req_id = 0x5647; /* fixed local request id */
    if (wasmos_ipc_send(g_fb_ep, g_vt_ep, FBTEXT_IPC_GEOMETRY_REQ, req_id, 0, 0, 0, 0) != 0) {
        return;
    }
    for (int tries = 0; tries < VT_GEOMETRY_QUERY_RETRIES; ++tries) {
        int32_t rc = wasmos_ipc_drain(g_vt_ep);
        if (rc < 0) {
            return;
        }
        if (rc == 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t rid = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (rid != req_id) {
            continue;
        }
        if (type != FBTEXT_IPC_RESP) {
            return;
        }
        int32_t cols = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t rows = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        if (cols >= 40 && cols <= (int32_t)VT_MAX_COLS) {
            g_vt_cols = (uint16_t)cols;
        }
        if (rows >= 16 && rows <= (int32_t)VT_MAX_ROWS) {
            g_vt_rows = (uint16_t)rows;
        }
        return;
    }
}

static void vt_fb_set_cursor(const vt_tty_t* tty) {
    if (!tty || !tty->cursor_visible) {
        return;
    }
    (void)vt_fb_send(FBTEXT_IPC_CURSOR_SET_REQ, (int32_t)tty->cursor_col, (int32_t)tty->cursor_row,
                     0, 0);
}

static void vt_fb_console_mode(uint8_t enabled) {
    (void)vt_fb_send(FBTEXT_IPC_CONSOLE_MODE_REQ, enabled ? 1 : 0, 0, 0, 0);
}

static void vt_store_cell(vt_tty_t* tty, uint16_t row, uint16_t col, uint32_t ch) {
    if (!tty || !tty->cells || row >= g_vt_rows || col >= g_vt_cols) {
        return;
    }
    uint32_t idx = vt_cell_index(row, col);
    tty->cells[idx].ch = ch;
    tty->cells[idx].fg = tty->fg;
    tty->cells[idx].bg = tty->bg;
    tty->cells[idx].attr = tty->attr;
}

static uint16_t vt_clamp_row(int32_t row) {
    if (row < 0) {
        return 0;
    }
    if (row >= (int32_t)g_vt_rows) {
        return (uint16_t)(g_vt_rows - 1u);
    }
    return (uint16_t)row;
}

static uint16_t vt_clamp_col(int32_t col) {
    if (col < 0) {
        return 0;
    }
    if (col >= (int32_t)g_vt_cols) {
        return (uint16_t)(g_vt_cols - 1u);
    }
    return (uint16_t)col;
}

static void vt_csi_reset(vt_tty_t* tty) {
    if (!tty) {
        return;
    }
    tty->csi_count = 0;
    tty->csi_current = 0;
    tty->csi_have_current = 0;
    tty->csi_private = 0;
}

static void vt_csi_push_param(vt_tty_t* tty) {
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

static uint16_t vt_csi_param(vt_tty_t* tty, uint8_t index, uint16_t def) {
    if (!tty) {
        return def;
    }
    if (index < tty->csi_count) {
        uint16_t v = tty->csi_params[index];
        return v == 0 ? def : v;
    }
    return def;
}

static void vt_clear_cell(vt_tty_t* tty, uint16_t row, uint16_t col, uint8_t render_now) {
    if (!tty || !tty->cells || row >= g_vt_rows || col >= g_vt_cols) {
        return;
    }
    vt_store_cell(tty, row, col, ' ');
    if (render_now) {
        vt_render_cell(tty, row, col);
    }
}

static void vt_apply_sgr(vt_tty_t* tty, uint16_t code) {
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

static void vt_apply_private_csi(uint32_t tty_index, vt_tty_t* tty, uint8_t final) {
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
    if ((tty_index != 0u) && (tty_index == g_active_tty) && !g_switch_barrier &&
        tty->cursor_visible) {
        vt_fb_set_cursor(tty);
    }
}

static void vt_apply_csi(uint32_t tty_index, vt_tty_t* tty, uint8_t final) {
    if (!tty) {
        return;
    }
    uint8_t render_now = (tty_index != 0) && (tty_index == g_active_tty) && !g_switch_barrier;
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
            for (uint16_t r = 0; r < g_vt_rows; ++r) {
                for (uint16_t c = 0; c < g_vt_cols; ++c) {
                    vt_clear_cell(tty, r, c, render_now);
                }
            }
        } else if (mode == 1) {
            for (uint16_t r = 0; r <= tty->cursor_row; ++r) {
                uint16_t max_col =
                    (r == tty->cursor_row) ? tty->cursor_col : (uint16_t)(g_vt_cols - 1u);
                for (uint16_t c = 0; c <= max_col; ++c) {
                    vt_clear_cell(tty, r, c, render_now);
                }
            }
        } else {
            for (uint16_t r = tty->cursor_row; r < g_vt_rows; ++r) {
                uint16_t start_col = (r == tty->cursor_row) ? tty->cursor_col : 0;
                for (uint16_t c = start_col; c < g_vt_cols; ++c) {
                    vt_clear_cell(tty, r, c, render_now);
                }
            }
        }
        break;
    }
    case 'K': {
        uint16_t mode = (tty->csi_count > 0) ? tty->csi_params[0] : 0;
        if (mode == 2) {
            for (uint16_t c = 0; c < g_vt_cols; ++c) {
                vt_clear_cell(tty, tty->cursor_row, c, render_now);
            }
        } else if (mode == 1) {
            for (uint16_t c = 0; c <= tty->cursor_col; ++c) {
                vt_clear_cell(tty, tty->cursor_row, c, render_now);
            }
        } else {
            for (uint16_t c = tty->cursor_col; c < g_vt_cols; ++c) {
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

static void vt_render_cell(const vt_tty_t* tty, uint16_t row, uint16_t col) {
    if (!tty || !tty->cells || row >= g_vt_rows || col >= g_vt_cols) {
        return;
    }
    uint32_t idx = vt_cell_index(row, col);
    const vt_cell_t* cell = &tty->cells[idx];
    uint32_t packed = vt_pack_cell_colors(cell);
    (void)vt_fb_send(FBTEXT_IPC_CELL_WRITE_REQ, (int32_t)col, (int32_t)row, (int32_t)cell->ch,
                     (int32_t)packed);
}

static int32_t vt_render_cell_switch(const vt_tty_t* tty, uint16_t row, uint16_t col) {
    if (!tty || !tty->cells || row >= g_vt_rows || col >= g_vt_cols) {
        return -1;
    }
    uint32_t idx = vt_cell_index(row, col);
    const vt_cell_t* cell = &tty->cells[idx];
    uint32_t packed = vt_pack_cell_colors(cell);
    return vt_fb_send_switch(FBTEXT_IPC_CELL_WRITE_REQ, (int32_t)col, (int32_t)row,
                             (int32_t)cell->ch, (int32_t)packed);
}

static void vt_scroll_up(vt_tty_t* tty, uint8_t render_now) {
    if (!tty || !tty->cells) {
        return;
    }

    for (uint16_t row = 1; row < g_vt_rows; ++row) {
        for (uint16_t col = 0; col < g_vt_cols; ++col) {
            uint32_t dst = vt_cell_index((uint16_t)(row - 1u), col);
            uint32_t src = vt_cell_index(row, col);
            tty->cells[dst] = tty->cells[src];
        }
    }
    for (uint16_t col = 0; col < g_vt_cols; ++col) {
        uint32_t idx = vt_cell_index((uint16_t)(g_vt_rows - 1u), col);
        tty->cells[idx].ch = ' ';
        tty->cells[idx].fg = tty->fg;
        tty->cells[idx].bg = tty->bg;
        tty->cells[idx].attr = tty->attr;
    }

    if (render_now) {
        (void)vt_fb_send(FBTEXT_IPC_SCROLL_REQ, 1, 0, 0, 0);
    }
}

static void vt_put_char_tty0(vt_tty_t* tty, uint8_t ch) {
    char c = (char)ch;
    if (tty) {
        if (c == '\r') {
            tty->cursor_col = 0;
        } else if (c == '\n') {
            tty->cursor_col = 0;
            if (tty->cursor_row + 1u >= g_vt_rows) {
                vt_scroll_up(tty, 0);
                tty->cursor_row = (uint16_t)(g_vt_rows - 1u);
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
            if (tty->cursor_col >= g_vt_cols) {
                tty->cursor_col = 0;
                if (tty->cursor_row + 1u >= g_vt_rows) {
                    vt_scroll_up(tty, 0);
                    tty->cursor_row = (uint16_t)(g_vt_rows - 1u);
                } else {
                    tty->cursor_row++;
                }
            }
        }
    }
    (void)wasmos_console_write(addr_cast(int32_t, &c), 1);
}

static void vt_put_char_virtual(vt_tty_t* tty, uint32_t tty_index, uint8_t ch) {
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
        if (tty->cursor_row + 1u >= g_vt_rows) {
            vt_scroll_up(tty, render_now);
            tty->cursor_row = (uint16_t)(g_vt_rows - 1u);
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
    if (tty->cursor_col >= g_vt_cols) {
        tty->cursor_col = 0;
        if (tty->cursor_row + 1u >= g_vt_rows) {
            vt_scroll_up(tty, render_now);
            tty->cursor_row = (uint16_t)(g_vt_rows - 1u);
        } else {
            tty->cursor_row++;
        }
    }
    if (render_now) {
        vt_fb_set_cursor(tty);
    }
}

static void vt_draw_tty0_hint(void) {
    vt_tty_t* tty = &g_ttys[0];
    tty->fg = 14;
    tty->bg = 0;
    tty->attr = 0;
    tty->cursor_row = 0;
    tty->cursor_col = 0;
    const char* line0 = "tty0 system console (read-only)";
    const char* line1 = "press F2/F3/F4 or Ctrl+Shift+F2/F3/F4";
    while (*line0) {
        vt_put_char_virtual(tty, 0, (uint8_t)*line0++);
    }
    vt_put_char_virtual(tty, 0, (uint8_t)'\n');
    while (*line1) {
        vt_put_char_virtual(tty, 0, (uint8_t)*line1++);
    }
    vt_put_char_virtual(tty, 0, (uint8_t)'\n');
    tty->fg = 15;
}

static void vt_process_byte(uint32_t tty_index, vt_tty_t* tty, uint8_t c) {
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
            uint32_t next = (uint32_t)tty->csi_current * 10u + (uint32_t)(c - '0');
            tty->csi_current = next > 0xFFFFu ? 0xFFFFu : (uint16_t)next;
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

/* Return the TTY index owning source_ep (as reader or writer), or -1. */
static int32_t vt_tty_index_for_source(int32_t source_ep) {
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

/* Redraw all cells of tty_index to the framebuffer.
 * reliable=1: uses vt_render_cell_switch and yields between rows to reduce
 *   FB queue saturation; dropped cells are tolerated (best-effort).
 * reliable=0: fast path for non-switch redraws. */
/* A cell that matches the framebuffer's cleared state (a blank space on the
 * default background) does not need replaying after an FBTEXT_IPC_CLEAR_REQ.
 * Skipping these collapses the replay of a mostly-empty slot (e.g. a CLI with a
 * prompt) from rows*cols cell writes to just the non-blank ones — the fix for
 * the ~80 s tty-switch wedge, where replaying a full 160x45 grid into the
 * framebuffer's depth-limited queue spun vt_fb_send_switch's per-cell retry. */
static int vt_cell_is_blank(const vt_cell_t* cell) {
    return (cell->ch == (uint32_t)' ' || cell->ch == 0u) && cell->bg == 0u;
}

static int32_t vt_replay_tty(uint32_t tty_index, uint8_t reliable) {
    if (tty_index >= VT_MAX_TTYS) {
        return -1;
    }
    vt_tty_t* tty = &g_ttys[tty_index];
    if (!tty->cells) {
        return -1;
    }

    /* Fast path: copy the whole grid into the shared blit buffer and repaint it
     * with a single IPC.  No per-cell loop, no yield, no queue storm. */
    if (g_blit_ready && g_blit_grid) {
        uint32_t cells = (uint32_t)g_vt_rows * (uint32_t)g_vt_cols;
        if (cells > VT_BLIT_MAX_CELLS) {
            cells = VT_BLIT_MAX_CELLS;
        }
        memcpy(g_blit_grid, tty->cells, cells * (uint32_t)sizeof(fbtext_blit_cell_t));
        (void)vt_fb_send_switch(FBTEXT_IPC_BLIT_GRID_REQ, (int32_t)g_vt_cols, (int32_t)g_vt_rows, 0,
                                0);
        vt_fb_set_cursor(tty);
        return 0;
    }

    uint32_t dropped_cells = 0;

    for (uint16_t row = 0; row < g_vt_rows; ++row) {
        for (uint16_t col = 0; col < g_vt_cols; ++col) {
            /* The framebuffer was just cleared to blanks; only paint the cells
             * that differ from that cleared state. */
            if (vt_cell_is_blank(&tty->cells[vt_cell_index(row, col)])) {
                continue;
            }
            if (reliable) {
                if (vt_render_cell_switch(tty, row, col) != 0) {
                    dropped_cells++;
                }
            } else {
                vt_render_cell(tty, row, col);
            }
        }
        if (reliable) {
            /* Give the framebuffer service an explicit scheduling window
             * between replay rows to reduce queue saturation bursts. */
            (void)wasmos_sched_yield();
        }
    }

    /* FIXME: replay currently remains best-effort under sustained framebuffer
     * backpressure; dropped cell updates are tolerated to avoid switch abort
     * loops and user-visible tty switch failures during stress. */
    (void)dropped_cells;
    vt_fb_set_cursor(tty);
    return 0;
}

static void vt_init_ttys(void) {
    uint32_t cell_count = vt_cell_capacity();
    if (cell_count == 0u || cell_count > (uint32_t)VT_MAX_COLS * (uint32_t)VT_MAX_ROWS) {
        return;
    }
    for (uint32_t i = 0; i < VT_MAX_TTYS; ++i) {
        vt_cell_t* cells = g_ttys[i].cells;
        memset(&g_ttys[i], 0, sizeof(g_ttys[i]));
        g_ttys[i].cells = cells;
        g_ttys[i].fg = 15;
        g_ttys[i].cursor_visible = 1;
        g_ttys[i].input_history_nav = -1;
        if (!cells) {
            continue;
        }
        memset(cells, 0, cell_count * (uint32_t)sizeof(vt_cell_t));
        for (uint32_t j = 0; j < cell_count; ++j) {
            cells[j].fg = 15;
        }
    }
    g_active_tty = 0;
    g_switch_generation = 1;
    g_switch_barrier = 0;
}

/* Tell the compositor (the vt-0 key sink) whether vt-0 is the visible slot, so
 * it owns the framebuffer only while visible. */
static void vt_notify_gfx_visibility(int32_t visible) {
    int32_t sink = g_tty_reader_ep[0];
    if (sink < 0) {
        return; /* compositor has not claimed vt-0 yet */
    }
    (void)wasmos_ipc_send(sink, g_vt_ep, VT_IPC_VIS_NOTIFY, 0, visible ? 1 : 0, 0, 0, 0);
}

static int32_t vt_switch_tty(uint32_t tty_index) {
    if (tty_index >= VT_MAX_TTYS) {
        return WASMOS_ERR_VT_BAD_TTY_ID;
    }
    if (tty_index == g_active_tty) {
        return 0;
    }

    uint32_t prev_active = g_active_tty;
    uint8_t prev_console_mode = (prev_active == 0) ? 1u : 0u;
    uint8_t next_console_mode = (tty_index == 0) ? 1u : 0u;
    g_switch_barrier = 1;

    /* Allow logical tty switching even when framebuffer control is unavailable
     * (startup races/headless mode). This keeps VT/CLI state consistent and
     * avoids reporting spurious switch failures to user-space. */
    if (g_fb_ep < 0) {
        g_switch_generation++;
        g_active_tty = tty_index;
        vt_trace_mark(VT_TRACE_SWITCH, (uint16_t)(tty_index & 0x0FFFu),
                      (uint16_t)(g_switch_generation & 0x0FFFu));
        g_switch_barrier = 0;
        return 0;
    }

    /* Leaving vt-0 for a text slot: the gfx overlay is locked (the compositor
     * owned the framebuffer), so unlock it BEFORE the clear/replay below.  The
     * framebuffer driver does not service cell writes while the overlay is
     * locked, so replaying ~rows*cols cells into a locked surface backs up its
     * IPC queue and spins vt_fb_send_switch's per-cell retry (up to
     * VT_FB_SWITCH_CELL_RETRIES) — an ~80 s wedge that looks like a hang. */
    if (prev_active == 0 && tty_index != 0) {
        /* Tell the compositor to stop drawing BEFORE the text slot is
         * repainted, so it cannot paint one more frame over the fresh grid. */
        vt_notify_gfx_visibility(0);
        (void)vt_fb_send_switch(FBTEXT_IPC_GFX_OVERLAY_REQ, 0, 0, 0, 0);
    }

    if (prev_console_mode != 0u) {
        if (vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 0, 0, 0, 0) != 0) {
            g_switch_barrier = 0;
            return WASMOS_ERR_VT_SWITCH_MODE_OFF;
        }
    }
    if (vt_fb_send_switch(FBTEXT_IPC_CLEAR_REQ, 0, 0, 0, 0) != 0) {
        if (prev_console_mode != 0u) {
            (void)vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 1, 0, 0, 0);
        } else {
            (void)vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 0, 0, 0, 0);
        }
        g_switch_barrier = 0;
        return WASMOS_ERR_VT_SWITCH_CLEAR;
    }
    if (vt_replay_tty(tty_index, 1) != 0) {
        if (prev_console_mode != 0u) {
            (void)vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 1, 0, 0, 0);
        } else {
            (void)vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 0, 0, 0, 0);
        }
        g_switch_barrier = 0;
        return WASMOS_ERR_VT_SWITCH_REPLAY;
    }
    if (next_console_mode != 0u &&
        vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 1, 0, 0, 0) != 0) {
        if (prev_console_mode != 0u) {
            (void)vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 1, 0, 0, 0);
        } else {
            (void)vt_fb_send_switch(FBTEXT_IPC_CONSOLE_MODE_REQ, 0, 0, 0, 0);
        }
        g_switch_barrier = 0;
        return WASMOS_ERR_VT_SWITCH_MODE_ON;
    }
    g_switch_generation++;
    g_active_tty = tty_index;
    vt_trace_mark(VT_TRACE_SWITCH, (uint16_t)(tty_index & 0x0FFFu),
                  (uint16_t)(g_switch_generation & 0x0FFFu));
    g_switch_barrier = 0;
    if (tty_index == 0) {
        /* Compositor is taking the framebuffer: lock the overlay so the fb
         * driver stops draining the console ring onto the gfx surface. */
        (void)vt_fb_send_switch(FBTEXT_IPC_GFX_OVERLAY_REQ, 1, 0, 0, 0);
        /* vt-0 is visible again: the compositor owns the framebuffer, so let it
         * repaint.  The overlay is already locked above, so its render lands on
         * a surface the framebuffer driver will not draw text over. */
        vt_notify_gfx_visibility(1);
    }
    /* (The vt-0 -> text "hidden" notify + overlay unlock happen before the
     * replay above, so the compositor stops before the text slot is repainted.) */
    return 0;
}

/* Active keyboard keymap (single decoder for the whole system).  Three layers,
 * indexed by PS/2 set-1 scancode 0..57.  Built-in default is US QWERTY (AltGr
 * layer all zero); vt_load_keymap() replaces it at init with a layout loaded
 * from /boot/system/keymaps, falling back to this built-in on any failure. */
static uint8_t g_km_plain[58] = {0,   0x1B, '1',  '2',  '3',  '4', '5', '6',  '7', '8', '9', '0',
                                 '-', '=',  '\b', '\t', 'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
                                 'o', 'p',  '[',  ']',  '\n', 0,   'a', 's',  'd', 'f', 'g', 'h',
                                 'j', 'k',  'l',  ';',  '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
                                 'b', 'n',  'm',  ',',  '.',  '/', 0,   '*',  0,   ' '};

static uint8_t g_km_shift[58] = {0,   0x1B, '!',  '@',  '#',  '$', '%', '^', '&', '*', '(', ')',
                                 '_', '+',  '\b', '\t', 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
                                 'O', 'P',  '{',  '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H',
                                 'J', 'K',  'L',  ':',  '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
                                 'B', 'N',  'M',  '<',  '>',  '?', 0,   '*', 0,   ' '};

static uint8_t g_km_altgr[58] = {0};

/* scancode -> character using the active keymap and modifier state.  AltGr wins
 * when its layer has a glyph for the key; otherwise Shift/plain.  Matches the
 * compositor's scancode_to_ascii precedence. */
static uint8_t vt_keymap_decode(uint32_t sc) {
    if (sc >= 58u) {
        return 0;
    }
    if (g_altgr_down && g_km_altgr[sc] != 0) {
        return g_km_altgr[sc];
    }
    return g_shift_down ? g_km_shift[sc] : g_km_plain[sc];
}

static uint32_t vt_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/* Parse one "0xNN" token starting at *p (skipping leading spaces/tabs). */
static int vt_parse_hex(const char** p, const char* end, uint32_t* out) {
    const char* q = *p;
    while (q < end && (*q == ' ' || *q == '\t')) {
        q++;
    }
    if (q + 2 > end || q[0] != '0' || (q[1] != 'x' && q[1] != 'X')) {
        return -1;
    }
    q += 2;
    uint32_t v = 0;
    int digits = 0;
    while (q < end) {
        char c = *q;
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        v = v * 16u + (uint32_t)d;
        digits++;
        q++;
    }
    if (digits == 0) {
        return -1;
    }
    *p = q;
    *out = v;
    return 0;
}

/* Parse a .kmap file (rows "<scancode> <plain> <shift> <altgr>" in hex) into the
 * active keymap.  Comment (#), blank, and non-data lines (e.g. "name ...") are
 * skipped.  Returns the number of scancode rows applied. */
static int vt_parse_keymap(const char* data, uint32_t len) {
    const char* p = data;
    const char* end = data + len;
    int rows = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && (*p == '#' || *p == '\n' || *p == '\r')) {
            while (p < end && *p != '\n') {
                p++;
            }
            if (p < end) {
                p++;
            }
            continue;
        }
        const char* save = p;
        uint32_t sc, pl, sh, ag;
        if (vt_parse_hex(&p, end, &sc) == 0 && vt_parse_hex(&p, end, &pl) == 0 &&
            vt_parse_hex(&p, end, &sh) == 0 && vt_parse_hex(&p, end, &ag) == 0 && sc < 58u) {
            g_km_plain[sc] = (uint8_t)pl;
            g_km_shift[sc] = (uint8_t)sh;
            g_km_altgr[sc] = (uint8_t)ag;
            rows++;
        } else {
            p = save;
        }
        while (p < end && *p != '\n') {
            p++;
        }
        if (p < end) {
            p++;
        }
    }
    return rows;
}

/* Read a file by path through the fs (owner-push: acquire a buffer, stage the
 * path, grant fs RW, fs writes the contents back).  Runs during init, before
 * the keyboard/serial subscriptions, so no stray messages contend for g_vt_ep. */
static int vt_read_keymap_file(const char* path, char* out, uint32_t out_cap, uint32_t* out_len) {
    if (g_fs_ep < 0) {
        return -1;
    }
    uint32_t plen = vt_strlen(path);
    if (plen == 0 || plen >= out_cap) {
        return -1;
    }
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)out_cap);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), (int32_t)plen, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    int32_t b1 = wasmos_xfer_buffer_borrow(g_fs_ep, bid,
                                           WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    int32_t req = 0x4B4D; /* "KM" */
    if (wasmos_ipc_send(g_fs_ep, g_vt_ep, FS_IPC_READ_PATH_REQ, req, (int32_t)plen, 0, bid, b1) !=
        0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    int32_t total = -1;
    for (int i = 0; i < 64; ++i) {
        if (wasmos_ipc_select_one(g_vt_ep) < 0) {
            break;
        }
        if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req) {
            continue;
        }
        if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) == FS_IPC_RESP) {
            total = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        }
        break;
    }
    if (total <= 0 || (uint32_t)total > out_cap) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_xfer_buffer_read(bid, addr_cast(int32_t, out), total, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    *out_len = (uint32_t)total;
    return 0;
}

/* Load the active layout from /boot/system/keymaps; keep the built-in US on
 * failure so keyboard input always works. */
static void vt_load_keymap(const char* path) {
    static char km_buf[4096];
    uint32_t len = 0;
    if (vt_read_keymap_file(path, km_buf, sizeof(km_buf), &len) != 0) {
        printf("[vt] keymap load failed (%s); using built-in US\n", path);
        return;
    }
    int rows = vt_parse_keymap(km_buf, len);
    printf("[vt] keymap loaded: %s (%d rows)\n", path, rows);
}

static void vt_input_echo_char(uint32_t tty_index, uint8_t ch) {
    if (tty_index >= VT_MAX_TTYS || tty_index != g_active_tty) {
        return;
    }
    vt_tty_t* tty = &g_ttys[tty_index];
    vt_process_byte(tty_index, tty, ch);
}

static void vt_input_commit_line(vt_tty_t* tty) {
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

static void vt_input_history_store(vt_tty_t* tty) {
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

static void vt_input_replace_line(uint32_t tty_index, vt_tty_t* tty, const uint8_t* line,
                                  uint16_t len) {
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

static void vt_input_history_nav(uint32_t tty_index, vt_tty_t* tty, uint8_t older) {
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

static void vt_input_handle_char(uint32_t tty_index, uint8_t ch) {
    if (tty_index >= VT_MAX_TTYS) {
        return;
    }
    vt_tty_t* tty = &g_ttys[tty_index];
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
        if (ch < 0x20) {
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

static void vt_set_input_mode(vt_tty_t* tty, uint8_t mode) {
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

/* Forward a decoded key event to the compositor (the vt-0 key sink), which maps
 * it to a GFX_EVENT_KEY for the focused window.  The vt is the single decoder,
 * so the message carries the decoded character plus the raw scancode and
 * modifier state.
 * arg0=ascii(keysym, 0 if none), arg1=scancode, arg2=flags
 * (bit0=down, bit1=extended, bit2=shift, bit3=ctrl, bit4=altgr). */
static void vt_forward_key_to_compositor(int32_t scancode, int32_t keyup, int32_t extended) {
    int32_t sink = g_tty_reader_ep[0];
    if (sink < 0) {
        return; /* compositor has not claimed vt-0 yet */
    }
    uint8_t ascii =
        (!extended && scancode > 0 && scancode < 58) ? vt_keymap_decode((uint32_t)scancode) : 0;
    int32_t flags = (keyup == 0 ? 1 : 0) | (extended ? 2 : 0) | (g_shift_down ? 4 : 0) |
                    (g_ctrl_down ? 8 : 0) | (g_altgr_down ? 16 : 0);
    (void)wasmos_ipc_send(sink, g_vt_ep, VT_IPC_KEY_FORWARD, 0, (int32_t)ascii, scancode, flags, 0);
}

static void vt_handle_key_notify(int32_t scancode, int32_t keyup, int32_t extended) {
    if (scancode == 0x1D) { /* Ctrl (left + extended right) */
        g_ctrl_down = keyup ? 0 : 1;
        return;
    }
    if (scancode == 0x2A || scancode == 0x36) { /* Left/Right Shift */
        g_shift_down = keyup ? 0 : 1;
        return;
    }
    if (scancode == 0x38 && extended) { /* AltGr (right Alt) */
        g_altgr_down = keyup ? 0 : 1;
        return;
    }

    /* vt-0 is the compositor's slot: forward every event (press and release) to
     * it after the vt-owned switch hotkeys.  This is the sole keyboard path for
     * gfx apps; the compositor does not subscribe to the keyboard driver. */
    if (g_active_tty == 0) {
        if (keyup == 0) {
            if (scancode >= 0x3C && scancode <= 0x3E) { /* F2..F4 => tty1..tty3 */
                (void)vt_switch_tty((uint32_t)(scancode - 0x3B));
                return;
            }
            if (g_ctrl_down && g_shift_down && scancode >= 0x3B &&
                scancode <= 0x3E) { /* Ctrl+Shift+F1..F4 => tty0..tty3 */
                (void)vt_switch_tty((uint32_t)(scancode - 0x3B));
                return;
            }
        }
        vt_forward_key_to_compositor(scancode, keyup, extended);
        return;
    }

    /* Text slots: only key-down produces input. */
    if (keyup != 0) {
        return;
    }

    if (extended && g_active_tty != 0) {
        vt_tty_t* tty = &g_ttys[g_active_tty];
        /* Extended set-1 keys include arrows plus nav/edit cluster. */
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

        if (scancode == 0x48) { /* Up */
            vt_input_q_push_escape(tty, 'A');
            return;
        } else if (scancode == 0x50) { /* Down */
            vt_input_q_push_escape(tty, 'B');
            return;
        } else if (scancode == 0x4D) { /* Right */
            vt_input_q_push_escape(tty, 'C');
            return;
        } else if (scancode == 0x4B) { /* Left */
            vt_input_q_push_escape(tty, 'D');
            return;
        } else if (scancode == 0x47) { /* Home */
            (void)vt_input_q_push(tty, 0x1B);
            (void)vt_input_q_push(tty, '[');
            (void)vt_input_q_push(tty, 'H');
            return;
        } else if (scancode == 0x4F) { /* End */
            (void)vt_input_q_push(tty, 0x1B);
            (void)vt_input_q_push(tty, '[');
            (void)vt_input_q_push(tty, 'F');
            return;
        } else if (scancode == 0x49) { /* Page Up */
            (void)vt_input_q_push(tty, 0x1B);
            (void)vt_input_q_push(tty, '[');
            (void)vt_input_q_push(tty, '5');
            (void)vt_input_q_push(tty, '~');
            return;
        } else if (scancode == 0x51) { /* Page Down */
            (void)vt_input_q_push(tty, 0x1B);
            (void)vt_input_q_push(tty, '[');
            (void)vt_input_q_push(tty, '6');
            (void)vt_input_q_push(tty, '~');
            return;
        } else if (scancode == 0x52) { /* Insert */
            (void)vt_input_q_push(tty, 0x1B);
            (void)vt_input_q_push(tty, '[');
            (void)vt_input_q_push(tty, '2');
            (void)vt_input_q_push(tty, '~');
            return;
        } else if (scancode == 0x53) { /* Delete */
            (void)vt_input_q_push(tty, 0x1B);
            (void)vt_input_q_push(tty, '[');
            (void)vt_input_q_push(tty, '3');
            (void)vt_input_q_push(tty, '~');
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
        if (scancode == 0x16) {        /* U */
            ch = 0x15;                 /* NAK / Ctrl+U */
        } else if (scancode == 0x2E) { /* C */
            ch = 0x03;                 /* ETX / Ctrl+C */
        } else if (scancode == 0x19) { /* P */
            ch = 0x10;                 /* DLE / Ctrl+P */
        } else if (scancode == 0x31) { /* N */
            ch = 0x0E;                 /* SO / Ctrl+N */
        }
    }
    if (ch == 0) {
        if (scancode <= 0 || scancode >= 58) {
            return;
        }
        ch = vt_keymap_decode((uint32_t)scancode);
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

/* Service entry point.  Registers "vt", looks up "fb" and "kbd", allocates
 * cell grids (retries with default geometry if large-grid alloc fails), then
 * subscribes to keyboard events and enters the main IPC receive loop.
 * g_switch_generation is a monotonic counter incremented on TTY switches;
 * write messages with a stale generation are silently dropped. */
/* Create the VT-owned klog ring, map it into the VT's linear memory, initialize
 * the SPSC header, and hand its xfer-buffer id to the kernel.  Best-effort: on
 * any failure g_klog_ring_ready stays 0 and klog reaches only the legacy
 * console_ring / COM1 TX. */
static void vt_klog_ring_init(void) {
    /* Overlay the SPSC ring on a BUFFER_KIND_TRANSFER xfer-buffer — the same
     * zero-copy transport the socket rings use (net.h), which handles WARP's
     * non-page-aligned linear-memory base for us (unlike raw shmem_map).  The
     * VT's heap_pages/INITIAL_MEMORY (see CMakeLists + linker.metadata) keep
     * WARP's scan ceiling under the 2 MiB low-guard so the overlay window lands
     * just above the VT's active memory, inside declared linear memory — a cheap
     * page-fault-in, not a multi-MiB commit that would stall the CLI handshake
     * or force the commit-probe to extend the module (which throws under WARP). */
    uint32_t region = wasmos_ringbuf_bytes_for(VT_KLOG_RING_CAPACITY);
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)region);
    if (bid <= 0) {
        return;
    }
    int32_t off = wasmos_xfer_buffer_map(bid);
    if (off < 0) {
        return;
    }
    void* base = ptr_cast(void, (uint32_t)off);
    if (wasmos_ringbuf_init(&g_klog_ring, base, region, VT_KLOG_RING_CAPACITY) != 0) {
        return;
    }
    if (wasmos_klog_register_ring(bid) != 0) {
        return;
    }
    g_klog_ring_ready = 1;
}

/* Drain any pending klog bytes into vt-1 through the normal per-slot byte path
 * (cell buffer updated; rendered only if vt-1 is the visible slot).  Never
 * targets vt-0 (tty0's byte path calls wasmos_console_write, which would loop
 * back into klog). */
static void vt_drain_klog_ring(void) {
    if (!g_klog_ring_ready) {
        return;
    }
    vt_tty_t* tty = &g_ttys[VT_KLOG_TTY];
    uint8_t buf[128];
    for (;;) {
        uint32_t n = wasmos_ringbuf_read(&g_klog_ring, buf, (uint32_t)sizeof(buf));
        if (n == 0) {
            break;
        }
        for (uint32_t i = 0; i < n; ++i) {
            vt_process_byte(VT_KLOG_TTY, tty, buf[i]);
        }
    }
}

/* Set up the shared cell-grid blit buffer: acquire an xfer-buffer, map it into
 * the VT's linear memory (write side), grant the framebuffer driver READ
 * access, and hand it the buffer/borrow ids so it can map and render from it.
 * Best-effort: on failure g_blit_ready stays 0 and vt_replay_tty falls back to
 * per-cell writes. */
static void vt_blit_init(void) {
    if (g_fb_ep < 0) {
        return;
    }
    uint32_t bytes = VT_BLIT_MAX_CELLS * (uint32_t)sizeof(fbtext_blit_cell_t);
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)bytes);
    if (bid <= 0) {
        return;
    }
    int32_t off = wasmos_xfer_buffer_map(bid);
    if (off < 0) {
        return;
    }
    int32_t grant = wasmos_xfer_buffer_borrow(g_fb_ep, bid, WASMOS_BUFFER_GRANT_READ);
    if (grant < 0) {
        return;
    }
    /* Hand the driver the ids to map; it renders on FBTEXT_IPC_BLIT_GRID_REQ. */
    if (vt_fb_send_switch(FBTEXT_IPC_BLIT_ATTACH_REQ, bid, grant, (int32_t)g_vt_cols,
                          (int32_t)g_vt_rows) != 0) {
        return;
    }
    g_blit_grid = ptr_cast(fbtext_blit_cell_t, (uint32_t)off);
    g_blit_ready = 1;
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t arg1, int32_t arg2,
                                      int32_t arg3) {
    /* proc.endpoint now comes from the spawn-info contract, not an entry arg. */
    proc_endpoint = wasmos_startup_proc_endpoint();
    (void)arg1;
    (void)arg2;
    (void)arg3;

    g_vt_ep = wasmos_ipc_create_endpoint();
    if (g_vt_ep < 0) {
        return -1;
    }
    if (wasmos_svc_register(proc_endpoint, g_vt_ep, "vt", 1) != 0) {
        return -1;
    }
    g_fb_ep = wasmos_svc_lookup(proc_endpoint, g_vt_ep, "fb", 2);
    g_kbd_ep = wasmos_svc_lookup(proc_endpoint, g_vt_ep, "kbd", 3);

    vt_query_geometry();
    vt_reset_tty_cells();
    vt_heap_init();
    if (vt_alloc_tty_cells() != 0) {
        vt_log_alloc_failure("runtime-grid", g_alloc_failure);
        /* Fallback if large-grid allocation fails under tight memory. */
        g_vt_cols = VT_COLS_DEFAULT;
        g_vt_rows = VT_ROWS_DEFAULT;
        vt_reset_tty_cells();
        if (vt_alloc_tty_cells() != 0) {
            vt_log_alloc_failure("default-grid", g_alloc_failure);
            return -1;
        }
    }
    vt_init_ttys();

    /* Load the active keyboard layout from the FS before subscribing to input,
     * so the FS reply is the only traffic on g_vt_ep during the synchronous
     * read.  Falls back to the built-in US keymap if the FS or file is
     * unavailable. */
    g_fs_ep = wasmos_sys_svc_lookup_retry(proc_endpoint, g_vt_ep, "fs.vfs", 4, 64);
    vt_load_keymap("/boot/system/keymaps/de-nodeadkeys.kmap");

    if (g_kbd_ep != -1) {
        (void)wasmos_ipc_send(g_kbd_ep, g_vt_ep, KBD_IPC_SUBSCRIBE_REQ, 1, 0, 0, 0, 0);
    }

    /* Subscribe to the serial driver so COM1 RX is delivered as
     * VT_IPC_SERIAL_INPUT_REQ pushes into the serial-bound slot.  The serial
     * driver comes up before the vt, but retry a few times to cover boot skew. */
    for (int attempt = 0; attempt < 8 && g_serial_in_ep < 0; ++attempt) {
        g_serial_in_ep = wasmos_svc_lookup(proc_endpoint, g_vt_ep, "sin", 4);
        if (g_serial_in_ep < 0) {
            wasmos_sched_yield();
        }
    }
    if (g_serial_in_ep >= 0) {
        (void)wasmos_ipc_send(g_serial_in_ep, g_vt_ep, SERIAL_IPC_SUBSCRIBE_REQ, 1, 0, 0, 0, 0);
    }

    if (g_fb_ep != -1) {
        vt_fb_console_mode(1);
        /* Share the cell-grid blit buffer with the framebuffer driver so tty
         * switches repaint in one IPC instead of a per-cell storm. */
        vt_blit_init();
    }

    /* Bring up the VT-owned klog ring (small, cheap overlay — see
     * vt_klog_ring_init).  The VT drains it into vt-1 on every wake below: it
     * blocks on g_vt_ep with wasmos_ipc_select_one (a timed select set strands
     * serial input under WARP), so klog reaches vt-1 whenever any IPC arrives.
     * An idle VT does not drain until the next event, which is fine: vt-1 is not
     * the visible slot, and COM1 TX always carries the full log. */
    vt_klog_ring_init();

    wasmos_sys_notify_ready(proc_endpoint, g_vt_ep);

    for (;;) {
        int32_t rc = wasmos_ipc_select_one(g_vt_ep);
        if (rc < 0) {
            wasmos_sched_yield();
            continue;
        }
        vt_drain_klog_ring();

        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);

        switch ((uint32_t)msg.type) {
        case VT_IPC_WRITE_REQ: {
            int32_t tty_index = -1;
            if (msg.source < 0) {
                /* Kernel-originated mirrored console writes target whichever TTY
                 * is active. They are advisory and intentionally bypass writer
                 * ownership plus generation checks. */
                tty_index = (int32_t)g_active_tty;
            } else {
                tty_index = vt_tty_index_for_source(msg.source);
            }
            if (tty_index < 0 || tty_index >= (int32_t)VT_MAX_TTYS) {
                vt_trace_mark(
                    VT_TRACE_DROP_UNOWNED,
                    (uint16_t)(msg.source < 0 ? 0x0FFFu : ((uint32_t)msg.source & 0x0FFFu)), 0);
                break;
            }
            if (msg.source >= 0 && (uint32_t)msg.request_id != g_switch_generation) {
                /* Drop stale write chunks queued before the last tty switch. */
                vt_trace_mark(VT_TRACE_DROP_STALE, (uint16_t)((uint32_t)tty_index & 0x0FFFu),
                              (uint16_t)(((uint32_t)msg.request_id) & 0x0FFFu));
                break;
            }
            /* FIXME: rapid Ctrl+Shift+Fn switching can intermittently render
             * duplicated/misaligned prompts (framebuffer-only; the cell grid is
             * correct). No reliable repro sequence exists yet, which is why the
             * VT trace markers (switch/write-drop/register events) stay enabled. */
            vt_tty_t* tty = &g_ttys[(uint32_t)tty_index];
            int32_t args[4] = {msg.arg0, msg.arg1, msg.arg2, msg.arg3};
            int count = (args[0] >> 24) & 0xF;
            if (count > 4)
                count = 4;
            args[0] &= 0xFF;
            for (int i = 0; i < count; ++i) {
                vt_process_byte((uint32_t)tty_index, tty, (uint8_t)(args[i] & 0xFF));
            }
            break;
        }

        case VT_IPC_SERIAL_INPUT_REQ: {
            /* COM1 RX from the serial driver -> the serial-bound slot's input
             * queue (line discipline), regardless of which slot is visible.
             * Bytes are packed like VT_IPC_WRITE_REQ. */
            int32_t args[4] = {msg.arg0, msg.arg1, msg.arg2, msg.arg3};
            int count = (args[0] >> 24) & 0xF;
            if (count > 4)
                count = 4;
            args[0] &= 0xFF;
            for (int i = 0; i < count; ++i) {
                vt_input_handle_char(g_serial_tty, (uint8_t)(args[i] & 0xFF));
            }
            break;
        }

        case SERIAL_IPC_SUBSCRIBE_RESP:
            /* Ack of the VT's serial subscription; nothing to do. */
            break;

        case VT_IPC_SET_ATTR_REQ: {
            int32_t tty_index = vt_tty_index_for_source(msg.source);
            if (tty_index < 0 || tty_index >= (int32_t)VT_MAX_TTYS) {
                if (msg.source >= 0 && msg.request_id != 0) {
                    (void)vt_ipc_reply_retry(msg.source, VT_IPC_ERROR, msg.request_id,
                                             WASMOS_ERR_VT_NO_TTY_FOR_SOURCE, 0);
                }
                break;
            }
            vt_tty_t* tty = &g_ttys[(uint32_t)tty_index];
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
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id, 0, 0);
            }
            break;
        }

        case VT_IPC_SWITCH_TTY: {
            if (msg.arg0 == 0 && msg.source >= 0 && g_tty_reader_ep[0] < 0) {
                /* The first client to take vt-0 (the compositor, at startup)
                 * becomes its key/visibility sink; the vt forwards decoded keys
                 * (VT_IPC_KEY_FORWARD) and visibility (VT_IPC_VIS_NOTIFY) there.
                 * A later `tty 0` (e.g. from a CLI switching the visible slot
                 * back) must NOT reclaim it, or those notifies would misroute. */
                g_tty_reader_ep[0] = msg.source;
            }
            int32_t sw = vt_switch_tty((uint32_t)msg.arg0);
            if (msg.source >= 0 && msg.request_id != 0) {
                (void)vt_ipc_reply_retry(
                    msg.source, (sw == 0) ? VT_IPC_RESP : VT_IPC_ERROR, msg.request_id,
                    (sw == 0) ? (int32_t)g_switch_generation : sw, (int32_t)g_active_tty);
            }
            break;
        }

        case VT_IPC_GET_ACTIVE_TTY:
            if (msg.source >= 0 && msg.request_id != 0) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id,
                                         (int32_t)g_switch_generation, (int32_t)g_active_tty);
            }
            break;

        case VT_IPC_REGISTER_WRITER: {
            if (msg.source < 0 || msg.request_id == 0) {
                break;
            }
            int32_t tty_id = msg.arg0;
            if (tty_id < 0 || tty_id >= (int32_t)VT_MAX_TTYS) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_ERROR, msg.request_id,
                                         WASMOS_ERR_VT_BAD_TTY_ID, 0);
                break;
            }
            uint32_t idx = (uint32_t)tty_id;
            if (g_tty_writer_ep[idx] >= 0 && g_tty_writer_ep[idx] != msg.source) {
                vt_trace_mark(VT_TRACE_WRITER_CONFLICT, (uint16_t)(idx & 0x0FFFu),
                              (uint16_t)((uint32_t)msg.source & 0x0FFFu));
                /* Replace stale/previous writer ownership instead of rejecting
                 * new registrations. This keeps CLI recovery robust when a
                 * prior writer process exited without an explicit unregister. */
            }
            g_tty_writer_ep[idx] = msg.source;
            vt_trace_mark(VT_TRACE_WRITER_OK, (uint16_t)(idx & 0x0FFFu),
                          (uint16_t)((uint32_t)msg.source & 0x0FFFu));
            (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id,
                                     (int32_t)g_switch_generation, tty_id);
            break;
        }

        case VT_IPC_READ_REQ: {
            if (msg.source < 0 || msg.request_id == 0) {
                break;
            }
            int32_t tty_id = msg.arg0;
            if (tty_id < 0 || tty_id >= (int32_t)VT_MAX_TTYS) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_ERROR, msg.request_id,
                                         WASMOS_ERR_VT_BAD_TTY_ID, 0);
                break;
            }
            if (g_tty_reader_ep[(uint32_t)tty_id] < 0) {
                g_tty_reader_ep[(uint32_t)tty_id] = msg.source;
            } else if (g_tty_reader_ep[(uint32_t)tty_id] != msg.source) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_ERROR, msg.request_id,
                                         WASMOS_ERR_VT_READER_BUSY, 0);
                break;
            }
            uint8_t ch = 0;
            if (vt_input_q_pop(&g_ttys[(uint32_t)tty_id], &ch) == 0) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id, 0, (int32_t)ch);
            } else {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id, 1, 0);
            }
            break;
        }

        case VT_IPC_SET_MODE_REQ: {
            if (msg.source < 0 || msg.request_id == 0) {
                break;
            }
            int32_t tty_index = vt_tty_index_for_source(msg.source);
            if (tty_index < 0 || tty_index >= (int32_t)VT_MAX_TTYS) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_ERROR, msg.request_id,
                                         WASMOS_ERR_VT_NO_TTY_FOR_SOURCE, 0);
                break;
            }
            uint8_t mode = (uint8_t)(msg.arg0 & (VT_INPUT_MODE_CANONICAL | VT_INPUT_MODE_ECHO));
            vt_set_input_mode(&g_ttys[(uint32_t)tty_index], mode);
            (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id, (int32_t)mode,
                                     tty_index);
            break;
        }

        case KBD_IPC_KEY_NOTIFY:
            vt_handle_key_notify(msg.arg0, msg.arg1, msg.arg2);
            break;

        case KBD_IPC_SUBSCRIBE_RESP:
            break;

        default:
            if (msg.source >= 0 && msg.request_id != 0) {
                (void)vt_ipc_reply_retry(msg.source, VT_IPC_ERROR, msg.request_id,
                                         WASMOS_ERR_VT_UNSUPPORTED_REQUEST, 0);
            }
            break;
        }
    }

    return 0;
}
