/* vt_main.c - virtual terminal service: owns VT_MAX_TTYS text slots with
 * VT100/ANSI emulation, decodes every keyboard event for the system (the single
 * keymap decoder), and renders the visible slot through the "fb" framebuffer
 * driver.  Slot vt-0 belongs to the gfx compositor: for that slot the vt
 * forwards decoded keys (VT_IPC_KEY_FORWARD) and visibility changes
 * (VT_IPC_VIS_NOTIFY) instead of painting text itself.
 *
 * Output does not arrive over IPC.  The kernel publishes the kernel log and
 * every process's console output into the vt's console ring, tagged with the
 * writer's controlling slot; the vt merges each slot's stream (vt_outmux.h),
 * renders it, and owns COM1 for the serial-bound slot.  See the console-ring
 * comment below.
 *
 * IPC surface, as served by the loop in initialize().  The slot model, the
 * reader/writer registration rules and the role of slot 0 and slot 1 are
 * documented in vt_types.h; opcode values and request packing come from
 * abi/opcodes.yaml.  Replies carry only two payload words (arg0, arg1):
 *
 *   VT_IPC_SERIAL_INPUT_REQ  1-4 bytes from the serial driver, arg0[27:24] =
 *                          count; they enter the serial-bound slot's line
 *                          discipline.  Not acknowledged.
 *   VT_IPC_READ_REQ        arg0=slot.  RESP arg0=0 with arg1=the byte, or
 *                          arg0=1 with arg1=0 when the queue is empty (a
 *                          non-blocking poll, not a wait).  The first caller
 *                          claims the slot's reader role.
 *   VT_IPC_SET_ATTR_REQ    arg0=fg, arg1=bg, arg2=attr (low byte of each); a
 *                          colour above 15 is ignored, attr is taken as-is.
 *                          RESP arg0=0.
 *   VT_IPC_SWITCH_TTY      arg0=slot.  RESP arg0=the new switch generation,
 *                          arg1=the now-visible slot.
 *   VT_IPC_GET_ACTIVE_TTY  RESP arg0=switch generation, arg1=visible slot.
 *   VT_IPC_REGISTER_WRITER arg0=slot.  RESP arg0=switch generation, arg1=slot.
 *   VT_IPC_SET_MODE_REQ    arg0=VT_INPUT_MODE_* bits (other bits are masked
 *                          off).  RESP arg0=the mode applied, arg1=the slot.
 *   VT_IPC_BIND_SERIAL_REQ arg0=slot (>= 1).  Moves the serial console to that
 *                          slot, independently of the visible one.  RESP
 *                          arg0=the bound slot.
 *
 * Every failure answers VT_IPC_ERROR with arg0 set to a packed vt-domain code
 * from abi/errors.yaml, including an unrecognised opcode
 * (WASMOS_ERR_VT_UNSUPPORTED_REQUEST).  A request that arrives with request_id 0
 * or from a negative source is never answered, since there is nowhere to reply
 * to. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"
#include "vt_types.h"
#include "vt_outmux.h"

#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

static int32_t g_vt_ep = -1;
static int32_t g_kbd_ep = -1;
static int32_t g_fb_ep = -1;
static vt_tty_t g_ttys[VT_MAX_TTYS];
/* The system console slot: kernel klog drains here and it is the default visible
 * and serial-bound slot.  vt-0 is the GUI slot (the compositor), never a text
 * console, which is why the console cannot be slot 0
 * (docs/architecture/19-virtual-terminal.md, slot model). */
#define VT_KLOG_TTY 1u
static uint32_t g_active_tty = VT_KLOG_TTY;
/* Slot bound to the serial console: RX from the serial driver is injected here,
 * regardless of which slot is visible.  Defaults to the system console and moves
 * only on VT_IPC_BIND_SERIAL_REQ, never on a visible-slot switch. */
static uint32_t g_serial_tty = VT_KLOG_TTY;
static int32_t g_serial_in_ep = -1;
static int32_t g_fs_ep = -1;

/* The vt's one receive pump: a libsys event loop on g_vt_ep. Replies to the
 * vt's own requests are loop intents, and the four peers it depends on are
 * service-name watches (fb, kbd, sin, fs.vfs) that report each peer when it
 * registers and again when a restarted one registers anew. Everything else is
 * client traffic, handled by vt_on_message as the loop's default.
 *
 * Intent request ids start at VT_LOOP_REQ_BASE because clients' requests share
 * this endpoint and are matched against pending intents first; the base keeps
 * the two id spaces apart. */
#define VT_LOOP_REQ_BASE 0x7E000000
static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_watch_fb;
static wasmos_sys_svc_watch_t g_watch_kbd;
static wasmos_sys_svc_watch_t g_watch_sin;
static wasmos_sys_svc_watch_t g_watch_fs;
static int32_t g_tty_reader_ep[VT_MAX_TTYS] = {-1, -1, -1, -1};
static int32_t g_tty_writer_ep[VT_MAX_TTYS] = {-1, -1, -1, -1};
static uint32_t g_switch_generation = 1;
static uint8_t g_switch_barrier = 0;
/* Suppresses per-cell rendering while a batch of bytes is written to a slot.  The
 * caller repaints the whole grid with one blit afterwards, which is what keeps a
 * klog burst from becoming one IPC per character. */
static uint8_t g_render_defer = 0;
static uint8_t g_ctrl_down = 0;
static uint8_t g_shift_down = 0;
static uint8_t g_altgr_down = 0;
static uint16_t g_vt_cols = VT_COLS_DEFAULT;
static uint16_t g_vt_rows = VT_ROWS_DEFAULT;
static uint32_t g_heap_cursor = 0;
static uint32_t g_heap_limit = 0;
static int32_t g_alloc_failure = 0;

/* Console ring: the VT owns an SPSC ring overlaid on an xfer buffer, and the
 * kernel publishes into it, as slot-tagged records, both the kernel log
 * (VT_CONSOLE_SLOT_KLOG, drained into VT_KLOG_TTY) and every process's console
 * output (tagged with its controlling slot).  The main loop drains after every
 * wake of the event loop, and the kernel's VT_IPC_KLOG_NOTIFY doorbell is one
 * such wake, so pending output is drained even when nothing else happens.
 *
 * Registering the ring also hands COM1 to the VT: the kernel stops transmitting
 * what it publishes, and the VT writes the serial-bound slot's output stream
 * through wasmos_serial_owner_write.  Text the ring cannot take is transmitted
 * by the kernel directly, so a full ring reorders serial output but loses none.
 * Sized for a boot-time log burst between two VT wakes. */
#define VT_KLOG_RING_CAPACITY 16384u /* SPSC data capacity (power of two) */
static wasmos_ringbuf_t g_klog_ring;
static int32_t g_klog_ring_ready = 0;

/* Per-slot merge of writer output and kernel log (vt_outmux.h): the single
 * stream that is rendered into the slot and, for the serial-bound slot, sent to
 * serial.  Slot 0 is never a text slot and its entry is unused. */
static vt_outmux_t g_outmux[VT_MAX_TTYS];

/* Serial-bound slot output not yet handed to wasmos_serial_owner_write.  Flushed
 * when full and once per main-loop iteration, so a burst costs one host call
 * per buffer rather than one per byte. */
static uint8_t g_serial_out[512];
static uint32_t g_serial_out_len = 0;

/* Bulk grid blit: a shared xfer-buffer holding the visible slot's cell grid,
 * granted READ to the framebuffer driver.  A tty switch repaints the whole
 * screen with a single FBTEXT_IPC_BLIT_GRID_REQ; one cell-write IPC per cell
 * storms the driver's queue and wedges switching under SMP.  Sized for the
 * maximum grid so it survives any geometry. */
#define VT_BLIT_MAX_CELLS ((uint32_t)VT_MAX_COLS * (uint32_t)VT_MAX_ROWS)
static fbtext_blit_cell_t* g_blit_grid = 0; /* VT-side mapped write pointer */
static int32_t g_blit_ready = 0;
static int32_t g_blit_bid = -1; /* the buffer behind g_blit_grid, -1 when none */
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

/* Allocate one cell grid per TTY from the bump heap; sets cells pointer. Each is
 * sized for the MAXIMUM geometry, because the framebuffer may report its real
 * one only after the vt is serving -- it can come up later, or restart -- and the
 * bump heap cannot reallocate; vt_regrid then only changes the stride. */
static int vt_alloc_tty_cells(void) {
    uint32_t bytes = (uint32_t)VT_MAX_COLS * (uint32_t)VT_MAX_ROWS * (uint32_t)sizeof(vt_cell_t);
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

/* Whether a write to `tty_index` should paint the framebuffer as it goes: only
 * the visible slot renders, never during a switch (the replay repaints it), and
 * never while a batch is deferring to a closing blit. */
static uint8_t vt_render_live(uint32_t tty_index) {
    return (uint8_t)((tty_index == g_active_tty) && !g_switch_barrier && !g_render_defer);
}

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

/* Re-lay every slot's grid out for a new geometry, in place. The grids are
 * allocated at VT_MAX_COLS x VT_MAX_ROWS capacity, so only the row stride
 * changes: the top-left min(rows) x min(cols) of each slot is kept, the rest is
 * blank, and cursors are clamped. A narrower stride copies forward (every
 * destination index is at or below its source), a wider one backward (at or
 * above), so no cell is overwritten before it is read. */
static void vt_regrid(uint16_t cols, uint16_t rows) {
    uint16_t old_cols = g_vt_cols;
    uint16_t keep_rows = rows < g_vt_rows ? rows : g_vt_rows;
    uint16_t keep_cols = cols < old_cols ? cols : old_cols;
    vt_cell_t blank = {.ch = 0u, .fg = 15u, .bg = 0u, .attr = 0u, ._pad = 0u};

    if (cols == g_vt_cols && rows == g_vt_rows) {
        return;
    }
    for (uint32_t t = 0; t < VT_MAX_TTYS; ++t) {
        vt_tty_t* tty = &g_ttys[t];
        vt_cell_t* cells = tty->cells;
        if (!cells) {
            continue;
        }
        if (cols <= old_cols) {
            for (uint32_t r = 0; r < keep_rows; ++r) {
                for (uint32_t c = 0; c < cols; ++c) {
                    cells[r * cols + c] = cells[r * old_cols + c];
                }
            }
        } else {
            for (uint32_t r = keep_rows; r-- > 0;) {
                for (uint32_t c = cols; c-- > 0;) {
                    cells[r * cols + c] = c < keep_cols ? cells[r * old_cols + c] : blank;
                }
            }
        }
        for (uint32_t i = (uint32_t)keep_rows * cols; i < (uint32_t)rows * cols; ++i) {
            cells[i] = blank;
        }
        if (tty->cursor_row >= rows) {
            tty->cursor_row = (uint16_t)(rows - 1u);
        }
        if (tty->cursor_col >= cols) {
            tty->cursor_col = (uint16_t)(cols - 1u);
        }
        tty->cursor_saved_valid = 0;
    }
    g_vt_cols = cols;
    g_vt_rows = rows;
}

static void vt_fb_set_cursor(const vt_tty_t* tty) {
    if (!tty || !tty->cursor_visible) {
        return;
    }
    (void)vt_fb_send(
        FBTEXT_IPC_CURSOR_SET_REQ, (int32_t)tty->cursor_col, (int32_t)tty->cursor_row, 0, 0);
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
    if ((tty_index != 0u) && vt_render_live(tty_index) && tty->cursor_visible) {
        vt_fb_set_cursor(tty);
    }
}

static void vt_apply_csi(uint32_t tty_index, vt_tty_t* tty, uint8_t final) {
    if (!tty) {
        return;
    }
    uint8_t render_now = (tty_index != 0) && vt_render_live(tty_index);
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
    (void)vt_fb_send(
        FBTEXT_IPC_CELL_WRITE_REQ, (int32_t)col, (int32_t)row, (int32_t)cell->ch, (int32_t)packed);
}

static int32_t vt_render_cell_switch(const vt_tty_t* tty, uint16_t row, uint16_t col) {
    if (!tty || !tty->cells || row >= g_vt_rows || col >= g_vt_cols) {
        return -1;
    }
    uint32_t idx = vt_cell_index(row, col);
    const vt_cell_t* cell = &tty->cells[idx];
    uint32_t packed = vt_pack_cell_colors(cell);
    return vt_fb_send_switch(
        FBTEXT_IPC_CELL_WRITE_REQ, (int32_t)col, (int32_t)row, (int32_t)cell->ch, (int32_t)packed);
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
    uint8_t render_now = vt_render_live(tty_index);

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

static void vt_serial_flush(void) {
    if (g_serial_out_len == 0) {
        return;
    }
    (void)wasmos_serial_owner_write(addr_cast(int32_t, g_serial_out), (int32_t)g_serial_out_len);
    g_serial_out_len = 0;
}

/* Sink of a slot's merged output stream (vt_outmux.h): every byte is rendered
 * into the slot, and the serial-bound slot's bytes are also queued for serial.
 * Serial is mirrored only once the console ring is registered, because until
 * then the kernel still transmits console output itself. */
static void vt_slot_sink(void* user, uint8_t byte) {
    uint32_t slot = (uint32_t)(uintptr_t)user;
    vt_process_byte(slot, &g_ttys[slot], byte);
    if (slot == g_serial_tty && g_klog_ring_ready) {
        g_serial_out[g_serial_out_len++] = byte;
        if (g_serial_out_len == (uint32_t)sizeof(g_serial_out)) {
            vt_serial_flush();
        }
    }
}

/* Writer output for a text slot: a console write of a process on that slot, or
 * the VT's own echo of typed input. */
static void vt_slot_write(uint32_t slot, const uint8_t* bytes, uint32_t len) {
    if (slot == 0 || slot >= VT_MAX_TTYS) {
        return;
    }
    vt_outmux_set_cols(&g_outmux[slot], g_vt_cols);
    vt_outmux_write(&g_outmux[slot], bytes, len, vt_slot_sink, (void*)(uintptr_t)slot);
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

/* A cell that matches the framebuffer's cleared state (a blank space on the
 * default background) does not need replaying after an FBTEXT_IPC_CLEAR_REQ.
 * Skipping these collapses the per-cell replay of a mostly-empty slot (e.g. a
 * CLI with a prompt) from rows*cols cell writes to just the non-blank ones;
 * replaying a full grid into the framebuffer's depth-limited queue otherwise
 * spins vt_fb_send_switch's per-cell retry for tens of seconds. */
static int vt_cell_is_blank(const vt_cell_t* cell) {
    return (cell->ch == (uint32_t)' ' || cell->ch == 0u) && cell->bg == 0u;
}

/* Repaint tty_index onto the framebuffer.  With the shared blit grid attached
 * this is a single FBTEXT_IPC_BLIT_GRID_REQ for the whole grid; otherwise it
 * falls back to per-cell writes that skip already-blank cells.  reliable=1 (the
 * tty-switch path) sends through vt_fb_send_switch, which retries up to
 * VT_FB_SWITCH_CELL_RETRIES and yields between rows; reliable=0 uses the plain
 * best-effort send.  Cells dropped under sustained backpressure are tolerated in
 * both paths.  Returns 0, or -1 for an invalid slot or an unallocated grid. */
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
        (void)vt_fb_send_switch(
            FBTEXT_IPC_BLIT_GRID_REQ, (int32_t)g_vt_cols, (int32_t)g_vt_rows, 0, 0);
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
        vt_outmux_init(&g_outmux[i]);
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
    g_active_tty = VT_KLOG_TTY;
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

/* FIXME: rapid Ctrl+Shift+Fn switching can intermittently render duplicated or
 * misaligned prompts (framebuffer only; the cell grid is correct).  No reliable
 * repro sequence exists yet, which is why the VT trace markers (switch and
 * writer-registration events) stay enabled. */
static int32_t vt_switch_tty(uint32_t tty_index) {
    if (tty_index >= VT_MAX_TTYS) {
        return WASMOS_ERR_VT_BAD_TTY_ID;
    }
    if (tty_index == g_active_tty) {
        return 0;
    }

    uint32_t prev_active = g_active_tty;
    g_switch_barrier = 1;

    /* Allow logical tty switching even when framebuffer control is unavailable
     * (startup races/headless mode). This keeps VT/CLI state consistent and
     * avoids reporting spurious switch failures to user-space. */
    if (g_fb_ep < 0) {
        g_switch_generation++;
        g_active_tty = tty_index;
        vt_trace_mark(VT_TRACE_SWITCH,
                      (uint16_t)(tty_index & 0x0FFFu),
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

    if (vt_fb_send_switch(FBTEXT_IPC_CLEAR_REQ, 0, 0, 0, 0) != 0) {
        g_switch_barrier = 0;
        return WASMOS_ERR_VT_SWITCH_CLEAR;
    }
    if (vt_replay_tty(tty_index, 1) != 0) {
        g_switch_barrier = 0;
        return WASMOS_ERR_VT_SWITCH_REPLAY;
    }
    g_switch_generation++;
    g_active_tty = tty_index;
    vt_trace_mark(VT_TRACE_SWITCH,
                  (uint16_t)(tty_index & 0x0FFFu),
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

/* The active layout, read from the filesystem once fs.vfs is first reported.
 * Until then, and for good if the read fails, the built-in US layout decodes, so
 * keyboard input always works. */
#define VT_KEYMAP_PATH "/boot/system/keymaps/de-nodeadkeys.kmap"
#define VT_KEYMAP_MAX 4096u

/* Keymap read state: the transfer buffer the filesystem writes the file into,
 * held from the request until its reply, and whether a read has been started
 * (the layout is loaded once; a restarted filesystem does not reload it). */
static int32_t g_keymap_bid = -1;
static int32_t g_keymap_req = 0; /* the read's intent id while it is in flight */
static uint8_t g_keymap_started = 0;

static void vt_keymap_read_done(void* user, const wasmos_ipc_message_t* msg) {
    static char km_buf[VT_KEYMAP_MAX];
    int32_t total = msg->type == FS_IPC_RESP ? msg->arg0 : -1;
    (void)user;
    if (total <= 0 || (uint32_t)total > sizeof(km_buf) ||
        wasmos_xfer_buffer_read(g_keymap_bid, km_buf, total, 0) != 0) {
        printf("[vt] keymap load failed (%s); using built-in US\n", VT_KEYMAP_PATH);
    } else {
        int rows = vt_parse_keymap(km_buf, (uint32_t)total);
        printf("[vt] keymap loaded: %s (%d rows)\n", VT_KEYMAP_PATH, rows);
    }
    (void)wasmos_xfer_buffer_release(g_keymap_bid);
    g_keymap_bid = -1;
    g_keymap_req = 0;
}

/* The filesystem ended with the read in flight: drop it, and read again from
 * the next filesystem the watch reports. */
static void vt_keymap_read_abandon(void) {
    if (g_keymap_bid < 0) {
        return;
    }
    wasmos_sys_intent_cancel(&g_loop, g_keymap_req);
    (void)wasmos_xfer_buffer_release(g_keymap_bid);
    g_keymap_bid = -1;
    g_keymap_req = 0;
    g_keymap_started = 0;
}

/* Start reading the layout file through the filesystem: the vt owns the buffer
 * (path at offset 0) and grants the filesystem read+write; the filesystem writes
 * the contents back and replies. Releasing the buffer on the reply revokes the
 * grant. */
static void vt_keymap_read_start(void) {
    static const char path[] = VT_KEYMAP_PATH;
    uint32_t plen = (uint32_t)sizeof(path) - 1u;
    int32_t grant;

    if (g_keymap_started || g_fs_ep < 0) {
        return;
    }
    g_keymap_started = 1;
    g_keymap_bid = wasmos_xfer_buffer_acquire((int32_t)VT_KEYMAP_MAX);
    if (g_keymap_bid < 0) {
        g_keymap_bid = -1;
        printf("[vt] keymap load failed (%s); using built-in US\n", VT_KEYMAP_PATH);
        return;
    }
    grant = wasmos_xfer_buffer_borrow(
        g_fs_ep, g_keymap_bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (wasmos_xfer_buffer_write(g_keymap_bid, path, (int32_t)plen, 0) != 0 || grant < 0 ||
        wasmos_sys_intent_send(&g_loop,
                               g_fs_ep,
                               g_vt_ep,
                               FS_IPC_READ_PATH_REQ,
                               (int32_t)plen,
                               0,
                               g_keymap_bid,
                               grant,
                               vt_keymap_read_done,
                               0,
                               &g_keymap_req) != 0) {
        (void)wasmos_xfer_buffer_release(g_keymap_bid);
        g_keymap_bid = -1;
        printf("[vt] keymap load failed (%s); using built-in US\n", VT_KEYMAP_PATH);
    }
}

/* Echo a typed byte into its slot (VT_INPUT_MODE_ECHO).  It is writer output of
 * that slot like any other, so it lands in the slot's grid whether or not the
 * slot is visible, and on serial when the slot is serial-bound. */
static void vt_input_echo_char(uint32_t tty_index, uint8_t ch) {
    vt_slot_write(tty_index, &ch, 1u);
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

/* Create the VT-owned console ring, map it into the VT's linear memory,
 * initialize the SPSC header, and hand its xfer-buffer id to the kernel, which
 * hands COM1 to the VT in the same step.  Best-effort: on any failure
 * g_klog_ring_ready stays 0, the kernel keeps transmitting all console output
 * itself, and the VT neither shows it nor mirrors any slot to serial. */
static void vt_klog_ring_init(void) {
    /* Overlay the SPSC ring on a BUFFER_KIND_TRANSFER xfer-buffer — the same
     * zero-copy transport the socket rings use (net.h), which handles WARP's
     * non-page-aligned linear-memory base for us.  The
     * VT's heap_pages/INITIAL_MEMORY (see linker.metadata) keep
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
    /* Register g_vt_ep as the doorbell: the kernel pushes VT_IPC_KLOG_NOTIFY when
     * it has published bytes, which wakes the select below so an idle VT drains
     * klog instead of sitting on it until unrelated traffic arrives. */
    if (wasmos_klog_register_ring(bid, g_vt_ep) != 0) {
        return;
    }
    g_klog_ring_ready = 1;
}

/* Drain every pending console record: kernel log into VT_KLOG_TTY, a process's
 * console output into its controlling slot, both through that slot's merge
 * (vt_outmux.h).  Kernel log never targets vt-0, whose byte path calls
 * wasmos_console_write and would loop back into the ring; a record naming a slot
 * the VT does not have is shown as kernel log rather than dropped.
 *
 * Rendering is deferred for the whole drain and the visible slot, if it received
 * anything, is closed with one full-grid blit.  Per-cell rendering would issue
 * one framebuffer IPC per character, and the ring delivers bursts of whole lines
 * -- thousands of cells during boot -- which is the same queue-flooding shape
 * that wedged tty switches before the blit path existed.  The grid is
 * authoritative, so a batch repaint loses nothing. */
static void vt_drain_klog_ring(void) {
    if (!g_klog_ring_ready) {
        return;
    }
    uint8_t record[1u + VT_CONSOLE_RECORD_TEXT_MAX];
    uint32_t touched = 0;
    g_render_defer = 1;
    for (;;) {
        uint32_t len = 0;
        int32_t n =
            wasmos_ringbuf_read_record(&g_klog_ring, record, (uint32_t)sizeof(record), &len);
        if (n == -2) {
            /* Longer than any record the kernel writes: skip it whole. */
            (void)wasmos_ringbuf_skip(&g_klog_ring, 4u + len);
            continue;
        }
        if (n <= 0) {
            if (n < 0) {
                break;
            }
            continue;
        }
        uint32_t slot = record[0];
        const uint8_t* text = &record[1];
        uint32_t text_len = (uint32_t)n - 1u;
        if (slot == VT_CONSOLE_SLOT_KLOG || slot >= VT_MAX_TTYS) {
            vt_outmux_set_cols(&g_outmux[VT_KLOG_TTY], g_vt_cols);
            vt_outmux_log(&g_outmux[VT_KLOG_TTY],
                          text,
                          text_len,
                          vt_slot_sink,
                          (void*)(uintptr_t)VT_KLOG_TTY);
            touched |= 1u << VT_KLOG_TTY;
        } else {
            vt_slot_write(slot, text, text_len);
            touched |= 1u << slot;
        }
    }
    g_render_defer = 0;
    if ((touched & (1u << g_active_tty)) != 0 && !g_switch_barrier) {
        (void)vt_replay_tty(g_active_tty, 0);
    }
}

/* Set up the shared cell-grid blit buffer: acquire an xfer-buffer, map it into
 * the VT's linear memory (write side), grant the framebuffer driver READ
 * access, and hand it the buffer/borrow ids so it can map and render from it.
 * Best-effort: on failure g_blit_ready stays 0 and vt_replay_tty falls back to
 * per-cell writes. */
static void vt_blit_release(void) {
    g_blit_ready = 0;
    g_blit_grid = 0;
    if (g_blit_bid >= 0) {
        /* Releasing revokes the grant the previous framebuffer held. */
        (void)wasmos_xfer_buffer_release(g_blit_bid);
        g_blit_bid = -1;
    }
}

static void vt_blit_init(void) {
    vt_blit_release();
    if (g_fb_ep < 0) {
        return;
    }
    uint32_t bytes = VT_BLIT_MAX_CELLS * (uint32_t)sizeof(fbtext_blit_cell_t);
    g_blit_bid = wasmos_xfer_buffer_acquire((int32_t)bytes);
    if (g_blit_bid <= 0) {
        g_blit_bid = -1;
        return;
    }
    int32_t off = wasmos_xfer_buffer_map(g_blit_bid);
    int32_t grant =
        off < 0 ? -1 : wasmos_xfer_buffer_borrow(g_fb_ep, g_blit_bid, WASMOS_BUFFER_GRANT_READ);
    /* Hand the driver the ids to map; it renders on FBTEXT_IPC_BLIT_GRID_REQ. */
    if (grant < 0 || vt_fb_send_switch(FBTEXT_IPC_BLIT_ATTACH_REQ,
                                       g_blit_bid,
                                       grant,
                                       (int32_t)g_vt_cols,
                                       (int32_t)g_vt_rows) != 0) {
        vt_blit_release();
        return;
    }
    g_blit_grid = ptr_cast(fbtext_blit_cell_t, (uint32_t)off);
    g_blit_ready = 1;
}

/* Path of the shell the vt spawns for a slot.  One CLI per slot, created the
 * first time that slot is put to use: a slot with no shell has nothing that
 * reads its input queue.
 *
 * The console slot's shell is NOT spawned here — sysinit starts it, last, after
 * the rest of the system is up.  The first prompt is what the test framework and
 * a human both read as "the machine is ready", and spawning it from the vt (which
 * comes up early) would put that prompt in the middle of boot and invite every
 * caller to start driving a half-started system. */
#define VT_CLI_PATH "/boot/system/services/cli.wap"

static int32_t g_proc_ep = -1;
static uint8_t g_tty_cli_spawned[VT_MAX_TTYS] = {0};
/* Transfer buffer holding the path of an in-flight spawn request.  PM reads the
 * path out of it while handling the request, so it is released only once the
 * reply lands.  One spawn at a time: they are triggered by user-visible events
 * (a switch, a serial rebind) and never arrive in bursts. */
static int32_t g_spawn_path_bid = -1;

/* Release the path buffer once PM answers the spawn that named it; the reply is
 * matched to the request by the loop's intent, so no other PROC_IPC_RESP can
 * release a buffer PM may still be reading. */
static void vt_spawn_reply_done(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type == PROC_IPC_ERROR) {
        printf("[vt] cli spawn failed: %d\n", (int)msg->arg1);
    }
    if (g_spawn_path_bid >= 0) {
        (void)wasmos_xfer_buffer_release(g_spawn_path_bid);
        g_spawn_path_bid = -1;
    }
}

/* Ask process-manager for a CLI pinned to `slot`.  Fire-and-forget with DETACH:
 * the vt must keep serving IPC while the child comes up, and a ready-gated spawn
 * would park the vt inside the request the new CLI needs answered.  The tty is
 * pinned rather than left to PM's round-robin, which would put the shell on some
 * other slot's queue where nothing reads it.  Best-effort: on any failure the
 * slot simply stays without a shell, and the next switch to it tries again. */
static void vt_spawn_cli_for_slot(uint32_t slot) {
    if (slot == 0u || slot >= VT_MAX_TTYS || g_tty_cli_spawned[slot] || g_proc_ep < 0) {
        return;
    }
    if (g_tty_writer_ep[slot] >= 0) {
        return; /* a shell already owns this slot (sysinit's, or an earlier spawn) */
    }
    if (g_spawn_path_bid >= 0) {
        return; /* one in flight; the next switch to this slot retries */
    }
    static const char path[] = VT_CLI_PATH;
    uint32_t path_len = (uint32_t)sizeof(path) - 1u;
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)path_len + 1);
    if (bid <= 0) {
        return;
    }
    if (wasmos_xfer_buffer_write(bid, path, (int32_t)path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return;
    }
    /* Mark before sending: a failed send leaves the slot marked only until the
     * release below clears it, and a double spawn would give one slot two shells
     * competing for its input. */
    g_tty_cli_spawned[slot] = 1;
    g_spawn_path_bid = bid;
    if (wasmos_sys_intent_send(&g_loop,
                               g_proc_ep,
                               g_vt_ep,
                               PROC_IPC_SPAWN_PATH,
                               (int32_t)(PROC_SPAWN_PATH_FLAG_DETACH | PROC_SPAWN_PATH_TTY(slot)),
                               (int32_t)(((uint32_t)bid << 12) | (path_len & 0xFFFu)),
                               0,
                               0,
                               vt_spawn_reply_done,
                               0,
                               0) != 0) {
        g_tty_cli_spawned[slot] = 0;
        g_spawn_path_bid = -1;
        (void)wasmos_xfer_buffer_release(bid);
    }
}

/* ------------------------------------------------------------------ peers
 *
 * The four services the vt uses are followed by service-name watches, not looked
 * up once: each is used from the moment it registers, and again when a
 * restarted one registers anew, whatever order the system came up in. A peer's
 * WASMOS_IPC_HANGUP drops it (vt_on_peer_gone) until its watch reports the
 * next one. */

/* Repaint the visible text slot on a framebuffer that has just been attached or
 * re-laid out. vt-0 belongs to the compositor and is never repainted here. */
static void vt_fb_repaint(void) {
    if (g_fb_ep < 0 || g_active_tty == 0u) {
        return;
    }
    if (vt_fb_send_switch(FBTEXT_IPC_CLEAR_REQ, 0, 0, 0, 0) == 0) {
        (void)vt_replay_tty(g_active_tty, 1);
    }
}

/* The framebuffer's geometry reply: re-lay the grids out for it (keeping the
 * current geometry when the query failed or reports one out of range), attach
 * the blit grid, repaint. */
static int32_t g_geometry_req = 0; /* the query's intent id while it is in flight */

static void vt_on_geometry(void* user, const wasmos_ipc_message_t* msg) {
    uint16_t cols = g_vt_cols;
    uint16_t rows = g_vt_rows;
    (void)user;
    g_geometry_req = 0;
    if (msg->type == FBTEXT_IPC_RESP) {
        if (msg->arg0 >= 40 && msg->arg0 <= (int32_t)VT_MAX_COLS) {
            cols = (uint16_t)msg->arg0;
        }
        if (msg->arg1 >= 16 && msg->arg1 <= (int32_t)VT_MAX_ROWS) {
            rows = (uint16_t)msg->arg1;
        }
    }
    vt_regrid(cols, rows);
    vt_blit_init();
    vt_fb_repaint();
}

static void vt_on_fb_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    vt_blit_release();
    g_fb_ep = endpoint;
    if (wasmos_sys_intent_send(&g_loop,
                               g_fb_ep,
                               g_vt_ep,
                               FBTEXT_IPC_GEOMETRY_REQ,
                               0,
                               0,
                               0,
                               0,
                               vt_on_geometry,
                               0,
                               &g_geometry_req) != 0) {
        vt_blit_init();
        vt_fb_repaint();
    }
}

static void vt_on_kbd_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    g_kbd_ep = endpoint;
    (void)wasmos_ipc_send(g_kbd_ep, g_vt_ep, KBD_IPC_SUBSCRIBE_REQ, 1, 0, 0, 0, 0);
}

/* COM1 RX arrives from the serial driver as VT_IPC_SERIAL_INPUT_REQ pushes into
 * the serial-bound slot once the vt has subscribed. */
static void vt_on_sin_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    g_serial_in_ep = endpoint;
    (void)wasmos_ipc_send(g_serial_in_ep, g_vt_ep, SERIAL_IPC_SUBSCRIBE_REQ, 1, 0, 0, 0, 0);
}

static void vt_on_fs_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    g_fs_ep = endpoint;
    vt_keymap_read_start();
}

/* A watch ends on its own only when the process manager refuses a request; the
 * vt then runs without that peer, as it does before the peer registers. */
static void vt_on_watch_failed(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    printf("[vt] watching %s failed: %d\n", watch->name, (int)status);
}

static void vt_start_watch(wasmos_sys_svc_watch_t* watch, const char* name,
                           wasmos_sys_svc_bind_fn on_bind) {
    int32_t rc =
        wasmos_sys_svc_watch_start(&g_watches, watch, name, 0, on_bind, vt_on_watch_failed, 0);
    if (rc != 0) {
        printf("[vt] cannot watch %s: %d\n", name, (int)rc);
    }
}

/* A peer's process ended. Forget its endpoint, so the vt stops addressing it and
 * the watch reports a restarted provider even when it gets the same endpoint
 * id. */
static void vt_on_peer_gone(int32_t endpoint) {
    if (endpoint < 0) {
        return;
    }
    if (endpoint == g_fb_ep) {
        g_fb_ep = -1;
        if (g_geometry_req > 0) {
            wasmos_sys_intent_cancel(&g_loop, g_geometry_req);
            g_geometry_req = 0;
        }
        vt_blit_release();
        wasmos_sys_svc_watch_forget(&g_watch_fb);
    }
    if (endpoint == g_kbd_ep) {
        g_kbd_ep = -1;
        wasmos_sys_svc_watch_forget(&g_watch_kbd);
    }
    if (endpoint == g_serial_in_ep) {
        g_serial_in_ep = -1;
        wasmos_sys_svc_watch_forget(&g_watch_sin);
    }
    if (endpoint == g_fs_ep) {
        g_fs_ep = -1;
        vt_keymap_read_abandon();
        wasmos_sys_svc_watch_forget(&g_watch_fs);
    }
}

/* ---------------------------------------------------------- registration */

static int32_t g_register_bid = -1;

static void vt_on_registered(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (g_register_bid >= 0) {
        (void)wasmos_xfer_buffer_release(g_register_bid);
        g_register_bid = -1;
    }
    if (msg->type != SVC_IPC_REGISTER_RESP) {
        printf("[vt] registering \"vt\" failed: %d/%d\n", (int)msg->arg0, (int)msg->arg1);
    }
}

/* Register g_vt_ep as "vt". The descriptor travels in a buffer the vt owns,
 * released on the reply, which the loop matches by request id on the service
 * endpoint itself. */
static int vt_register(int32_t proc_endpoint) {
    svc_register_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_SVC_REGISTER_DESC_VERSION;
    desc.service_endpoint = (uint32_t)g_vt_ep;
    memcpy(desc.name, "vt", 3);
    g_register_bid = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (g_register_bid < 0) {
        g_register_bid = -1;
        return -1;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               proc_endpoint,
                               g_vt_ep,
                               SVC_IPC_REGISTER_DESC_REQ,
                               0,
                               (int32_t)sizeof(desc),
                               g_register_bid,
                               0,
                               vt_on_registered,
                               0,
                               0) != 0) {
        (void)wasmos_xfer_buffer_release(g_register_bid);
        g_register_bid = -1;
        return -1;
    }
    return 0;
}

/* Consumes the process manager's acknowledgement of NOTIFY_READY. */
static void vt_on_ready_ack(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

/* ------------------------------------------------------- client traffic */

/* Every message no intent or watch claims: client requests and notifications.
 * The console ring is drained first, so kernel output that preceded a message is
 * rendered before whatever the message produces. */
static void vt_on_message(void* user, const wasmos_ipc_message_t* message) {
    wasmos_ipc_message_t msg = *message;
    (void)user;
    vt_drain_klog_ring();
    switch ((uint32_t)msg.type) {
    case VT_IPC_SERIAL_INPUT_REQ: {
        /* COM1 RX from the serial driver -> the serial-bound slot's input
         * queue (line discipline), regardless of which slot is visible.
         * arg0[27:24] = byte count (1-4), argN[7:0] = the bytes. */
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
                (void)vt_ipc_reply_retry(
                    msg.source, VT_IPC_ERROR, msg.request_id, WASMOS_ERR_VT_NO_TTY_FOR_SOURCE, 0);
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
        if (sw == 0 && msg.arg0 > 0) {
            /* A text slot the user just made visible needs a shell to type
             * into; the first switch to it is what creates one. */
            vt_spawn_cli_for_slot((uint32_t)msg.arg0);
        }
        if (msg.source >= 0 && msg.request_id != 0) {
            (void)vt_ipc_reply_retry(msg.source,
                                     (sw == 0) ? VT_IPC_RESP : VT_IPC_ERROR,
                                     msg.request_id,
                                     (sw == 0) ? (int32_t)g_switch_generation : sw,
                                     (int32_t)g_active_tty);
        }
        break;
    }

    case VT_IPC_BIND_SERIAL_REQ: {
        /* The serial-bound slot is independent of the visible one: serial RX
         * reaches this slot no matter what is on screen (see doc 19, "two
         * independent selectors"). */
        int32_t tty_id = msg.arg0;
        int32_t rc = 0;
        if (tty_id < 0 || tty_id >= (int32_t)VT_MAX_TTYS) {
            rc = WASMOS_ERR_VT_BAD_TTY_ID;
        } else if (tty_id == 0) {
            /* vt-0 is the compositor's; a serial line cannot render it. */
            rc = WASMOS_ERR_VT_SERIAL_SLOT_GUI;
        } else {
            g_serial_tty = (uint32_t)tty_id;
            /* Same reason as a switch: a serial line bound to a slot with no
             * shell has nothing to answer it. */
            vt_spawn_cli_for_slot(g_serial_tty);
        }
        if (msg.source >= 0 && msg.request_id != 0) {
            (void)vt_ipc_reply_retry(msg.source,
                                     (rc == 0) ? VT_IPC_RESP : VT_IPC_ERROR,
                                     msg.request_id,
                                     (rc == 0) ? (int32_t)g_serial_tty : rc,
                                     0);
        }
        break;
    }

    case VT_IPC_GET_ACTIVE_TTY:
        if (msg.source >= 0 && msg.request_id != 0) {
            (void)vt_ipc_reply_retry(msg.source,
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
            (void)vt_ipc_reply_retry(
                msg.source, VT_IPC_ERROR, msg.request_id, WASMOS_ERR_VT_BAD_TTY_ID, 0);
            break;
        }
        uint32_t idx = (uint32_t)tty_id;
        if (g_tty_writer_ep[idx] >= 0 && g_tty_writer_ep[idx] != msg.source) {
            vt_trace_mark(VT_TRACE_WRITER_CONFLICT,
                          (uint16_t)(idx & 0x0FFFu),
                          (uint16_t)((uint32_t)msg.source & 0x0FFFu));
            /* Replace stale/previous writer ownership instead of rejecting
             * new registrations. This keeps CLI recovery robust when a
             * prior writer process exited without an explicit unregister. */
        }
        g_tty_writer_ep[idx] = msg.source;
        vt_trace_mark(VT_TRACE_WRITER_OK,
                      (uint16_t)(idx & 0x0FFFu),
                      (uint16_t)((uint32_t)msg.source & 0x0FFFu));
        (void)vt_ipc_reply_retry(
            msg.source, VT_IPC_RESP, msg.request_id, (int32_t)g_switch_generation, tty_id);
        break;
    }

    case VT_IPC_READ_REQ: {
        if (msg.source < 0 || msg.request_id == 0) {
            break;
        }
        int32_t tty_id = msg.arg0;
        if (tty_id < 0 || tty_id >= (int32_t)VT_MAX_TTYS) {
            (void)vt_ipc_reply_retry(
                msg.source, VT_IPC_ERROR, msg.request_id, WASMOS_ERR_VT_BAD_TTY_ID, 0);
            break;
        }
        if (g_tty_reader_ep[(uint32_t)tty_id] < 0) {
            g_tty_reader_ep[(uint32_t)tty_id] = msg.source;
        } else if (g_tty_reader_ep[(uint32_t)tty_id] != msg.source) {
            (void)vt_ipc_reply_retry(
                msg.source, VT_IPC_ERROR, msg.request_id, WASMOS_ERR_VT_READER_BUSY, 0);
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
            (void)vt_ipc_reply_retry(
                msg.source, VT_IPC_ERROR, msg.request_id, WASMOS_ERR_VT_NO_TTY_FOR_SOURCE, 0);
            break;
        }
        uint8_t mode = (uint8_t)(msg.arg0 & (VT_INPUT_MODE_CANONICAL | VT_INPUT_MODE_ECHO));
        vt_set_input_mode(&g_ttys[(uint32_t)tty_index], mode);
        (void)vt_ipc_reply_retry(msg.source, VT_IPC_RESP, msg.request_id, (int32_t)mode, tty_index);
        break;
    }

    case VT_IPC_KLOG_NOTIFY:
        /* The kernel's console-ring doorbell.  Waking the loop is its entire
         * purpose -- the ring was drained above -- so there is nothing to do
         * here and nothing to reply to. */
        break;

    case KBD_IPC_KEY_NOTIFY:
        vt_handle_key_notify(msg.arg0, msg.arg1, msg.arg2);
        break;

    case KBD_IPC_SUBSCRIBE_RESP:
        break;

    case WASMOS_IPC_HANGUP:
        /* Sent by the kernel only; arg0 names the peer endpoint whose
         * process ended. */
        vt_on_peer_gone(msg.arg0);
        break;

    default:
        if (msg.source >= 0 && msg.request_id != 0) {
            (void)vt_ipc_reply_retry(
                msg.source, VT_IPC_ERROR, msg.request_id, WASMOS_ERR_VT_UNSUPPORTED_REQUEST, 0);
        }
        break;
    }
}

/* Service entry point.  Allocates the per-slot cell grids, registers "vt",
 * starts watching its peers, attaches the klog ring, signals readiness, then
 * serves IPC forever from the event loop.  Never returns except on an init
 * failure (-1).
 * g_switch_generation is a monotonic counter bumped on every TTY switch; writes
 * whose request_id carries a stale generation are dropped. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* The proc endpoint comes from the spawn-info contract; the entry args are
     * unused and the parameter is overwritten. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();

    g_vt_ep = wasmos_ipc_create_endpoint();
    if (g_vt_ep < 0) {
        return -1;
    }
    wasmos_sys_event_loop_init(&g_loop, g_vt_ep, VT_LOOP_REQ_BASE);
    (void)wasmos_sys_event_set_default(&g_loop, vt_on_message, 0);

    /* Grids first, at the default geometry: output can arrive before the
     * framebuffer reports its own. */
    vt_reset_tty_cells();
    vt_heap_init();
    if (vt_alloc_tty_cells() != 0) {
        vt_log_alloc_failure("grid", g_alloc_failure);
        return -1;
    }
    vt_init_ttys();

    if (vt_register(proc_endpoint) != 0) {
        printf("[vt] cannot register \"vt\"\n");
        return -1;
    }
    if (wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, proc_endpoint) != 0) {
        printf("[vt] cannot set up service watches\n");
        return -1;
    }
    vt_start_watch(&g_watch_fb, "fb", vt_on_fb_bound);
    vt_start_watch(&g_watch_kbd, "kbd", vt_on_kbd_bound);
    vt_start_watch(&g_watch_sin, "sin", vt_on_sin_bound);
    vt_start_watch(&g_watch_fs, "fs.vfs", vt_on_fs_bound);

    /* Bring up the VT-owned console ring (small, cheap overlay — see
     * vt_klog_ring_init).  The loop below drains it after every wake, and the
     * kernel's VT_IPC_KLOG_NOTIFY doorbell is itself such a wake, so pending
     * output reaches its slot and serial without polling. */
    vt_klog_ring_init();

    /* Sent after the registration: the process manager serves one sender in
     * order, so "vt" is registered by the time anyone sees the vt ready. */
    (void)wasmos_sys_intent_send(
        &g_loop, proc_endpoint, g_vt_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, vt_on_ready_ack, 0, 0);

    /* Kept for the lazy per-slot shells; the console slot's shell comes from
     * sysinit. */
    g_proc_ep = proc_endpoint;

    /* One message per poll, so echo produced while handling it, and whatever the
     * drain left under a full buffer, reaches serial before the VT parks again.
     * The poll parks untimed: a timed select set strands serial input under
     * WARP. */
    for (;;) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 1);
        vt_drain_klog_ring();
        vt_serial_flush();
    }
}
