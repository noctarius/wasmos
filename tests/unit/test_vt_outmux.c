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

/* Regression: 2026-11-16-outmux-edit-history -- the open line recorded every
 * byte the shell wrote, including the "\b \b" it sends for each backspace, so
 * about forty corrections filled the 256-byte buffer even under a short
 * command. The next log line then took the overflow path: the prompt and the
 * input were not repainted, and typing continued below the log line with no
 * prompt in front of it. */
static void test_corrections_do_not_exhaust_the_open_line(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    writer(&mux, &s, "/ wamos> ");
    type(&mux, &s, "abc");
    for (int i = 0; i < 100; ++i) {
        type(&mux, &s, "x");
        writer(&mux, &s, "\b \b");
    }
    s.len = 0;
    s.buf[0] = '\0';
    klog(&mux, &s, "[x] y\n");
    CHECK(strcmp(s.buf, "\r\x1b[K[x] y\n/ wamos> abc") == 0,
          "the line as it stands is repainted, not its editing history");
}

/* Regression: 2026-11-16-outmux-wrapped-line -- the erase before a log line
 * was "\r" + CSI K, which clears only the cursor's row. A prompt and command
 * longer than the terminal width wrap onto a second row; a log line arriving
 * then left the first row standing, and the repaint printed the prompt and
 * command a second time below it. */
static void test_a_wrapped_open_line_is_erased_whole(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    vt_outmux_set_cols(&mux, 20);
    writer(&mux, &s, "/ wamos> ");
    type(&mux, &s, "0123456789abcdefghijklmno");
    s.len = 0;
    s.buf[0] = '\0';
    klog(&mux, &s, "[x] y\n");
    CHECK(strcmp(s.buf, "\r\x1b[1A\x1b[J[x] y\n/ wamos> 0123456789abcdefghijklmno") == 0,
          "the erase starts at the open line's first row and clears to the end");
}

/* A cursor left inside the line by a carriage return is restored after the
 * repaint, so the writer's next byte lands where it would have. */
static void test_the_cursor_is_restored_after_the_repaint(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    writer(&mux, &s, "progress 10%\rprog");
    s.len = 0;
    s.buf[0] = '\0';
    klog(&mux, &s, "[x] y\n");
    CHECK(strcmp(s.buf, "\r\x1b[K[x] y\nprogress 10%\x1b[8D") == 0,
          "the whole line is repainted and the cursor moved back to column 4");
}

/* An escape sequence in the open line cannot be replayed by column; the line
 * is left standing and the log starts below it, as for an overlong line. */
static void test_an_open_line_with_an_escape_is_not_repainted(void) {
    vt_outmux_t mux;
    sink_t s = {0};
    vt_outmux_init(&mux);
    writer(&mux, &s, "\x1b[1mbold");
    s.len = 0;
    s.buf[0] = '\0';
    klog(&mux, &s, "[x] y\n");
    CHECK(strcmp(s.buf, "\n[x] y\n") == 0, "no erase and no repaint");
}

int main(void) {
    test_a_log_line_does_not_split_a_typed_command();
    test_a_log_line_on_an_idle_prompt_repaints_the_prompt();
    test_a_log_line_at_column_zero_is_emitted_verbatim();
    test_an_unterminated_log_tail_waits_for_its_newline();
    test_an_overlong_open_line_is_not_repainted();
    test_a_log_line_that_fills_the_buffer_is_flushed();
    test_corrections_do_not_exhaust_the_open_line();
    test_a_wrapped_open_line_is_erased_whole();
    test_the_cursor_is_restored_after_the_repaint();
    test_an_open_line_with_an_escape_is_not_repainted();
    if (g_failures != 0) {
        printf("test_vt_outmux: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_vt_outmux: ok\n");
    return 0;
}
