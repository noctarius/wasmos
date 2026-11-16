/* test_vt_outmux.c — how the vt merges a slot's writer output with the kernel
 * log (src/services/vt/vt_outmux.c).
 *
 * Regression: 2026-11-15-split-echo -- the shell echoes a command one keystroke
 * at a time while services log whole lines, and both reached COM1 through the
 * kernel log in arrival order. A late service line landed inside an echo
 * ("/ wamos> fs_leave_open /[net-stack] tls: no CA trust store\nleakfds/held
 * 8"), so the typed command never appeared contiguously and a QEMU test timed
 * out against a healthy guest (CI run 43608707742). The same split garbles the
 * line a user is typing on screen.
 *
 * Only vt_outmux.c is linked: it touches no IPC, no framebuffer and no global
 * state, which is why it is a separate translation unit.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vt_outmux.h"

static int g_failures;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, (what));                              \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    char buf[2048];
    uint32_t len;
} sink_t;

static void sink_byte(void* user, uint8_t byte) {
    sink_t* s = (sink_t*)user;
    if (s->len + 1u < sizeof(s->buf)) {
        s->buf[s->len++] = (char)byte;
        s->buf[s->len] = '\0';
    }
}

static void writer(vt_outmux_t* mux, sink_t* s, const char* text) {
    vt_outmux_write(mux, (const uint8_t*)text, (uint32_t)strlen(text), sink_byte, s);
}

static void klog(vt_outmux_t* mux, sink_t* s, const char* text) {
    vt_outmux_log(mux, (const uint8_t*)text, (uint32_t)strlen(text), sink_byte, s);
}

/* Each keystroke is its own write, as the shell's echo is. */
static void type(vt_outmux_t* mux, sink_t* s, const char* keys) {
    for (const char* k = keys; *k; ++k) {
        vt_outmux_write(mux, (const uint8_t*)k, 1u, sink_byte, s);
    }
}

static void test_a_log_line_does_not_split_a_typed_command(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    writer(&mux, &s, "/ wamos> ");
    type(&mux, &s, "fs_leave_open /");
    klog(&mux, &s, "[net-stack] tls: no CA trust store\n");
    type(&mux, &s, "leakfds/held 8\n");

    CHECK(strstr(s.buf, "/ wamos> fs_leave_open /leakfds/held 8\n") != NULL,
          "the prompt and the whole command appear contiguously after the log line");
    CHECK(strstr(s.buf, "[net-stack] tls: no CA trust store\n") != NULL,
          "the log line is emitted whole");
    const char* log_at = strstr(s.buf, "[net-stack]");
    const char* cmd_at = strstr(s.buf, "/ wamos> fs_leave_open /leakfds/held 8");
    CHECK(log_at && cmd_at && log_at < cmd_at, "the repainted line follows the log line");
}

static void test_a_log_line_on_an_idle_prompt_repaints_the_prompt(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    writer(&mux, &s, "/ wamos> ");
    klog(&mux, &s, "[pm] spawned calculator\n");
    CHECK(strcmp(s.buf, "/ wamos> \r\x1b[K[pm] spawned calculator\n/ wamos> ") == 0,
          "the prompt line is erased, the log line printed, the prompt repainted");
}

static void test_a_log_line_at_column_zero_is_emitted_verbatim(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    writer(&mux, &s, "fs-leave-open: left open\n");
    klog(&mux, &s, "[vt] keymap loaded\n");
    CHECK(strcmp(s.buf, "fs-leave-open: left open\n[vt] keymap loaded\n") == 0,
          "no erase and no repaint when no writer line is open");
}

static void test_an_unterminated_log_tail_waits_for_its_newline(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    klog(&mux, &s, "[ata] io regions ");
    CHECK(s.len == 0, "a partial log line is held");
    writer(&mux, &s, "/ wamos> ");
    klog(&mux, &s, "0/1\n");
    CHECK(strcmp(s.buf, "/ wamos> \r\x1b[K[ata] io regions 0/1\n/ wamos> ") == 0,
          "the log line is assembled across writes and emitted once complete");
}

static void test_an_overlong_open_line_is_not_repainted(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    char longline[VT_OUTMUX_LINE_MAX + 16u];
    memset(longline, 'x', sizeof(longline) - 1u);
    longline[sizeof(longline) - 1u] = '\0';
    vt_outmux_init(&mux);
    writer(&mux, &s, longline);
    klog(&mux, &s, "[x] y\n");
    writer(&mux, &s, "z");
    const char* tail = s.buf + strlen(longline);
    CHECK(strcmp(tail, "\n[x] y\nz") == 0,
          "an open line too long to replay is left in place and the log starts below it");
}

static void test_a_log_line_that_fills_the_buffer_is_flushed(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    char longlog[VT_OUTMUX_LINE_MAX + 1u];
    memset(longlog, 'l', VT_OUTMUX_LINE_MAX);
    longlog[VT_OUTMUX_LINE_MAX] = '\0';
    vt_outmux_init(&mux);
    klog(&mux, &s, longlog);
    CHECK(s.len == VT_OUTMUX_LINE_MAX + 1u && s.buf[s.len - 1u] == '\n',
          "a log line that fills the buffer is emitted with a newline of its own");
}

int main(void) {
    test_a_log_line_does_not_split_a_typed_command();
    test_a_log_line_on_an_idle_prompt_repaints_the_prompt();
    test_a_log_line_at_column_zero_is_emitted_verbatim();
    test_an_unterminated_log_tail_waits_for_its_newline();
    test_an_overlong_open_line_is_not_repainted();
    test_a_log_line_that_fills_the_buffer_is_flushed();
    if (g_failures != 0) {
        printf("test_vt_outmux: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_vt_outmux: ok\n");
    return 0;
}
