#include <stdint.h>
#include "ctype.h"
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

/*
 * The CLI is a small user-space shell used both for manual interaction and as a
 * regression target. It is intentionally synchronous from the user's point of
 * view but yields while idle so background processes continue to run.
 */

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
static int32_t g_vt_endpoint = -1;
static int32_t g_active_tty = 1;
static int32_t g_request_id = 1;
static int32_t g_pending_req = -1;
static char g_cwd[64] = "/";
static int32_t g_pending_kind = 0;
static int32_t g_pending_cd_use_path = 0;
static char g_pending_cd_path[32] = "/";
/* Keep in sync with kernel ipc.h */
#define IPC_ERR_FULL (-3)
#define CLI_VT_SEND_RETRIES 1024

static void
stall_forever(void)
{
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_recv(endpoint);
        }
    }
}

enum {
    PENDING_NONE = 0,
    PENDING_LIST,
    PENDING_CAT,
    PENDING_CD,
    PENDING_CD_CHAIN,
    PENDING_EXEC
};

static int32_t
str_len(const char *s)
{
    return (int32_t)strlen(s);
}

static int
str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
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
    return (char)tolower((unsigned char)c);
}

static void
console_write(const char *s)
{
    if (!s) {
        return;
    }
    uint32_t len = (uint32_t)strlen(s);
    if (len == 0) {
        return;
    }
    if (g_vt_endpoint >= 0 && g_reply_endpoint >= 0 && g_active_tty > 0) {
        uint32_t pos = 0;
        while (pos < len) {
            int32_t args[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4 && pos < len; ++i, ++pos) {
                args[i] = (int32_t)(uint8_t)s[pos];
            }
            uint32_t tries = 0;
            for (;;) {
                int32_t rc = wasmos_ipc_send(g_vt_endpoint,
                                             g_reply_endpoint,
                                             VT_IPC_WRITE_REQ,
                                             0,
                                             args[0], args[1], args[2], args[3]);
                if (rc == 0) {
                    break;
                }
                if (rc != IPC_ERR_FULL) {
                    break;
                }
                /* FIXME: if VT endpoint remains full we drop this chunk to
                 * avoid freezing the CLI input loop behind output backpressure. */
                if (++tries >= CLI_VT_SEND_RETRIES) {
                    break;
                }
                (void)wasmos_sched_yield();
            }
        }
    }
    /* tty0 is the system console (serial + ring). For tty1+ we render through
     * VT only so framebuffer output is not duplicated by serial mirroring. */
    if (g_active_tty <= 0 || g_vt_endpoint < 0 || g_reply_endpoint < 0) {
        putsn(s, len);
    }
}

static void
console_prompt(void)
{
    /* Build the full prompt in one buffer so it is emitted as a single
     * console_write call.  Multiple separate calls release the serial spinlock
     * between them; another process can log a '\n' in the gap, causing a
     * scroll that splits the prompt across rows. */
    char buf[80];
    int32_t pos = 0;
    if (g_cwd[0]) {
        int32_t cwd_len = str_len(g_cwd);
        if (cwd_len > (int32_t)(sizeof(buf) - 9)) {
            cwd_len = (int32_t)(sizeof(buf) - 9);
        }
        for (int32_t i = 0; i < cwd_len; ++i) {
            buf[pos++] = g_cwd[i];
        }
        buf[pos++] = ' ';
    }
    const char *suffix = "wamos> ";
    for (int32_t i = 0; suffix[i]; ++i) {
        buf[pos++] = suffix[i];
    }
    buf[pos] = '\0';
    console_write(buf);
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

/* Buffer helpers: append string/uint32 into a fixed-size buffer.
 * cap = total buffer capacity (including NUL terminator).
 * Returns the new write position. */
static int
buf_append_str(char *buf, int pos, int cap, const char *s)
{
    for (int i = 0; s[i] && pos + 1 < cap; ++i) {
        buf[pos++] = s[i];
    }
    return pos;
}

static int
buf_append_u32(char *buf, int pos, int cap, uint32_t v)
{
    if (v == 0) {
        if (pos + 1 < cap) { buf[pos++] = '0'; }
        return pos;
    }
    char tmp[12];
    int tpos = 0;
    while (v > 0 && tpos < (int)sizeof(tmp)) {
        tmp[tpos++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = tpos - 1; i >= 0 && pos + 1 < cap; --i) {
        buf[pos++] = tmp[i];
    }
    return pos;
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
    /* Build the entire line in one buffer so it is emitted atomically. */
    char buf[128];
    int pos = 0;
    for (uint32_t i = 0; i < depth && pos + 2 < (int)sizeof(buf); ++i) {
        buf[pos++] = ' ';
        buf[pos++] = ' ';
    }
    pos = buf_append_str(buf, pos, (int)sizeof(buf), names[index]);
    pos = buf_append_str(buf, pos, (int)sizeof(buf), " (pid ");
    pos = buf_append_u32(buf, pos, (int)sizeof(buf), pids[index]);
    pos = buf_append_str(buf, pos, (int)sizeof(buf), ")\n");
    buf[pos] = '\0';
    console_write(buf);

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
    /* The CLI tracks one outstanding request at a time, which keeps the state
     * machine small and makes the Python QEMU tests deterministic. */
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
set_cwd_path(const char *path)
{
    if (!path || !path[0] || str_eq(path, "/") || str_eq(path, ".") || str_eq(path, "..")) {
        set_cwd_root();
        return;
    }

    char old_cwd[64];
    uint32_t old_len = 0;
    while (g_cwd[old_len] && old_len + 1 < sizeof(old_cwd)) {
        old_cwd[old_len] = g_cwd[old_len];
        old_len++;
    }
    old_cwd[old_len] = '\0';

    uint32_t out = 0;
    g_cwd[0] = '\0';

    if (path[0] == '/') {
        g_cwd[out++] = '/';
    } else if (old_cwd[0] == '/' && old_cwd[1] == '\0') {
        g_cwd[out++] = '/';
    } else {
        uint32_t i = 0;
        while (old_cwd[i] && out + 1 < sizeof(g_cwd)) {
            g_cwd[out++] = old_cwd[i++];
        }
        if (out == 0) {
            g_cwd[out++] = '/';
        } else if (g_cwd[out - 1] != '/' && out + 1 < sizeof(g_cwd)) {
            g_cwd[out++] = '/';
        }
    }

    uint32_t i = 0;
    while (path[i] && out + 1 < sizeof(g_cwd)) {
        if (i == 0 && path[i] == '/') {
            i++;
            continue;
        }
        g_cwd[out++] = path[i++];
    }

    if (out > 1 && g_cwd[out - 1] == '/') {
        out--;
    }
    g_cwd[out] = '\0';
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
        console_write("commands: help, ps, ls, cat <name>, cd <path>, exec <app>, tty <0-3>, halt, reboot\n");
        return 0;
    }
    if (g_line_len > 4 &&
        to_lower(g_line[0]) == 't' &&
        to_lower(g_line[1]) == 't' &&
        to_lower(g_line[2]) == 'y' &&
        g_line[3] == ' ') {
        int32_t tty = (int32_t)(g_line[4] - '0');
        if (g_line[4] < '0' || g_line[4] > '3' || g_line[5] != '\0') {
            console_write("tty usage: tty <0-3>\n");
            return 0;
        }
        if (g_vt_endpoint < 0 || g_reply_endpoint < 0) {
            console_write("tty switch unavailable\n");
            return 0;
        }
        if (wasmos_ipc_send(g_vt_endpoint,
                            g_reply_endpoint,
                            VT_IPC_SWITCH_TTY,
                            0,
                            tty, 0, 0, 0) != 0) {
            console_write("tty switch failed\n");
            return 0;
        }
        g_active_tty = tty;
        if (tty == 0) {
            console_write("switched to tty0 (system console)\n");
        }
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
            g_pending_cd_use_path = 0;
            g_pending_kind = PENDING_CD;
            return 1;
        }
        if (path[0] == '/' && str_len(path) >= 16) {
            uint32_t i = 0;
            while (path[i] && i + 1 < sizeof(g_pending_cd_path)) {
                g_pending_cd_path[i] = path[i];
                i++;
            }
            g_pending_cd_path[i] = '\0';
            if (cli_send_fs(FS_IPC_CHDIR_REQ, 0, 0, 0, 0) != 0) {
                console_write("cd failed\n");
                return 0;
            }
            g_pending_cd_use_path = 1;
            g_pending_kind = PENDING_CD_CHAIN;
            return 1;
        }
        uint32_t packed[4];
        cli_pack_name(path, packed);
        if (cli_send_fs(FS_IPC_CHDIR_REQ, packed[0], packed[1], packed[2], packed[3]) != 0) {
            console_write("cd failed\n");
            return 0;
        }
        g_pending_cd_use_path = 0;
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
        {
            char sbuf[80];
            int spos = 0;
            spos = buf_append_str(sbuf, spos, (int)sizeof(sbuf), "sched: ticks ");
            spos = buf_append_u32(sbuf, spos, (int)sizeof(sbuf), (uint32_t)wasmos_sched_ticks());
            spos = buf_append_str(sbuf, spos, (int)sizeof(sbuf), " ready ");
            spos = buf_append_u32(sbuf, spos, (int)sizeof(sbuf), (uint32_t)wasmos_sched_ready_count());
            spos = buf_append_str(sbuf, spos, (int)sizeof(sbuf), " running ");
            spos = buf_append_u32(sbuf, spos, (int)sizeof(sbuf), (uint32_t)wasmos_sched_current_pid());
            spos = buf_append_str(sbuf, spos, (int)sizeof(sbuf), "\n");
            sbuf[spos] = '\0';
            console_write(sbuf);
        }
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
initialize(int32_t proc_endpoint,
           int32_t fs_endpoint,
           int32_t vt_endpoint,
           int32_t ignored_arg3)
{
    (void)ignored_arg3;

    g_phase = CLI_PHASE_INIT;

    for (;;) {
        if (g_phase == CLI_PHASE_INIT) {
            const char *msg = "WAMOS CLI\ncommands: help, ps, ls, cat <name>, cd <path>, exec <app>, tty <0-3>, halt, reboot\n";
            wasmos_console_write((int32_t)(uintptr_t)msg, str_len(msg));
            set_cwd_root();
            g_reply_endpoint = wasmos_ipc_create_endpoint();
            if (g_reply_endpoint < 0) {
                g_phase = CLI_PHASE_FAILED;
                console_write("[cli] failed to create reply endpoint\n");
                stall_forever();
            }
            g_proc_endpoint = proc_endpoint;
            g_fs_endpoint = fs_endpoint;
            g_vt_endpoint = vt_endpoint;
            if (g_vt_endpoint >= 0) {
                (void)wasmos_ipc_send(g_vt_endpoint,
                                      g_reply_endpoint,
                                      VT_IPC_SWITCH_TTY,
                                      0,
                                      1,
                                      0,
                                      0,
                                      0);
                g_active_tty = 1;
            }
            g_phase = CLI_PHASE_PROMPT;
            continue;
        }

        if (g_phase == CLI_PHASE_PROMPT) {
            console_prompt();
            g_line_len = 0;
            g_phase = CLI_PHASE_READ;
            continue;
        }

        if (g_phase == CLI_PHASE_READ) {
            if (g_line_len >= (int32_t)(sizeof(g_line) - 1)) {
                console_write("\n");
                g_line_len = 0;
                g_phase = CLI_PHASE_PROMPT;
                continue;
            }
            /* Check the keyboard input ring first (fed by the vt service).
             * Fall back to the serial console for headless/test mode. */
            int32_t kbd = wasmos_input_read();
            if (kbd >= 0) {
                g_line[g_line_len] = (char)(kbd & 0xFF);
            } else {
                int32_t rc = wasmos_console_read((int32_t)(uintptr_t)&g_line[g_line_len], 1);
                if (rc == 0) {
                    (void)wasmos_sched_yield();
                    continue;
                }
                if (rc < 0) {
                    g_phase = CLI_PHASE_FAILED;
                    console_write("[cli] console read failed\n");
                    stall_forever();
                }
            }

            char ch = g_line[g_line_len];
            if (ch == '\r' || ch == '\n') {
                console_write("\n");
                if (cli_handle_line()) {
                    g_phase = CLI_PHASE_WAIT_IPC;
                } else {
                    g_phase = CLI_PHASE_PROMPT;
                }
                continue;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (g_line_len > 0) {
                    g_line_len--;
                    console_write("\b \b");
                }
                continue;
            }
            g_line_len++;
            char echo_buf[2] = { ch, '\0' };
            console_write(echo_buf);
            continue;
        }

        if (g_phase == CLI_PHASE_WAIT_IPC) {
            int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
            if (recv_rc < 0) {
                g_phase = CLI_PHASE_FAILED;
                console_write("[cli] ipc recv failed\n");
                stall_forever();
            }
            int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
            int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            if (resp_req != g_pending_req) {
                g_phase = CLI_PHASE_FAILED;
                console_write("[cli] ipc response mismatch\n");
                stall_forever();
            }
            if (resp_type == PROC_IPC_ERROR) {
                console_write("exec failed\n");
            } else if (resp_type == FS_IPC_ERROR || (resp_type == FS_IPC_RESP && resp_status != 0)) {
                console_write("fs failed\n");
            } else if (resp_type != FS_IPC_RESP && resp_type != PROC_IPC_RESP) {
                g_phase = CLI_PHASE_FAILED;
                console_write("[cli] ipc response invalid\n");
                stall_forever();
            } else if (g_pending_kind == PENDING_EXEC && resp_type == PROC_IPC_RESP) {
                {
                char pbuf[32];
                int ppos = 0;
                ppos = buf_append_str(pbuf, ppos, (int)sizeof(pbuf), "spawned pid ");
                ppos = buf_append_u32(pbuf, ppos, (int)sizeof(pbuf), (uint32_t)resp_status);
                ppos = buf_append_str(pbuf, ppos, (int)sizeof(pbuf), "\n");
                pbuf[ppos] = '\0';
                console_write(pbuf);
            }
            } else if (g_pending_kind == PENDING_CD_CHAIN) {
                const char *tail = g_pending_cd_path;
                if (tail[0] == '/') {
                    tail++;
                }
                uint32_t packed[4];
                cli_pack_name(tail, packed);
                if (cli_send_fs(FS_IPC_CHDIR_REQ, packed[0], packed[1], packed[2], packed[3]) != 0) {
                    console_write("cd failed\n");
                    g_pending_req = -1;
                    g_pending_kind = PENDING_NONE;
                    g_pending_cd_use_path = 0;
                    g_phase = CLI_PHASE_PROMPT;
                    continue;
                }
                g_pending_kind = PENDING_CD;
                continue;
            } else if (g_pending_kind == PENDING_CD) {
                if (g_pending_cd_use_path) {
                    set_cwd_path(g_pending_cd_path);
                    g_pending_cd_use_path = 0;
                } else if (g_line_len > 3) {
                    const char *path = &g_line[3];
                    set_cwd_path(path);
                } else {
                    set_cwd_root();
                }
            }
            g_pending_req = -1;
            g_pending_kind = PENDING_NONE;
            g_phase = CLI_PHASE_PROMPT;
            continue;
        }

        console_write("[cli] failed\n");
        stall_forever();
    }
}
