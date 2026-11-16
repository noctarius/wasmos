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
 * erases that line ("\r" + CSI K), prints the log line, and repaints the open
 * line, so the writer's text stays contiguous on screen and on serial.
 *
 * No IPC, no globals: the caller supplies the state and a byte sink. */

#define VT_OUTMUX_LINE_MAX 256u

typedef void (*vt_outmux_sink_fn)(void* user, uint8_t byte);

typedef struct {
    /* Writer bytes since the last '\n', replayed after a log line. */
    uint8_t open[VT_OUTMUX_LINE_MAX];
    uint16_t open_len;
    /* The open line outgrew `open`: it cannot be repainted, so a log line
     * arriving now starts on a fresh line and the writer continues below it. */
    uint8_t open_overflow;
    /* Log bytes not yet terminated by '\n'.  A log line is emitted only when
     * complete, or when it fills this buffer. */
    uint8_t log[VT_OUTMUX_LINE_MAX];
    uint16_t log_len;
} vt_outmux_t;

void vt_outmux_init(vt_outmux_t* mux);

/* Writer output: passed to the sink in order, and remembered as the open line
 * until a '\n'. */
void vt_outmux_write(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                     void* user);

/* Kernel-log output: buffered to line granularity and emitted as described
 * above.  An unterminated tail stays buffered until its '\n' arrives. */
void vt_outmux_log(vt_outmux_t* mux, const uint8_t* bytes, uint32_t len, vt_outmux_sink_fn sink,
                   void* user);

#endif
