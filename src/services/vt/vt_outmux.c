#include "vt_outmux.h"

void vt_outmux_init(vt_outmux_t* mux) {
    mux->open_len = 0;
    mux->open_overflow = 0;
    mux->log_len = 0;
}

void vt_outmux_write(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                     void* user) {
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t b = bytes[i];
        sink(user, b);
        if (b == '\n') {
            mux->open_len = 0;
            mux->open_overflow = 0;
        } else if (mux->open_len < VT_OUTMUX_LINE_MAX) {
            mux->open[mux->open_len++] = b;
        } else {
            mux->open_overflow = 1;
        }
    }
}

/* Emits the buffered log line, terminated, around whatever writer line is open. */
static void vt_outmux_flush_log_line(vt_outmux_t* mux, vt_outmux_sink_fn sink, void* user) {
    uint8_t repaint = 0;
    if (mux->open_overflow) {
        /* Too long to replay: leave it standing and continue below the log. */
        sink(user, '\n');
        mux->open_len = 0;
        mux->open_overflow = 0;
    } else if (mux->open_len != 0) {
        sink(user, '\r');
        sink(user, 0x1B);
        sink(user, '[');
        sink(user, 'K');
        repaint = 1;
    }
    for (uint16_t i = 0; i < mux->log_len; ++i) {
        sink(user, mux->log[i]);
    }
    sink(user, '\n');
    mux->log_len = 0;
    if (repaint) {
        for (uint16_t i = 0; i < mux->open_len; ++i) {
            sink(user, mux->open[i]);
        }
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
