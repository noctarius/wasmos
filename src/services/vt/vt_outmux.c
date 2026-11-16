#include "vt_outmux.h"

static void vt_outmux_reset_open(vt_outmux_t* mux) {
    mux->open_len = 0;
    mux->open_cursor = 0;
    mux->open_overflow = 0;
}

void vt_outmux_init(vt_outmux_t* mux) {
    vt_outmux_reset_open(mux);
    mux->cols = 0;
    mux->log_len = 0;
}

void vt_outmux_set_cols(vt_outmux_t* mux, uint16_t cols) {
    mux->cols = cols;
}

/* Applies one writer byte to the open line the way the vt renders it. */
static void vt_outmux_track(vt_outmux_t* mux, uint8_t b) {
    if (b == '\n') {
        vt_outmux_reset_open(mux);
        return;
    }
    if (mux->open_overflow) {
        return;
    }
    if (b == '\r') {
        mux->open_cursor = 0;
    } else if (b == '\b') {
        if (mux->open_cursor > 0) {
            mux->open_cursor--;
        }
        if (mux->open_cursor < mux->open_len) {
            mux->open[mux->open_cursor] = ' ';
        }
    } else if (b < 0x20u || b == 0x7Fu || mux->open_cursor >= VT_OUTMUX_LINE_MAX) {
        mux->open_overflow = 1;
    } else {
        mux->open[mux->open_cursor++] = b;
        if (mux->open_cursor > mux->open_len) {
            mux->open_len = mux->open_cursor;
        }
    }
}

void vt_outmux_write(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                     void* user) {
    for (uint32_t i = 0; i < len; ++i) {
        sink(user, bytes[i]);
        vt_outmux_track(mux, bytes[i]);
    }
}

/* Emits ESC [ n <final>, with no parameter digits for n == 0 (the default). */
static void vt_outmux_csi(vt_outmux_sink_fn sink, void* user, uint16_t n, uint8_t final) {
    char digits[5];
    int count = 0;
    while (n != 0) {
        digits[count++] = (char)('0' + n % 10u);
        n = (uint16_t)(n / 10u);
    }
    sink(user, 0x1B);
    sink(user, '[');
    while (count > 0) {
        sink(user, (uint8_t)digits[--count]);
    }
    sink(user, final);
}

/* Row of column `col` of the open line, which starts at column 0 and wraps
 * immediately on reaching the terminal width, as the vt does. */
static uint16_t vt_outmux_row(const vt_outmux_t* mux, uint16_t col) {
    return mux->cols ? (uint16_t)(col / mux->cols) : 0;
}

static uint16_t vt_outmux_col(const vt_outmux_t* mux, uint16_t col) {
    return mux->cols ? (uint16_t)(col % mux->cols) : col;
}

/* Moves the cursor back to the start of the open line and clears from there:
 * CSI K for a line on one row, CSI A to its first row and CSI J for a wrapped
 * one. */
static void vt_outmux_erase_open(vt_outmux_t* mux, vt_outmux_sink_fn sink, void* user) {
    uint16_t up = vt_outmux_row(mux, mux->open_cursor);
    uint16_t last = vt_outmux_row(mux, mux->open_len);
    sink(user, '\r');
    if (up == 0 && last == 0) {
        vt_outmux_csi(sink, user, 0, 'K');
        return;
    }
    if (up != 0) {
        vt_outmux_csi(sink, user, up, 'A');
    }
    vt_outmux_csi(sink, user, 0, 'J');
}

/* Prints the open line and puts the cursor back on the writer's column.  Blanks
 * past the cursor are not printed: the erase already cleared them. */
static void vt_outmux_repaint_open(vt_outmux_t* mux, vt_outmux_sink_fn sink, void* user) {
    uint16_t end = mux->open_len;
    while (end > mux->open_cursor && mux->open[end - 1u] == ' ') {
        end--;
    }
    for (uint16_t i = 0; i < end; ++i) {
        sink(user, mux->open[i]);
    }
    if (end == mux->open_cursor) {
        return;
    }
    uint16_t up = (uint16_t)(vt_outmux_row(mux, end) - vt_outmux_row(mux, mux->open_cursor));
    if (up == 0) {
        vt_outmux_csi(sink,
                      user,
                      (uint16_t)(vt_outmux_col(mux, end) - vt_outmux_col(mux, mux->open_cursor)),
                      'D');
        return;
    }
    sink(user, '\r');
    vt_outmux_csi(sink, user, up, 'A');
    if (vt_outmux_col(mux, mux->open_cursor) != 0) {
        vt_outmux_csi(sink, user, vt_outmux_col(mux, mux->open_cursor), 'C');
    }
}

/* Emits the buffered log line, terminated, around whatever writer line is open. */
static void vt_outmux_flush_log_line(vt_outmux_t* mux, vt_outmux_sink_fn sink, void* user) {
    uint8_t repaint = 0;
    if (mux->open_overflow) {
        /* Not modeled by column: leave it standing and continue below the log. */
        sink(user, '\n');
        vt_outmux_reset_open(mux);
    } else if (mux->open_len != 0 || mux->open_cursor != 0) {
        vt_outmux_erase_open(mux, sink, user);
        repaint = 1;
    }
    for (uint16_t i = 0; i < mux->log_len; ++i) {
        sink(user, mux->log[i]);
    }
    sink(user, '\n');
    mux->log_len = 0;
    if (repaint) {
        vt_outmux_repaint_open(mux, sink, user);
    }
}

void vt_outmux_log(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                   void* user) {
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t b = bytes[i];
        if (b == '\n') {
            vt_outmux_flush_log_line(mux, sink, user);
            continue;
        }
        mux->log[mux->log_len++] = b;
        if (mux->log_len == VT_OUTMUX_LINE_MAX) {
            vt_outmux_flush_log_line(mux, sink, user);
        }
    }
}
