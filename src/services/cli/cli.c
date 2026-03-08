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
extern int32_t wasmos_proc_info_ex(int32_t index, int32_t ptr, int32_t len, int32_t parent_ptr)
    WASMOS_WASM_IMPORT("wasmos", "proc_info_ex");
extern int32_t wasmos_system_halt(void)
    WASMOS_WASM_IMPORT("wasmos", "system_halt");
extern int32_t wasmos_system_reboot(void)
    WASMOS_WASM_IMPORT("wasmos", "system_reboot");

typedef enum {
    CLI_PHASE_INIT = 0,
    CLI_PHASE_PROMPT,
    CLI_PHASE_READ,
    CLI_PHASE_WAIT_IPC,
    CLI_PHASE_FAILED
} cli_phase_t;

#define CLI_MAX_PROCS 16

static cli_phase_t g_phase = CLI_PHASE_INIT;
static char g_line[128];
static int32_t g_line_len = 0;
static int32_t g_reply_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_proc_endpoint = -1;
static int32_t g_request_id = 1;
static int32_t g_pending_req = -1;
static char g_cwd[64] = "/";
static int32_t g_pending_kind = 0;

enum {
    PENDING_NONE = 0,
    PENDING_LIST,
    PENDING_CAT,
    PENDING_CD,
    PENDING_EXEC
};

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
set_cwd_root(void)
{
    g_cwd[0] = '/';
    g_cwd[1] = '\0';
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
    if (g_cwd[0]) {
        console_write(g_cwd);
        console_write(" ");
    }
    console_write("wamos> ");
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
console_write_u32(uint32_t value)
{
    char buf[16];
    int pos = 0;
    if (value == 0) {
        buf[pos++] = '0';
    } else {
        uint32_t v = value;
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
    buf[pos] = '\0';
    console_write(buf);
}

static int
cli_find_index_by_pid(uint32_t count, const uint32_t *pids, uint32_t pid)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (pids[i] == pid) {
            return (int)i;
        }
    }
    return -1;
}

static void
cli_print_tree(uint32_t index,
               uint32_t count,
               const uint32_t *pids,
               const uint32_t *parents,
               const char names[][32],
               uint8_t *visited,
               uint32_t depth)
{
    if (index >= count || depth > 16 || visited[index]) {
        return;
    }
    visited[index] = 1;
    for (uint32_t i = 0; i < depth; ++i) {
        console_write("  ");
    }
    console_write(names[index]);
    console_write(" (pid ");
    console_write_u32(pids[index]);
    console_write(")\n");

    uint32_t pid = pids[index];
    for (uint32_t i = 0; i < count; ++i) {
        if (parents[i] == pid && i != index) {
            cli_print_tree(i, count, pids, parents, names, visited, depth + 1);
        }
    }
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
    int32_t req_id = g_request_id++;
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
    g_pending_req = req_id;
    return 0;
}

static int
cli_send_proc(int32_t type, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0) {
        return -1;
    }
    int32_t req_id = g_request_id++;
    if (wasmos_ipc_send(g_proc_endpoint,
                        g_reply_endpoint,
                        type,
                        req_id,
                        (int32_t)arg0,
                        (int32_t)arg1,
                        (int32_t)arg2,
                        (int32_t)arg3) != 0) {
        return -1;
    }
    g_pending_req = req_id;
    return 0;
}

static void
set_cwd_name(const char *name)
{
    if (!name || !name[0]) {
        set_cwd_root();
        return;
    }
    g_cwd[0] = '/';
    uint32_t i = 0;
    while (name[i] && i < sizeof(g_cwd) - 2) {
        g_cwd[i + 1] = name[i];
        i++;
    }
    g_cwd[i + 1] = '\0';
}

static void
cli_trim_name(char *name)
{
    if (!name) {
        return;
    }
    uint32_t len = 0;
    while (name[len]) {
        len++;
    }
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t')) {
        name[len - 1] = '\0';
        len--;
    }
}

static void
cli_extract_exec_name(const char *input, char *out, uint32_t out_len)
{
    uint32_t start = 0;
    uint32_t end = 0;
    if (!input || !out || out_len == 0) {
        return;
    }
    while (input[end]) {
        if (input[end] == '/' || input[end] == '\\') {
            start = end + 1;
        }
        end++;
    }
    uint32_t len = 0;
    for (uint32_t i = start; i < end && len + 1 < out_len; ++i) {
        out[len++] = to_lower(input[i]);
    }
    out[len] = '\0';
    cli_trim_name(out);
    len = 0;
    while (out[len]) {
        len++;
    }
    if (len >= 10) {
        const char *ext = out + (len - 10);
        if (ext[0] == '.' &&
            ext[1] == 'w' && ext[2] == 'a' && ext[3] == 's' && ext[4] == 'm' &&
            ext[5] == 'o' && ext[6] == 's' && ext[7] == 'a' && ext[8] == 'p' &&
            ext[9] == 'p') {
            out[len - 10] = '\0';
        }
    }
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
        console_write("commands: help, ps, ls, cat <name>, cd <path>, exec <app>, halt, reboot\n");
        return 0;
    }
    if (g_line_len > 3 &&
        to_lower(g_line[0]) == 'c' &&
        to_lower(g_line[1]) == 'd' &&
        g_line[2] == ' ') {
        const char *path = &g_line[3];
        if (str_eq(path, "/") || str_eq(path, ".") || str_eq(path, "..")) {
            set_cwd_root();
            if (cli_send_fs(FS_IPC_CHDIR_REQ, 0, 0, 0, 0) != 0) {
                console_write("cd failed\n");
                return 0;
            }
            g_pending_kind = PENDING_CD;
            return 1;
        }
        uint32_t packed[4];
        cli_pack_name(path, packed);
        if (cli_send_fs(FS_IPC_CHDIR_REQ, packed[0], packed[1], packed[2], packed[3]) != 0) {
            console_write("cd failed\n");
            return 0;
        }
        g_pending_kind = PENDING_CD;
        return 1;
    }
    if (g_line_len > 5 &&
        to_lower(g_line[0]) == 'e' &&
        to_lower(g_line[1]) == 'x' &&
        to_lower(g_line[2]) == 'e' &&
        to_lower(g_line[3]) == 'c' &&
        g_line[4] == ' ') {
        char name[32];
        name[0] = '\0';
        cli_extract_exec_name(&g_line[5], name, sizeof(name));
        if (name[0] == '\0') {
            console_write("exec failed\n");
            return 0;
        }
        if (str_len(name) > 15) {
            console_write("exec name too long\n");
            return 0;
        }
        uint32_t packed[4];
        cli_pack_name(name, packed);
        if (cli_send_proc(PROC_IPC_SPAWN_NAME, packed[0], packed[1], packed[2], packed[3]) != 0) {
            console_write("exec failed\n");
            return 0;
        }
        g_pending_kind = PENDING_EXEC;
        return 1;
    }
    if (g_line_len == 4 &&
        to_lower(g_line[0]) == 'h' &&
        to_lower(g_line[1]) == 'a' &&
        to_lower(g_line[2]) == 'l' &&
        to_lower(g_line[3]) == 't') {
        wasmos_system_halt();
        return 0;
    }
    if (g_line_len == 6 &&
        to_lower(g_line[0]) == 'r' &&
        to_lower(g_line[1]) == 'e' &&
        to_lower(g_line[2]) == 'b' &&
        to_lower(g_line[3]) == 'o' &&
        to_lower(g_line[4]) == 'o' &&
        to_lower(g_line[5]) == 't') {
        wasmos_system_reboot();
        return 0;
    }
    if (g_line_len == 2 &&
        to_lower(g_line[0]) == 'p' &&
        to_lower(g_line[1]) == 's') {
        uint32_t pids[CLI_MAX_PROCS];
        uint32_t parents[CLI_MAX_PROCS];
        char names[CLI_MAX_PROCS][32];
        uint8_t visited[CLI_MAX_PROCS];

        int32_t count = wasmos_proc_count();
        if (count <= 0) {
            console_write("no processes\n");
            return 0;
        }
        if (count > (int32_t)CLI_MAX_PROCS) {
            count = (int32_t)CLI_MAX_PROCS;
        }
        console_write_num("processes: ", count);
        for (int32_t i = 0; i < count; ++i) {
            uint32_t parent = 0;
            int32_t pid = wasmos_proc_info_ex(i,
                                              (int32_t)(uintptr_t)names[i],
                                              (int32_t)sizeof(names[i]),
                                              (int32_t)(uintptr_t)&parent);
            if (pid <= 0) {
                pids[i] = 0;
                parents[i] = 0;
                names[i][0] = '\0';
                continue;
            }
            pids[i] = (uint32_t)pid;
            parents[i] = parent;
            visited[i] = 0;
        }
        console_write("tree:\n");
        for (int32_t i = 0; i < count; ++i) {
            if (pids[i] == 0) {
                continue;
            }
            int parent_index = cli_find_index_by_pid((uint32_t)count, pids, parents[i]);
            if (parents[i] == 0 || parent_index < 0 || parents[i] == pids[i]) {
                cli_print_tree((uint32_t)i, (uint32_t)count, pids, parents, names, visited, 0);
            }
        }
        for (int32_t i = 0; i < count; ++i) {
            if (pids[i] != 0 && !visited[i]) {
                cli_print_tree((uint32_t)i, (uint32_t)count, pids, parents, names, visited, 0);
            }
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
        g_pending_kind = PENDING_LIST;
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
        g_pending_kind = PENDING_CAT;
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
        const char *msg = "WAMOS CLI\ncommands: help, ps, ls, cat <name>, cd <path>, exec <app>, halt, reboot\n";
        wasmos_console_write((int32_t)(uintptr_t)msg, str_len(msg));
        set_cwd_root();
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        g_proc_endpoint = proc_endpoint;
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
                g_phase = CLI_PHASE_WAIT_IPC;
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

    if (g_phase == CLI_PHASE_WAIT_IPC) {
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
        if (resp_req != g_pending_req) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        if (resp_type == PROC_IPC_ERROR) {
            console_write("exec failed\n");
        } else if (resp_type == FS_IPC_ERROR || (resp_type == FS_IPC_RESP && resp_status != 0)) {
            console_write("fs failed\n");
        } else if (resp_type != FS_IPC_RESP && resp_type != PROC_IPC_RESP) {
            g_phase = CLI_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        } else if (g_pending_kind == PENDING_EXEC && resp_type == PROC_IPC_RESP) {
            console_write("spawned pid ");
            console_write_u32((uint32_t)resp_status);
            console_write("\n");
        } else if (g_pending_kind == PENDING_CD) {
            if (g_line_len > 3) {
                const char *path = &g_line[3];
                if (str_eq(path, "/") || str_eq(path, ".") || str_eq(path, "..")) {
                    set_cwd_root();
                } else {
                    set_cwd_name(path);
                }
            } else {
                set_cwd_root();
            }
        }
        g_pending_req = -1;
        g_pending_kind = PENDING_NONE;
        g_phase = CLI_PHASE_PROMPT;
        return WASMOS_WASM_STEP_YIELDED;
    }

    return WASMOS_WASM_STEP_FAILED;
}
