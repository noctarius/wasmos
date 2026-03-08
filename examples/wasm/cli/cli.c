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
extern int32_t wasmos_ipc_create_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint,
                               int32_t source_endpoint,
                               int32_t type,
                               int32_t request_id,
                               int32_t arg0,
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");
extern int32_t wasmos_proc_count(void)
    WASMOS_WASM_IMPORT("wasmos", "proc_count");
extern int32_t wasmos_proc_info(int32_t index, int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "proc_info");
extern int32_t wasmos_system_halt(void)
    WASMOS_WASM_IMPORT("wasmos", "system_halt");

typedef enum {
    CLI_PHASE_INIT = 0,
    CLI_PHASE_PROMPT,
    CLI_PHASE_READ,
    CLI_PHASE_WAIT_FS,
    CLI_PHASE_FAILED
} cli_phase_t;

static cli_phase_t g_phase = CLI_PHASE_INIT;
static char g_line[128];
static int32_t g_line_len = 0;
static int32_t g_reply_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_fs_request_id = 1;
static int32_t g_fs_pending_req = -1;

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

static char
to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
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
cli_pack_name(const char *name, uint32_t out[4])
{
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = 0;
    }
    if (!name) {
        return;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; name[i] && idx < 16; ++i, ++idx) {
        uint32_t slot = idx / 4;
        uint32_t shift = (idx % 4) * 8;
        out[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
}

static int
cli_send_fs(int32_t type, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    if (g_fs_endpoint < 0 || g_reply_endpoint < 0) {
        return -1;
    }
    int32_t req_id = g_fs_request_id++;
    if (wasmos_ipc_send(g_fs_endpoint,
                        g_reply_endpoint,
                        type,
                        req_id,
                        (int32_t)arg0,
                        (int32_t)arg1,
                        (int32_t)arg2,
                        (int32_t)arg3) != 0) {
        return -1;
    }
    g_fs_pending_req = req_id;
    return 0;
}

static int
cli_handle_line(void)
{
    g_line[g_line_len] = '\0';
    if (g_line_len == 0) {
        return 0;
    }
    if (g_line_len == 4 &&
        to_lower(g_line[0]) == 'h' &&
        to_lower(g_line[1]) == 'e' &&
        to_lower(g_line[2]) == 'l' &&
        to_lower(g_line[3]) == 'p') {
        console_write("commands: help, ps, ls, cat <name>, halt\n");
        return 0;
    }
    if (g_line_len == 4 &&
        to_lower(g_line[0]) == 'h' &&
        to_lower(g_line[1]) == 'a' &&
        to_lower(g_line[2]) == 'l' &&
        to_lower(g_line[3]) == 't') {
        wasmos_system_halt();
        return 0;
    }
    if (g_line_len == 2 &&
        to_lower(g_line[0]) == 'p' &&
        to_lower(g_line[1]) == 's') {
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
        return 0;
    }
    if (g_line_len == 2 &&
        to_lower(g_line[0]) == 'l' &&
        to_lower(g_line[1]) == 's') {
        if (cli_send_fs(FS_IPC_LIST_ROOT_REQ, 0, 0, 0, 0) != 0) {
            console_write("ls failed\n");
            return 0;
        }
        return 1;
    }
    if (g_line_len > 4 && to_lower(g_line[0]) == 'c' && to_lower(g_line[1]) == 'a' &&
        to_lower(g_line[2]) == 't' && g_line[3] == ' ') {
        char *name = &g_line[4];
        uint32_t packed[4];
        cli_pack_name(name, packed);
        if (cli_send_fs(FS_IPC_CAT_ROOT_REQ, packed[0], packed[1], packed[2], packed[3]) != 0) {
            console_write("cat failed\n");
            return 0;
        }
        return 1;
    }
    console_write("unknown command\n");
    return 0;
}

WASMOS_WASM_EXPORT int32_t
cli_step(int32_t ignored_type,
         int32_t proc_endpoint,
         int32_t fs_endpoint,
         int32_t ignored_arg2,
         int32_t ignored_arg3)
{
    (void)ignored_type;
    (void)proc_endpoint;
    (void)fs_endpoint;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (g_phase == CLI_PHASE_INIT) {
        const char *msg = "WASMOS CLI\ncommands: help, ps, ls, cat <name>, halt\n";
        wasmos_console_write((int32_t)(uintptr_t)msg, str_len(msg));
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        g_fs_endpoint = fs_endpoint;
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
            if (cli_handle_line()) {
                g_phase = CLI_PHASE_WAIT_FS;
            } else {
                g_phase = CLI_PHASE_PROMPT;
            }
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

    if (g_phase == CLI_PHASE_WAIT_FS) {
        int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
        if (recv_rc == 0) {
            return WASMOS_WASM_STEP_BLOCKED;
        }
        if (recv_rc < 0) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        if (resp_req != g_fs_pending_req) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        if (resp_type == FS_IPC_ERROR || resp_status != 0) {
            console_write("fs failed\n");
        } else if (resp_type != FS_IPC_RESP) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        g_fs_pending_req = -1;
        g_phase = CLI_PHASE_PROMPT;
        return WASMOS_WASM_STEP_YIELDED;
    }

    return WASMOS_WASM_STEP_FAILED;
}
