#ifndef WASMOS_VT_OUTMUX_H
#define WASMOS_VT_OUTMUX_H

#include <stdint.h>

/* Output merge for one text slot (docs/architecture/19-virtual-terminal.md,
 * "Output routing").
 *
 * A slot carries two sources: its writers (the shell, the apps it runs, the
 * shell's own echo of typed keys) and, on the system-console slot, the kernel
 * log.  Writers produce partial lines -- a prompt waits for input, an echo
 * arrives one keystroke at a time -- while the log produces whole lines at
 * arbitrary moments.  The merge emits each log line whole and never inside a
 * writer's open line: a log line that arrives while a writer line is open
 * erases that line, prints the log line, and repaints the open line with the
 * cursor where the writer left it, so the writer's text stays contiguous on
 * screen and on serial.
 *
 * The open line is kept as the vt renders it -- one byte per column, plus the
 * cursor column -- not as the bytes that produced it: "\r" and "\b" move the
 * cursor, and the vt's "\b" blanks the cell it moves onto, so a shell's
 * "\b \b" correction leaves the line as short as it looks.  The line starts at
 * column 0 (it follows a '\n') and wraps at the terminal width, which is what
 * lets the erase start on the line's first row (CSI A, then CSI J) rather than
 * on the cursor's row only.
 *
 * A line that cannot be modeled by column is not repainted: one holding an
 * escape sequence or another control byte, or one longer than the buffer.  A
 * log line arriving then starts on a fresh line and the writer continues below
 * it.
 *
 * No IPC, no globals: the caller supplies the state and a byte sink. */

#define VT_OUTMUX_LINE_MAX 256u

typedef void (*vt_outmux_sink_fn)(void* user, uint8_t byte);

typedef struct {
    /* The open line as rendered: one byte per column since the last '\n'. */
    uint8_t open[VT_OUTMUX_LINE_MAX];
    uint16_t open_len;    /* columns written */
    uint16_t open_cursor; /* column the writer's next byte lands in */
    /* The open line cannot be modeled by column (see above), so a log line
     * arriving now starts on a fresh line and the writer continues below it. */
    uint8_t open_overflow;
    /* Terminal width the open line wraps at; 0 when unknown (no wrap). */
    uint16_t cols;
    /* Log bytes not yet terminated by '\n'.  A log line is emitted only when
     * complete, or when it fills this buffer. */
    uint8_t log[VT_OUTMUX_LINE_MAX];
    uint16_t log_len;
} vt_outmux_t;

void vt_outmux_init(vt_outmux_t* mux);

/* Terminal width in columns, for an open line that wraps; 0 means unknown. */
void vt_outmux_set_cols(vt_outmux_t* mux, uint16_t cols);

/* Writer output: passed to the sink in order, and tracked as the open line
 * until a '\n'. */
void vt_outmux_write(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                     void* user);

/* Kernel-log output: buffered to line granularity and emitted as described
 * above.  An unterminated tail stays buffered until its '\n' arrives. */
void vt_outmux_log(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                   void* user);

#endif
