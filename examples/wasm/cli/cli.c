#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, import_name)
#define WASMOS_WASM_EXPORT
#endif

extern int32_t wasmos_console_write(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_write");
extern int32_t wasmos_console_read(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_read");
extern int32_t wasmos_proc_count(void)
    WASMOS_WASM_IMPORT("wasmos", "proc_count");
extern int32_t wasmos_proc_info(int32_t index, int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "proc_info");
extern int32_t wasmos_fs_list_root(void)
    WASMOS_WASM_IMPORT("wasmos", "fs_list_root");
extern int32_t wasmos_fs_cat_root(int32_t ptr)
    WASMOS_WASM_IMPORT("wasmos", "fs_cat_root");

typedef enum {
    CLI_PHASE_INIT = 0,
    CLI_PHASE_PROMPT,
    CLI_PHASE_READ,
    CLI_PHASE_FAILED
} cli_phase_t;

static cli_phase_t g_phase = CLI_PHASE_INIT;
static char g_line[128];
static int32_t g_line_len = 0;

static int32_t
str_len(const char *s)
{
    int32_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

static int
str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void
console_write(const char *s)
{
    int32_t len = str_len(s);
    if (len <= 0) {
        return;
    }
    wasmos_console_write((int32_t)(uintptr_t)s, len);
}

static void
console_prompt(void)
{
    console_write("wasmos> ");
}

static void
console_write_num(const char *label, int32_t value)
{
    char buf[32];
    int pos = 0;
    if (label) {
        console_write(label);
    }
    if (value == 0) {
        buf[pos++] = '0';
    } else {
        int32_t v = value;
        char tmp[16];
        int tpos = 0;
        while (v > 0 && tpos < (int)sizeof(tmp)) {
            tmp[tpos++] = (char)('0' + (v % 10));
            v /= 10;
        }
        for (int i = tpos - 1; i >= 0; --i) {
            buf[pos++] = tmp[i];
        }
    }
    buf[pos++] = '\n';
    buf[pos] = '\0';
    console_write(buf);
}

static void
cli_handle_line(void)
{
    g_line[g_line_len] = '\0';
    if (g_line_len == 0) {
        return;
    }
    if (str_eq(g_line, "help")) {
        console_write("commands: help, ps, ls, cat <name>\n");
        return;
    }
    if (str_eq(g_line, "ps")) {
        int32_t count = wasmos_proc_count();
        console_write_num("processes: ", count);
        for (int32_t i = 0; i < count; ++i) {
            char name_buf[32];
            int32_t pid = wasmos_proc_info(i, (int32_t)(uintptr_t)name_buf, (int32_t)sizeof(name_buf));
            if (pid <= 0) {
                continue;
            }
            console_write_num("pid: ", pid);
            console_write("name: ");
            console_write(name_buf);
            console_write("\n");
        }
        return;
    }
    if (str_eq(g_line, "ls")) {
        if (wasmos_fs_list_root() != 0) {
            console_write("ls failed\n");
        }
        return;
    }
    if (g_line_len > 4 && g_line[0] == 'c' && g_line[1] == 'a' &&
        g_line[2] == 't' && g_line[3] == ' ') {
        char *name = &g_line[4];
        if (wasmos_fs_cat_root((int32_t)(uintptr_t)name) != 0) {
            console_write("cat failed\n");
        }
        return;
    }
    console_write("unknown command\n");
}

WASMOS_WASM_EXPORT int32_t
cli_step(int32_t ignored_type,
         int32_t ignored_arg0,
         int32_t ignored_arg1,
         int32_t ignored_arg2,
         int32_t ignored_arg3)
{
    (void)ignored_type;
    (void)ignored_arg0;
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (g_phase == CLI_PHASE_INIT) {
        const char *msg = "WASMOS CLI\ncommands: help, ps\n";
        wasmos_console_write((int32_t)(uintptr_t)msg, str_len(msg));
        g_phase = CLI_PHASE_PROMPT;
        return WASMOS_WASM_STEP_YIELDED;
    }

    if (g_phase == CLI_PHASE_PROMPT) {
        console_prompt();
        g_line_len = 0;
        g_phase = CLI_PHASE_READ;
        return WASMOS_WASM_STEP_YIELDED;
    }

    if (g_phase == CLI_PHASE_READ) {
        if (g_line_len >= (int32_t)(sizeof(g_line) - 1)) {
            console_write("\n");
            g_line_len = 0;
            g_phase = CLI_PHASE_PROMPT;
            return WASMOS_WASM_STEP_YIELDED;
        }
        int32_t rc = wasmos_console_read((int32_t)(uintptr_t)&g_line[g_line_len], 1);
        if (rc == 0) {
            return WASMOS_WASM_STEP_YIELDED;
        }
        if (rc < 0) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        char ch = g_line[g_line_len];
        if (ch == '\r' || ch == '\n') {
            console_write("\n");
            cli_handle_line();
            g_phase = CLI_PHASE_PROMPT;
            return WASMOS_WASM_STEP_YIELDED;
        }
        if (ch == '\b' || ch == 0x7F) {
            if (g_line_len > 0) {
                g_line_len--;
                console_write("\b \b");
            }
            return WASMOS_WASM_STEP_YIELDED;
        }
        g_line_len++;
        char echo_buf[2] = { ch, '\0' };
        console_write(echo_buf);
        return WASMOS_WASM_STEP_YIELDED;
    }

    return WASMOS_WASM_STEP_FAILED;
}
