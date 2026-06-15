/* cli.c - interactive shell service; dispatches IPC via a phase state machine
 * and routes console output to the VT service or serial depending on TTY focus */
#include <stdint.h>
#include "ctype.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/script.h"
#include "wasmos_driver_abi.h"
#include "cli_types.h"

/*
 * The CLI is a small user-space shell used both for manual interaction and as a
 * regression target. It is intentionally synchronous from the user's point of
 * view but yields while idle so background processes continue to run.
 */

static cli_phase_t g_phase = CLI_PHASE_INIT;
static char g_line[128];
static int32_t g_line_len = 0;
static int32_t g_reply_endpoint = -1;
static int32_t g_vt_client_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_devmgr_endpoint = -1;
static int32_t g_proc_endpoint = -1;
static int32_t g_vt_endpoint = -1;
static int32_t g_home_tty = 1;
static int32_t g_last_seen_active_tty = 1;
static uint32_t g_vt_switch_generation = 1;
static int32_t g_request_id = 1;
static int32_t g_pending_req = -1;
static char g_cwd[64] = "/";
static int32_t g_pending_kind = 0;
static int32_t g_pending_exec_pid = -1;
static int32_t g_pending_cd_use_path = 0;
static char g_pending_cd_path[32] = "/";
static char g_history[CLI_HISTORY_MAX][sizeof(g_line)];
static uint8_t g_history_len[CLI_HISTORY_MAX];
static uint8_t g_history_count = 0;
static uint8_t g_history_head = 0;
static int8_t g_history_nav = -1;
static char g_history_scratch[sizeof(g_line)];
static int32_t g_history_scratch_len = 0;
static uint8_t g_history_have_scratch = 0;
static uint8_t g_esc_state = 0;
static uint8_t g_vt_read_backoff = 0;
static uint8_t g_fg_query_backoff = 0;
static cli_env_var_t *g_env = 0;
static wasmos_script_state_t g_cli_script_state;

static void
set_cwd_root(void)
{
    g_cwd[0] = '/';
    g_cwd[1] = '\0';
}

static char
to_lower(char c)
{
    return wasmos_sys_to_lower(c);
}

/* Case-insensitive exact match of g_line against s. */
static int
line_eq_ci(const char *s)
{
    int32_t i = 0;
    if (!s) {
        return 0;
    }
    while (s[i]) {
        if (i >= g_line_len || to_lower(g_line[i]) != to_lower(s[i])) {
            return 0;
        }
        i++;
    }
    return i == g_line_len;
}

/* Case-insensitive prefix match of g_line against s. */
static int
line_starts_with_ci(const char *s)
{
    int32_t i = 0;
    if (!s) {
        return 0;
    }
    while (s[i]) {
        if (i >= g_line_len || to_lower(g_line[i]) != to_lower(s[i])) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int
str_ends_with(const char *s, const char *suffix)
{
    int32_t slen = 0;
    int32_t xlen = 0;
    if (!s || !suffix) {
        return 0;
    }
    while (s[slen]) {
        slen++;
    }
    while (suffix[xlen]) {
        xlen++;
    }
    if (slen < xlen) {
        return 0;
    }
    return wasmos_sys_strcmp(s + (slen - xlen), suffix) == 0;
}

static int
str_starts_with_ci(const char *s, const char *prefix)
{
    int32_t i = 0;
    if (!s || !prefix) {
        return 0;
    }
    while (prefix[i]) {
        if (!s[i] || to_lower(s[i]) != to_lower(prefix[i])) {
            return 0;
        }
        i++;
    }
    return 1;
}

static void
console_write(const char *s);

static int
str_find_char(const char *s, char ch)
{
    int32_t i = 0;
    if (!s) {
        return -1;
    }
    while (s[i]) {
        if (s[i] == ch) {
            return i;
        }
        i++;
    }
    return -1;
}

/* Find a shell variable by name; is_export selects the exported vs local list. */
static cli_env_var_t *
cli_env_find(const char *name, int is_export)
{
    cli_env_var_t *it = g_env;
    if (!name || !name[0]) {
        return 0;
    }
    while (it) {
        if (it->is_export == (uint8_t)(is_export ? 1 : 0) && wasmos_sys_streq(it->name, name)) {
            return it;
        }
        it = it->next;
    }
    return 0;
}

static const char *
cli_env_get(const char *name)
{
    cli_env_var_t *var = cli_env_find(name, 0);
    if (var) {
        return var->value;
    }
    var = cli_env_find(name, 1);
    return var ? var->value : 0;
}

static void
cli_env_clear(void)
{
    cli_env_var_t *it = g_env;
    while (it) {
        cli_env_var_t *next = it->next;
        free(it);
        it = next;
    }
    g_env = 0;
}

/* Create or update a shell variable; passing NULL/empty value removes it. */
static int
cli_env_set(const char *name, const char *value, int is_export)
{
    cli_env_var_t *it = 0;
    cli_env_var_t *prev = 0;
    if (!name || !name[0]) {
        return -1;
    }
    if (wasmos_sys_strlen(name) >= CLI_ENV_NAME_MAX) {
        return -1;
    }
    for (it = g_env; it; prev = it, it = it->next) {
        if (it->is_export == (uint8_t)(is_export ? 1 : 0) && wasmos_sys_streq(it->name, name)) {
            break;
        }
    }
    if (!value || value[0] == '\0') {
        if (it) {
            if (prev) {
                prev->next = it->next;
            } else {
                g_env = it->next;
            }
            free(it);
        }
        return 0;
    }
    if (wasmos_sys_strlen(value) >= CLI_ENV_VALUE_MAX) {
        return -1;
    }
    if (!it) {
        it = (cli_env_var_t *)malloc(sizeof(*it));
        if (!it) {
            return -1;
        }
        memset(it, 0, sizeof(*it));
        it->is_export = is_export ? 1 : 0;
        it->next = g_env;
        g_env = it;
    }
    it->is_export = is_export ? 1 : 0;
    (void)snprintf(it->name, sizeof(it->name), "%s", name);
    (void)snprintf(it->value, sizeof(it->value), "%s", value);
    return 0;
}

static void
cli_env_init_defaults(void)
{
    cli_env_clear();
    (void)cli_env_set("PATH", "/boot/apps:/boot/system/services:/boot/system/drivers:/boot/system/utils", 1);
    (void)cli_env_set("?", "0", 0);
}

static int
cli_parse_name_value(const char *line, char *name, int name_cap,
                     char *value, int val_cap, int32_t *out_nlen, int32_t *out_vlen)
{
    int32_t start = 0;
    int32_t eq = -1;
    int32_t nlen = 0;
    int32_t vlen = 0;
    if (!line) {
        return -1;
    }
    while (line[start] == ' ' || line[start] == '\t') {
        start++;
    }
    eq = str_find_char(&line[start], '=');
    if (eq <= 0) {
        return -1;
    }
    nlen = eq;
    while (nlen > 0 &&
           (line[start + nlen - 1] == ' ' ||
            line[start + nlen - 1] == '\t' ||
            line[start + nlen - 1] == '\r' ||
            line[start + nlen - 1] == '\n')) {
        nlen--;
    }
    if (nlen <= 0 || nlen >= name_cap) {
        return -1;
    }
    for (int32_t i = 0; i < nlen; ++i) {
        name[i] = line[start + i];
    }
    name[nlen] = '\0';
    {
        int32_t value_start = start + eq + 1;
        while (line[value_start] == ' ' || line[value_start] == '\t') {
            value_start++;
        }
        while (line[value_start + vlen] && vlen + 1 < val_cap) {
            value[vlen] = line[value_start + vlen];
            vlen++;
        }
        value[vlen] = '\0';
        while (vlen > 0 &&
               (value[vlen - 1] == ' ' ||
                value[vlen - 1] == '\t' ||
                value[vlen - 1] == '\r' ||
                value[vlen - 1] == '\n')) {
            value[vlen - 1] = '\0';
            vlen--;
        }
    }
    if (out_nlen) {
        *out_nlen = nlen;
    }
    if (out_vlen) {
        *out_vlen = vlen;
    }
    return 0;
}

static int
cli_env_apply_export_line(const char *line)
{
    char name[CLI_ENV_NAME_MAX];
    char value[CLI_ENV_VALUE_MAX];
    int32_t nlen = 0;
    int32_t vlen = 0;
    if (cli_parse_name_value(line, name, (int)sizeof(name), value, (int)sizeof(value), &nlen, &vlen) != 0) {
        return -1;
    }
    (void)nlen;
    (void)vlen;
    return cli_env_set(name, value, 1);
}

static int
cli_env_set_local_line(const char *line)
{
    char name[CLI_ENV_NAME_MAX];
    char value[CLI_ENV_VALUE_MAX];
    if (cli_parse_name_value(line, name, (int)sizeof(name), value, (int)sizeof(value), 0, 0) != 0) {
        return -1;
    }
    return cli_env_set(name, value, 0);
}

static int
cli_echo_env_expr(const char *expr)
{
    char out[512];
    char expanded_expr[512];
    uint32_t expanded_len = 0;
    uint32_t out_len = 0;
    int32_t i = 0;
    int no_newline = 0;
    int escape_mode = 0;
    int parse_flags = 1;
    int wrote_any = 0;
    if (!expr) {
        return -1;
    }
    while (expr[i] && expanded_len + 1u < sizeof(expanded_expr)) {
        if (expr[i] == '$' && expr[i + 1] == '{') {
            char name[CLI_ENV_NAME_MAX];
            const char *val = 0;
            int32_t nlen = 0;
            int32_t j = i + 2;
            while (expr[j] && expr[j] != '}' && nlen + 1 < (int32_t)sizeof(name)) {
                name[nlen++] = expr[j++];
            }
            if (expr[j] == '}') {
                name[nlen] = '\0';
                if (nlen > 0) {
                    val = cli_env_get(name);
                    if (val) {
                        for (int32_t vi = 0; val[vi] && expanded_len + 1u < sizeof(expanded_expr); ++vi) {
                            expanded_expr[expanded_len++] = val[vi];
                        }
                    }
                }
                i = j + 1;
                continue;
            }
        }
        expanded_expr[expanded_len++] = expr[i++];
    }
    expanded_expr[expanded_len] = '\0';
    expr = expanded_expr;
    i = 0;

    while (expr[i] == ' ' || expr[i] == '\t') {
        i++;
    }

    while (parse_flags) {
        int32_t j = i;
        int flag_ok = 1;
        while (expr[j] == ' ' || expr[j] == '\t') {
            j++;
        }
        if (expr[j] == '\0') {
            i = j;
            break;
        }
        if (expr[j] == '-' && expr[j + 1] == '-') {
            int32_t k = j + 2;
            while (expr[k] == ' ' || expr[k] == '\t') {
                k++;
            }
            i = k;
            break;
        }
        if (expr[j] != '-' || expr[j + 1] == '\0') {
            i = j;
            break;
        }
        for (int32_t k = j + 1; expr[k] && expr[k] != ' ' && expr[k] != '\t'; ++k) {
            if (expr[k] == 'n') {
                no_newline = 1;
            } else if (expr[k] == 'e') {
                escape_mode = 1;
            } else if (expr[k] == 'E') {
                escape_mode = 0;
            } else {
                flag_ok = 0;
                break;
            }
            j = k + 1;
        }
        if (!flag_ok) {
            break;
        }
        i = j;
    }

    while (expr[i]) {
        int in_single = 0;
        int in_double = 0;
        int token_started = 0;
        int token_emitted = 0;
        while (expr[i] == ' ' || expr[i] == '\t') {
            i++;
        }
        if (expr[i] == '\0') {
            break;
        }
        if (wrote_any && out_len + 1u < sizeof(out)) {
            out[out_len++] = ' ';
        }
        token_emitted = 1;
        while (expr[i]) {
            char c = expr[i];
            if (!in_single && !in_double && (c == ' ' || c == '\t')) {
                break;
            }
            if (!in_double && c == '\'') {
                in_single = !in_single;
                token_started = 1;
                i++;
                continue;
            }
            if (!in_single && c == '"') {
                in_double = !in_double;
                token_started = 1;
                i++;
                continue;
            }
            if (c == '\\') {
                char next = expr[i + 1];
                token_started = 1;
                if (next == '\0') {
                    i++;
                    continue;
                }
                if (!in_single) {
                    char emit = next;
                    if (escape_mode) {
                        if (next == 'n') {
                            emit = '\n';
                        } else if (next == 't') {
                            emit = '\t';
                        } else if (next == 'r') {
                            emit = '\r';
                        } else if (next == '0') {
                            emit = '\0';
                        } else if (next == 'a') {
                            emit = '\a';
                        } else if (next == 'b') {
                            emit = '\b';
                        } else if (next == 'f') {
                            emit = '\f';
                        } else if (next == 'v') {
                            emit = '\v';
                        }
                    }
                    if (emit != '\0' && out_len + 1u < sizeof(out)) {
                        out[out_len++] = emit;
                    }
                    i += 2;
                    continue;
                }
            }
            if (!in_single && c == '$' && expr[i + 1] == '{') {
                char name[CLI_ENV_NAME_MAX];
                int32_t nlen = 0;
                const char *val = 0;
                i += 2;
                while (expr[i] && expr[i] != '}' && nlen + 1 < (int32_t)sizeof(name)) {
                    name[nlen++] = expr[i++];
                }
                if (expr[i] == '}') {
                    name[nlen] = '\0';
                    i++;
                    token_started = 1;
                    if (nlen > 0) {
                        val = cli_env_get(name);
                        if (val) {
                            for (int32_t vi = 0; val[vi] && out_len + 1u < sizeof(out); ++vi) {
                                out[out_len++] = val[vi];
                            }
                        }
                    }
                    continue;
                }
                if (out_len + 2u < sizeof(out)) {
                    out[out_len++] = '$';
                    out[out_len++] = '{';
                }
                for (int32_t ni = 0; ni < nlen && out_len + 1u < sizeof(out); ++ni) {
                    out[out_len++] = name[ni];
                }
                continue;
            }
            token_started = 1;
            if (out_len + 1u < sizeof(out)) {
                out[out_len++] = c;
            }
            i++;
        }
        if (in_single || in_double) {
            return -1;
        }
        if (token_emitted && token_started) {
            wrote_any = 1;
        }
    }

    out[out_len] = '\0';
    console_write(out);
    if (!no_newline) {
        console_write("\n");
    }
    return 0;
}

/* Walk PATH variable segments and probe each <dir>/<prog> via fopen;
 * writes the first match to resolved and returns 0, or -1 if not found. */
static int
cli_resolve_path_from_pathvar(const char *prog, char *resolved, uint32_t resolved_len)
{
    char candidate[96];
    const char *path = cli_env_get("PATH");
    int32_t i = 0;
    int32_t seg_start = 0;
    if (!path || !prog || !prog[0]) {
        return -1;
    }
    while (1) {
        if (path[i] == ':' || path[i] == '\0') {
            int32_t seg_len = i - seg_start;
            if (seg_len > 0 && seg_len < (int32_t)sizeof(candidate)) {
                int32_t pos = 0;
                for (int32_t j = 0; j < seg_len && pos + 1 < (int32_t)sizeof(candidate); ++j) {
                    candidate[pos++] = path[seg_start + j];
                }
                if (pos > 0 && candidate[pos - 1] != '/' && pos + 1 < (int32_t)sizeof(candidate)) {
                    candidate[pos++] = '/';
                }
                for (int32_t j = 0; prog[j] && pos + 1 < (int32_t)sizeof(candidate); ++j) {
                    candidate[pos++] = prog[j];
                }
                candidate[pos] = '\0';
                FILE *f = fopen(candidate, "r");
                if (f) {
                    (void)fclose(f);
                    (void)snprintf(resolved, resolved_len, "%s", candidate);
                    return 0;
                }
            }
            if (path[i] == '\0') {
                break;
            }
            seg_start = i + 1;
        }
        i++;
    }
    return -1;
}

/* Emit a string to the console: sends to the VT service (4 bytes per IPC
 * message, tagged with g_vt_switch_generation) when the CLI's home TTY is
 * the active TTY; otherwise falls back to serial via putsn.
 * g_vt_switch_generation is a monotonic counter that the VT service uses to
 * drop writes from stale writers (e.g. a CLI registered on a previous focus). */
static void
console_write(const char *s)
{
    if (!s) {
        return;
    }
    uint32_t len = (uint32_t)wasmos_sys_strlen(s);
    if (len == 0) {
        return;
    }
    int use_vt = (g_vt_endpoint >= 0 && g_reply_endpoint >= 0 && g_home_tty > 0 &&
        g_vt_client_endpoint >= 0 &&
        g_last_seen_active_tty == g_home_tty);
    if (use_vt) {
        uint32_t pos = 0;
        while (pos < len) {
            int32_t args[4] = {0, 0, 0, 0};
            int count = 0;
            for (int i = 0; i < 4 && pos < len; ++i, ++pos, ++count) {
                args[i] = (int32_t)(uint8_t)s[pos];
            }
            args[0] |= (count << 24);
            (void)wasmos_sys_ipc_send_retry(g_vt_endpoint,
                                            g_vt_client_endpoint,
                                            VT_IPC_WRITE_REQ,
                                            (int32_t)g_vt_switch_generation,
                                            args[0],
                                            args[1],
                                            args[2],
                                            args[3],
                                            CLI_VT_SEND_RETRIES);
        }
    }
    putsn(s, len);
}

/* Ask the VT service for the current active TTY index and its switch generation.
 * Updates g_vt_switch_generation from the response; returns TTY index or -1. */
static int32_t
cli_query_active_tty(uint32_t *out_generation)
{
    if (g_vt_endpoint < 0 || g_vt_client_endpoint < 0) {
        if (out_generation) {
            *out_generation = g_vt_switch_generation;
        }
        return g_home_tty;
    }
    int32_t req_id = g_request_id++;
    if (wasmos_ipc_send(g_vt_endpoint,
                        g_vt_client_endpoint,
                        VT_IPC_GET_ACTIVE_TTY,
                        req_id,
                        0, 0, 0, 0) != 0) {
        return -1;
    }

    for (int tries = 0; tries < CLI_VT_RESP_RETRIES; ++tries) {
        int32_t rc = wasmos_ipc_drain(g_vt_client_endpoint);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != req_id) {
            continue;
        }
        if (resp_type != VT_IPC_RESP) {
            return -1;
        }
        uint32_t gen = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        if (gen != 0) {
            g_vt_switch_generation = gen;
            if (out_generation) {
                *out_generation = gen;
            }
        }
        return wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    }

    return -1;
}

static int
cli_switch_tty(int32_t tty, int wait_resp, int32_t *out_error)
{
    if (out_error) {
        *out_error = 0;
    }
    if (g_vt_endpoint < 0 || g_vt_client_endpoint < 0) {
        if (out_error) {
            *out_error = -1;
        }
        return -1;
    }

    int32_t req_id = wait_resp ? g_request_id++ : 0;
    int32_t send_rc = wasmos_sys_ipc_send_retry(g_vt_endpoint,
                                                g_vt_client_endpoint,
                                                VT_IPC_SWITCH_TTY,
                                                req_id,
                                                tty,
                                                0,
                                                0,
                                                0,
                                                CLI_VT_SEND_RETRIES);
    if (send_rc != 0) {
        if (out_error) {
            *out_error = send_rc;
        }
        return -1;
    }

    if (!wait_resp) {
        return 0;
    }

    for (int tries_resp = 0; tries_resp < CLI_VT_RESP_RETRIES; ++tries_resp) {
        int32_t rc = wasmos_ipc_drain(g_vt_client_endpoint);
        if (rc < 0) {
            if (out_error) {
                *out_error = rc;
            }
            return -1;
        }
        if (rc == 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != req_id) {
            continue;
        }
        if (resp_type == VT_IPC_RESP) {
            uint32_t gen = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            if (gen != 0) {
                g_vt_switch_generation = gen;
            }
            g_last_seen_active_tty = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
            return 0;
        }
        if (out_error) {
            *out_error = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        }
        return -1;
    }
    if (out_error) {
        *out_error = -2;
    }
    return -1;
}

static int
cli_is_foreground(void)
{
    if (g_vt_endpoint < 0 || g_home_tty <= 0) {
        return 1;
    }
    if (g_fg_query_backoff > 0) {
        g_fg_query_backoff--;
        return g_last_seen_active_tty == g_home_tty;
    }
    int32_t active_tty = cli_query_active_tty(NULL);
    if (active_tty >= 0) {
        if (!(g_home_tty == 1 && active_tty == 0)) {
            g_last_seen_active_tty = active_tty;
        }
    }
    g_fg_query_backoff = (g_last_seen_active_tty == g_home_tty) ? 31 : 3;
    /* Do not forcibly reclaim tty1 while tty0 is active; compositor may
     * temporarily own tty0 for graphics presentation. */
    return g_last_seen_active_tty == g_home_tty;
}

static int
cli_register_vt_writer(void)
{
    if (g_vt_endpoint < 0 || g_vt_client_endpoint < 0 || g_home_tty < 0) {
        return -1;
    }
    int32_t req_id = g_request_id++;
    int32_t send_rc = wasmos_sys_ipc_send_retry(g_vt_endpoint,
                                                g_vt_client_endpoint,
                                                VT_IPC_REGISTER_WRITER,
                                                req_id,
                                                g_home_tty,
                                                0,
                                                0,
                                                0,
                                                CLI_VT_SEND_RETRIES);
    if (send_rc != 0) {
        return -1;
    }

    for (int tries = 0; tries < CLI_VT_RESP_RETRIES; ++tries) {
        int32_t rc = wasmos_ipc_drain(g_vt_client_endpoint);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != req_id) {
            continue;
        }
        if (resp_type != VT_IPC_RESP) {
            return -1;
        }
        uint32_t gen = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        if (gen != 0) {
            g_vt_switch_generation = gen;
        }
        return 0;
    }
    return -1;
}

static int
cli_set_vt_mode(uint32_t mode)
{
    if (g_vt_endpoint < 0 || g_vt_client_endpoint < 0) {
        return -1;
    }
    int32_t req_id = g_request_id++;
    if (wasmos_ipc_send(g_vt_endpoint,
                        g_vt_client_endpoint,
                        VT_IPC_SET_MODE_REQ,
                        req_id,
                        (int32_t)mode, 0, 0, 0) != 0) {
        return -1;
    }

    for (int tries = 0; tries < CLI_VT_RESP_RETRIES; ++tries) {
        int32_t rc = wasmos_ipc_drain(g_vt_client_endpoint);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != req_id) {
            continue;
        }
        return (resp_type == VT_IPC_RESP) ? 0 : -1;
    }
    return -1;
}

static int32_t
cli_vt_read_char(char *out_ch)
{
    if (!out_ch) {
        return -1;
    }
    if (g_vt_endpoint < 0 || g_vt_client_endpoint < 0) {
        return -1;
    }

    int32_t req_id = g_request_id++;
    int32_t send_rc = wasmos_sys_ipc_send_retry(g_vt_endpoint,
                                                g_vt_client_endpoint,
                                                VT_IPC_READ_REQ,
                                                req_id,
                                                g_home_tty,
                                                0,
                                                0,
                                                0,
                                                CLI_VT_SEND_RETRIES);
    if (send_rc != 0) {
        return -1;
    }

    for (int wait = 0; wait < 32; ++wait) {
        int32_t rc = wasmos_ipc_drain(g_vt_client_endpoint);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != req_id) {
            continue;
        }
        if (resp_type != VT_IPC_RESP) {
            return -1;
        }
        int32_t status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        if (status != 0) {
            return 0;
        }
        int32_t ch = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        *out_ch = (char)(ch & 0xFF);
        return 1;
    }
    return 0;
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
        int32_t cwd_len = wasmos_sys_strlen(g_cwd);
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
    uint32_t uv = 0;
    if (value < 0) {
        buf[pos++] = '-';
        uv = (uint32_t)(-(int64_t)value);
    } else {
        uv = (uint32_t)value;
    }
    if (uv == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[16];
        int tpos = 0;
        while (uv > 0 && tpos < (int)sizeof(tmp)) {
            tmp[tpos++] = (char)('0' + (uv % 10u));
            uv /= 10u;
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
cli_bytes_equal(const char *a, const char *b, int32_t len)
{
    if (!a || !b || len < 0) {
        return 0;
    }
    for (int32_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void
cli_replace_line(const char *line, int32_t len)
{
    if (!line || len < 0) {
        return;
    }
    if (len > (int32_t)(sizeof(g_line) - 1)) {
        len = (int32_t)(sizeof(g_line) - 1);
    }
    while (g_line_len > 0) {
        g_line_len--;
        console_write("\b \b");
    }
    for (int32_t i = 0; i < len; ++i) {
        g_line[i] = line[i];
        char echo_buf[2] = { line[i], '\0' };
        console_write(echo_buf);
    }
    g_line_len = len;
}

static void
cli_history_store_current(void)
{
    if (g_line_len <= 0) {
        return;
    }
    uint8_t newest = (uint8_t)((g_history_head + CLI_HISTORY_MAX - 1u) % CLI_HISTORY_MAX);
    if (g_history_count > 0 &&
        g_history_len[newest] == (uint8_t)g_line_len &&
        cli_bytes_equal(g_history[newest], g_line, g_line_len)) {
        return;
    }
    uint8_t slot = g_history_head;
    for (int32_t i = 0; i < g_line_len; ++i) {
        g_history[slot][i] = g_line[i];
    }
    g_history[slot][g_line_len] = '\0';
    g_history_len[slot] = (uint8_t)g_line_len;
    g_history_head = (uint8_t)((g_history_head + 1u) % CLI_HISTORY_MAX);
    if (g_history_count < CLI_HISTORY_MAX) {
        g_history_count++;
    }
}

static void
cli_history_reset_nav(void)
{
    g_history_nav = -1;
    g_history_have_scratch = 0;
    g_history_scratch_len = 0;
}

static void
cli_history_nav(int older)
{
    if (g_history_count == 0) {
        return;
    }

    if (older) {
        if (g_history_nav < 0) {
            g_history_scratch_len = g_line_len;
            for (int32_t i = 0; i < g_line_len; ++i) {
                g_history_scratch[i] = g_line[i];
            }
            g_history_scratch[g_line_len] = '\0';
            g_history_have_scratch = 1;
            g_history_nav = 0;
        } else if (g_history_nav + 1 < (int8_t)g_history_count) {
            g_history_nav++;
        }
    } else {
        if (g_history_nav < 0) {
            return;
        }
        if (g_history_nav == 0) {
            if (g_history_have_scratch) {
                cli_replace_line(g_history_scratch, g_history_scratch_len);
            } else {
                cli_replace_line("", 0);
            }
            cli_history_reset_nav();
            return;
        }
        g_history_nav--;
    }

    uint8_t slot = (uint8_t)((g_history_head + CLI_HISTORY_MAX - 1u -
                              (uint8_t)g_history_nav) % CLI_HISTORY_MAX);
    cli_replace_line(g_history[slot], g_history_len[slot]);
}

static void
cli_pack_name(const char *name, uint32_t out[4])
{
    int32_t packed[4] = { 0, 0, 0, 0 };
    if (!out) {
        return;
    }
    wasmos_sys_ipc_pack_name16(name, packed);
    out[0] = (uint32_t)packed[0];
    out[1] = (uint32_t)packed[1];
    out[2] = (uint32_t)packed[2];
    out[3] = (uint32_t)packed[3];
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
    if (wasmos_sys_ipc_send_retry(g_fs_endpoint,
                                  g_reply_endpoint,
                                  type,
                                  req_id,
                                  (int32_t)arg0,
                                  (int32_t)arg1,
                                  (int32_t)arg2,
                                  (int32_t)arg3,
                                  CLI_REQ_SEND_RETRIES) != 0) {
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
    if (wasmos_sys_ipc_send_retry(g_proc_endpoint,
                                  g_reply_endpoint,
                                  type,
                                  req_id,
                                  (int32_t)arg0,
                                  (int32_t)arg1,
                                  (int32_t)arg2,
                                  (int32_t)arg3,
                                  CLI_REQ_SEND_RETRIES) != 0) {
        return -1;
    }
    g_pending_req = req_id;
    return 0;
}

static void
cli_show_mounts(void)
{
    char buf[384];
    int32_t req_id = 0;
    int32_t resp_type = 0;
    int32_t n = 0;
    if (g_fs_endpoint < 0 || g_reply_endpoint < 0) {
        console_write("mount failed\n");
        return;
    }
    req_id = g_request_id++;
    if (wasmos_ipc_send(g_fs_endpoint, g_reply_endpoint, FSMGR_IPC_QUERY_MOUNTS_REQ, req_id, 0, 0, 0, 0) != 0 ||
        wasmos_ipc_select_one(g_reply_endpoint) < 0 ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req_id) {
        console_write("mount failed\n");
        return;
    }
    resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    if (resp_type != FSMGR_IPC_QUERY_MOUNTS_RESP) {
        console_write("mount failed\n");
        return;
    }
    n = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (n <= 0 || n >= (int32_t)sizeof(buf)) {
        console_write("mount failed\n");
        return;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)buf, n, 0) != 0) {
        console_write("mount failed\n");
        return;
    }
    buf[n] = '\0';
    console_write(buf);
}

static void
set_cwd_path(const char *path)
{
    if (!path || !path[0] || wasmos_sys_streq(path, ".")) {
        return;
    }
    if (wasmos_sys_streq(path, "/")) {
        set_cwd_root();
        return;
    }

    char resolved[64];
    uint32_t out = 0;

    resolved[out++] = '/';
    if (path[0] == '/') {
        /* Absolute path starts from root. */
    } else if (!(g_cwd[0] == '/' && g_cwd[1] == '\0')) {
        /* Relative path starts from current cwd. */
        uint32_t i = 0;
        while (g_cwd[i] && out + 1 < sizeof(resolved)) {
            if (i == 0 && g_cwd[i] == '/') {
                i++;
                continue;
            }
            resolved[out++] = g_cwd[i++];
        }
        if (out > 1 && resolved[out - 1] != '/' && out + 1 < sizeof(resolved)) {
            resolved[out++] = '/';
        }
    }

    uint32_t i = 0;
    while (path[i]) {
        while (path[i] == '/') {
            i++;
        }
        if (!path[i]) {
            break;
        }
        uint32_t seg_start = i;
        while (path[i] && path[i] != '/') {
            i++;
        }
        uint32_t seg_len = i - seg_start;
        if (seg_len == 1 && path[seg_start] == '.') {
            continue;
        }
        if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            if (out > 1) {
                if (out > 1 && resolved[out - 1] == '/') {
                    out--;
                }
                while (out > 1 && resolved[out - 1] != '/') {
                    out--;
                }
            }
            continue;
        }
        if (out > 1 && resolved[out - 1] != '/' && out + 1 < sizeof(resolved)) {
            resolved[out++] = '/';
        }
        for (uint32_t j = 0; j < seg_len && out + 1 < sizeof(resolved); ++j) {
            resolved[out++] = path[seg_start + j];
        }
    }

    if (out > 1 && resolved[out - 1] == '/') {
        out--;
    }
    resolved[out] = '\0';

    uint32_t k = 0;
    while (resolved[k] && k + 1 < sizeof(g_cwd)) {
        g_cwd[k] = resolved[k];
        k++;
    }
    g_cwd[k] = '\0';
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
cli_extract_exec_path(const char *input, char *out, uint32_t out_len)
{
    uint32_t start = 0;
    uint32_t end = 0;
    if (!input || !out || out_len == 0) {
        return;
    }
    while (input[start] == ' ' || input[start] == '\t') {
        start++;
    }
    end = start;
    while (input[end] && input[end] != ' ' && input[end] != '\t') {
        end++;
    }
    uint32_t len = 0u;
    for (uint32_t i = start; i < end && len + 1u < out_len; ++i) {
        char c = input[i];
        out[len++] = (c == '\\') ? '/' : c;
    }
    out[len] = '\0';
    cli_trim_name(out);
    if (out[0] == '\0') {
        return;
    }
    if (!str_ends_with(out, ".wap")) {
        uint32_t n = (uint32_t)wasmos_sys_strlen(out);
        if (n + 4u + 1u < out_len) {
            out[n++] = '.';
            out[n++] = 'w';
            out[n++] = 'a';
            out[n++] = 'p';
            out[n] = '\0';
        }
    }
}

static int
cli_resolve_exec_path(const char *input, char *resolved, uint32_t resolved_len)
{
    int n = 0;
    char path[96];
    path[0] = '\0';
    if (!input || !resolved || resolved_len == 0) {
        return -1;
    }
    resolved[0] = '\0';
    cli_extract_exec_path(input, path, sizeof(path));
    if (path[0] == '\0') {
        return -1;
    }
    if (path[0] == '/') {
        n = snprintf(resolved, resolved_len, "%s", path);
        if (n < 0 || (uint32_t)n >= resolved_len) {
            return -1;
        }
    } else if (str_find_char(path, '/') < 0) {
        if (cli_resolve_path_from_pathvar(path, resolved, resolved_len) != 0) {
            char candidate[96];
            FILE *f = 0;
            if (g_cwd[0] == '/' && g_cwd[1] == '\0') {
                n = snprintf(candidate, sizeof(candidate), "/%s", path);
            } else {
                n = snprintf(candidate, sizeof(candidate), "%s/%s", g_cwd, path);
            }
            if (n < 0 || (uint32_t)n >= sizeof(candidate)) {
                return -1;
            }
            f = fopen(candidate, "r");
            if (!f) {
                return -1;
            }
            (void)fclose(f);
            n = snprintf(resolved, resolved_len, "%s", candidate);
            if (n < 0 || (uint32_t)n >= resolved_len) {
                return -1;
            }
        }
    } else if (g_cwd[0] == '/' && g_cwd[1] == '\0') {
        n = snprintf(resolved, resolved_len, "/%s", path);
        if (n < 0 || (uint32_t)n >= resolved_len) {
            return -1;
        }
    } else {
        n = snprintf(resolved, resolved_len, "%s/%s", g_cwd, path);
        if (n < 0 || (uint32_t)n >= resolved_len) {
            return -1;
        }
    }
    {
        uint32_t path_len = (uint32_t)wasmos_sys_strlen(resolved);
        if (path_len == 0u || path_len >= resolved_len) {
            return -1;
        }
    }
    return 0;
}

static int
cli_spawn_exec_path(const char *input, int32_t *out_pid)
{
    char resolved[96];
    uint32_t path_len = 0;
    const char *args = 0;
    uint32_t args_len = 0;
    uint32_t write_off = 0;
    int32_t fs_buf_size = 0;
    uint32_t i = 0;
    if (!input || !out_pid) {
        return -1;
    }
    *out_pid = -1;
    if (cli_resolve_exec_path(input, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    while (input[i] == ' ' || input[i] == '\t') {
        i++;
    }
    while (input[i] && input[i] != ' ' && input[i] != '\t') {
        i++;
    }
    if (input[i] == ' ' || input[i] == '\t') {
        args = &input[i + 1u];
        args_len = (uint32_t)wasmos_sys_strlen(args);
    }
    path_len = (uint32_t)wasmos_sys_strlen(resolved);
    fs_buf_size = wasmos_fs_buffer_size();
    if (path_len == 0u || fs_buf_size <= 0) {
        return -1;
    }
    write_off = path_len + 1u;
    if ((int32_t)path_len >= fs_buf_size ||
        (args_len > 0u && ((int32_t)write_off >= fs_buf_size ||
                           (int32_t)(write_off + args_len) > fs_buf_size))) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)resolved, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    if (args_len > 0u &&
        wasmos_fs_buffer_write((int32_t)(uintptr_t)args, (int32_t)args_len, (int32_t)write_off) != 0) {
        return -1;
    }
    if (cli_send_proc(PROC_IPC_SPAWN_PATH, 0, path_len, args_len, 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_select_one(g_reply_endpoint) < 0) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != g_pending_req) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    *out_pid = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    return (*out_pid > 0) ? 0 : -1;
}

static int
cli_wait_for_pid_exit(int32_t pid, int32_t *out_exit_status)
{
    if (pid <= 0 || !out_exit_status) {
        return -1;
    }
    *out_exit_status = -1;
    if (cli_send_proc(PROC_IPC_WAIT, (uint32_t)pid, 0, 0, 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_select_one(g_reply_endpoint) < 0) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != g_pending_req) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0) != pid) {
        return -1;
    }
    *out_exit_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    return 0;
}

static int
cli_script_on_start(void *user, const char *path)
{
    (void)user;
    int32_t pid = -1;
    if (cli_spawn_exec_path(path, &pid) != 0) {
        return 0;
    }
    return 0;
}

static int
cli_script_on_spawn(void *user, const char *path)
{
    (void)user;
    int32_t pid = -1;
    (void)cli_spawn_exec_path(path, &pid);
    return 0;
}

static int
cli_script_on_exec(void *user, const char *path, const char *args, int32_t *out_exit_code)
{
    (void)user;
    int32_t pid = -1;
    char input[192];
    int pos = 0;
    while (path[pos] && pos + 1 < (int)sizeof(input)) {
        input[pos] = path[pos];
        pos++;
    }
    if (args && args[0]) {
        if (pos + 1 < (int)sizeof(input)) {
            input[pos++] = ' ';
        }
        int ai = 0;
        while (args[ai] && pos + 1 < (int)sizeof(input)) {
            input[pos++] = args[ai++];
        }
    }
    input[pos] = '\0';
    if (cli_spawn_exec_path(input, &pid) != 0) {
        if (out_exit_code) {
            *out_exit_code = -1;
        }
        return 0;
    }
    int32_t exit_status = 0;
    (void)cli_wait_for_pid_exit(pid, &exit_status);
    if (out_exit_code) {
        *out_exit_code = exit_status;
    }
    return 0;
}

static int
cli_script_on_wait_svc(void *user, const char *name)
{
    (void)user;
    for (int i = 0; i < 256; i++) {
        int32_t ep = wasmos_sys_svc_lookup_retry(g_proc_endpoint,
                                                 g_reply_endpoint,
                                                 name,
                                                 g_request_id,
                                                 1);
        g_request_id++;
        if (ep >= 0) {
            return 0;
        }
        wasmos_sched_yield();
    }
    return 0;
}

static void
cli_script_on_echo(void *user, const char *text)
{
    (void)user;
    console_write(text);
    console_write("\n");
}

static void
cli_script_on_echo_ex(void *user, const char *text, int newline)
{
    (void)user;
    console_write(text);
    if (newline) {
        console_write("\n");
    }
}

static int
cli_script_on_export(void *user, const char *name, const char *value)
{
    (void)user;
    (void)name;
    (void)value;
    return 0;
}

static int
cli_script_scope_put(wasmos_script_env_node_t **table, const char *name, const char *value)
{
    wasmos_script_env_node_t *it = 0;
    if (!table || !name || !name[0] || !value || !value[0]) {
        return -1;
    }
    for (it = *table; it; it = it->next) {
        if (wasmos_sys_streq(it->pair.name, name)) {
            (void)snprintf(it->pair.value, sizeof(it->pair.value), "%s", value);
            return 0;
        }
    }
    it = (wasmos_script_env_node_t *)malloc(sizeof(*it));
    if (!it) {
        return -1;
    }
    memset(it, 0, sizeof(*it));
    (void)snprintf(it->pair.name, sizeof(it->pair.name), "%s", name);
    (void)snprintf(it->pair.value, sizeof(it->pair.value), "%s", value);
    it->next = *table;
    *table = it;
    return 0;
}

static void
cli_scope_to_script(wasmos_script_state_t *state, int include_locals)
{
    if (!state) {
        return;
    }
    wasmos_script_state_dispose(state);
    wasmos_script_state_init(state);
    for (cli_env_var_t *it = g_env; it; it = it->next) {
        if (it->is_export) {
            (void)cli_script_scope_put(&state->exports, it->name, it->value);
        } else if (include_locals) {
            (void)cli_script_scope_put(&state->locals, it->name, it->value);
        }
    }
}

static void
cli_scope_from_script(const wasmos_script_state_t *state)
{
    if (!state) {
        return;
    }
    cli_env_clear();
    for (const wasmos_script_env_node_t *it = state->exports; it; it = it->next) {
        (void)cli_env_set(it->pair.name, it->pair.value, 1);
    }
    for (const wasmos_script_env_node_t *it = state->locals; it; it = it->next) {
        (void)cli_env_set(it->pair.name, it->pair.value, 0);
    }
}

static int
cli_run_script(const char *script_path, int source_mode)
{
    if (!script_path || script_path[0] == '\0') {
        return -1;
    }
    cli_scope_to_script(&g_cli_script_state, source_mode ? 1 : 0);
    wasmos_script_ops_t ops;
    ops.on_start    = cli_script_on_start;
    ops.on_spawn    = cli_script_on_spawn;
    ops.on_exec     = cli_script_on_exec;
    ops.on_wait_svc = cli_script_on_wait_svc;
    ops.on_echo     = cli_script_on_echo;
    ops.on_echo_ex  = cli_script_on_echo_ex;
    ops.on_export   = cli_script_on_export;
    ops.user        = 0;
    if (wasmos_script_run(&g_cli_script_state, &ops, script_path) != 0) {
        return -1;
    }
    if (source_mode) {
        cli_scope_from_script(&g_cli_script_state);
    }
    return 0;
}

static int
cli_handle_line(void)
{
    g_line[g_line_len] = '\0';
    if (g_line_len == 0) {
        return 0;
    }
    if (line_eq_ci("help")) {
        console_write("commands: help, kmaps [all], ls, cd <path>, mount, script <file>, source <file>, spawn <cmd>, export VAR=<value>, set VAR=<value>, echo [-n] [-e|-E] [--] [text|${VAR}...], tty <0-3>, halt, reboot\n");
        return 0;
    }
    if (line_eq_ci("mount")) {
        cli_show_mounts();
        return 0;
    }
    if (line_eq_ci("kmaps")) {
        int32_t rc = wasmos_kmap_dump();
        if (rc == 0) {
            console_write("kmaps: dumped\n");
        } else {
            console_write("kmaps: failed\n");
        }
        return 0;
    }
    if (line_eq_ci("kmaps all")) {
        int32_t rc = wasmos_kmap_dump_all();
        if (rc == 0) {
            console_write("kmaps all: dumped\n");
        } else {
            console_write("kmaps all: failed\n");
        }
        return 0;
    }
    if (g_line_len > 4 && line_starts_with_ci("tty ")) {
        int32_t tty = (int32_t)(g_line[4] - '0');
        if (g_line[4] < '0' || g_line[4] > '3' || g_line[5] != '\0') {
            console_write("tty usage: tty <0-3>\n");
            return 0;
        }
        if (g_vt_endpoint < 0 || g_reply_endpoint < 0) {
            console_write("tty switch unavailable\n");
            return 0;
        }
        int32_t sw_err = 0;
        if (cli_switch_tty(tty, 1, &sw_err) != 0) {
            if (sw_err != 0) {
                console_write_num("tty switch failed: ", sw_err);
            } else {
                console_write("tty switch failed\n");
            }
            return 0;
        }
        g_last_seen_active_tty = tty;
        if (tty == 0) {
            console_write("switched to tty0 (system console)\n");
        }
        return 0;
    }
    if (g_line_len > 3 && line_starts_with_ci("cd ")) {
        const char *path = &g_line[3];
        if (wasmos_sys_streq(path, "/")) {
            set_cwd_root();
            if (cli_send_fs(FS_IPC_CHDIR_REQ, 0, 0, 0, 0) != 0) {
                console_write("cd failed\n");
                return 0;
            }
            g_pending_cd_use_path = 0;
            g_pending_kind = PENDING_CD;
            return 1;
        }
        if (path[0] == '/' && wasmos_sys_strlen(path) >= 16) {
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
    if (g_line_len > 7 && line_starts_with_ci("script ")) {
        const char *path = &g_line[7];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        if (*path == '\0') {
            console_write("script failed\n");
            return 0;
        }
        if (cli_run_script(path, 0) != 0) {
            return 0;
        }
        console_write("script ok\n");
        return 0;
    }
    if ((g_line_len > 7 && line_starts_with_ci("source ")) ||
        (g_line_len > 2 && g_line[0] == '.' && g_line[1] == ' ')) {
        const char *path = line_starts_with_ci("source ") ? &g_line[7] : &g_line[2];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        if (*path == '\0') {
            console_write("source failed\n");
            return 0;
        }
        if (cli_run_script(path, 1) != 0) {
            return 0;
        }
        console_write("source ok\n");
        return 0;
    }
    if (g_line_len > 7 && line_starts_with_ci("export ")) {
        if (cli_env_apply_export_line(&g_line[7]) != 0) {
            console_write("export usage: export VAR=<value>\n");
        }
        return 0;
    }
    if (g_line_len > 4 && line_starts_with_ci("set ")) {
        if (cli_env_set_local_line(&g_line[4]) != 0) {
            console_write("set usage: set VAR=<value>\n");
        }
        return 0;
    }
    if (g_line_len > 5 && line_starts_with_ci("echo ")) {
        if (cli_echo_env_expr(&g_line[5]) != 0) {
            console_write("echo usage: echo [-n] [-e|-E] [--] [text|${VAR}...]\n");
        }
        return 0;
    }
    if (line_eq_ci("halt")) {
        wasmos_system_halt();
        return 0;
    }
    if (line_eq_ci("reboot")) {
        wasmos_system_reboot();
        return 0;
    }
    if (line_eq_ci("ls")) {
        if (cli_send_fs(FS_IPC_READDIR_REQ, 0, 0, 0, 0) != 0) {
            console_write("ls failed\n");
            return 0;
        }
        g_pending_kind = PENDING_LIST;
        return 1;
    }
    if (g_line_len > 6 && line_starts_with_ci("spawn ")) {
        const char *spawn_input = &g_line[6];
        char resolved[96];
        uint32_t path_len = 0;
        const char *args = 0;
        uint32_t args_len = 0;
        uint32_t write_off = 0;
        int32_t fs_buf_size = 0;
        uint32_t i = 0;
        char cmd_name[96];
        uint32_t name_len = 0;
        while (spawn_input[name_len] != '\0' && spawn_input[name_len] != ' ' && spawn_input[name_len] != '\t') {
            name_len++;
        }
        if (name_len == 0) {
            return 0;
        }
        if (name_len >= sizeof(cmd_name)) {
            name_len = (uint32_t)(sizeof(cmd_name) - 1);
        }
        for (uint32_t j = 0; j < name_len; ++j) {
            cmd_name[j] = spawn_input[j];
        }
        cmd_name[name_len] = '\0';
        if (cli_resolve_exec_path(cmd_name, resolved, sizeof(resolved)) != 0) {
            char msg[140];
            int n = snprintf(msg, sizeof(msg), "no such command found: %s\n", cmd_name);
            if (n > 0) {
                console_write(msg);
            } else {
                console_write("no such command found\n");
            }
            return 0;
        }
        while (spawn_input[i] == ' ' || spawn_input[i] == '\t') { i++; }
        while (spawn_input[i] && spawn_input[i] != ' ' && spawn_input[i] != '\t') { i++; }
        if (spawn_input[i] == ' ' || spawn_input[i] == '\t') {
            args = &spawn_input[i + 1u];
            args_len = (uint32_t)wasmos_sys_strlen(args);
        }
        path_len = (uint32_t)wasmos_sys_strlen(resolved);
        fs_buf_size = wasmos_fs_buffer_size();
        write_off = path_len + 1u;
        if (path_len == 0u || fs_buf_size <= 0 ||
            (int32_t)path_len >= fs_buf_size ||
            (args_len > 0u && ((int32_t)write_off >= fs_buf_size ||
                               (int32_t)(write_off + args_len) > fs_buf_size))) {
            console_write("spawn failed\n");
            return 0;
        }
        if (wasmos_fs_buffer_write((int32_t)(uintptr_t)resolved, (int32_t)path_len, 0) != 0) {
            console_write("spawn failed\n");
            return 0;
        }
        if (args_len > 0u &&
            wasmos_fs_buffer_write((int32_t)(uintptr_t)args, (int32_t)args_len, (int32_t)write_off) != 0) {
            console_write("spawn failed\n");
            return 0;
        }
        if (cli_send_proc(PROC_IPC_SPAWN_PATH,
                          PROC_SPAWN_PATH_FLAG_DETACH,
                          (int32_t)path_len,
                          (int32_t)args_len,
                          0) != 0) {
            console_write("spawn failed\n");
            return 0;
        }
        g_pending_kind = PENDING_SPAWN;
        return 1;
    }
    {
        uint32_t name_len = 0;
        while (g_line[name_len] != '\0' && g_line[name_len] != ' ' && g_line[name_len] != '\t') {
            name_len++;
        }
        if (name_len == 0) {
            return 0;
        }
        char cmd_name[96];
        if (name_len >= sizeof(cmd_name)) {
            name_len = (uint32_t)(sizeof(cmd_name) - 1);
        }
        for (uint32_t i = 0; i < name_len; ++i) {
            cmd_name[i] = g_line[i];
        }
        cmd_name[name_len] = '\0';

        char resolved[96];
        uint32_t path_len = 0;
        const char *args = 0;
        uint32_t args_len = 0;
        uint32_t write_off = 0;
        int32_t fs_buf_size = 0;
        uint32_t i = 0;
        if (cli_resolve_exec_path(cmd_name, resolved, sizeof(resolved)) != 0) {
            char msg[140];
            int n = snprintf(msg, sizeof(msg), "no such command found: %s\n", cmd_name);
            if (n > 0) {
                console_write(msg);
            } else {
                console_write("no such command found\n");
            }
            return 0;
        }
        while (g_line[i] == ' ' || g_line[i] == '\t') {
            i++;
        }
        while (g_line[i] && g_line[i] != ' ' && g_line[i] != '\t') {
            i++;
        }
        if (g_line[i] == ' ' || g_line[i] == '\t') {
            args = &g_line[i + 1u];
            args_len = (uint32_t)wasmos_sys_strlen(args);
        }
        path_len = (uint32_t)wasmos_sys_strlen(resolved);
        fs_buf_size = wasmos_fs_buffer_size();
        write_off = path_len + 1u;
        if (path_len == 0u || fs_buf_size <= 0 ||
            (int32_t)path_len >= fs_buf_size ||
            (args_len > 0u && ((int32_t)write_off >= fs_buf_size ||
                               (int32_t)(write_off + args_len) > fs_buf_size))) {
            console_write("exec failed\n");
            return 0;
        }
        if (wasmos_fs_buffer_write((int32_t)(uintptr_t)resolved, (int32_t)path_len, 0) != 0) {
            console_write("exec failed\n");
            return 0;
        }
        if (args_len > 0u &&
            wasmos_fs_buffer_write((int32_t)(uintptr_t)args, (int32_t)args_len, (int32_t)write_off) != 0) {
            console_write("exec failed\n");
            return 0;
        }
        if (cli_send_proc(PROC_IPC_SPAWN_PATH,
                          0,
                          (int32_t)path_len,
                          (int32_t)args_len,
                          0) != 0) {
            console_write("exec failed\n");
            return 0;
        }
        g_pending_kind = PENDING_EXEC;
        return 1;
    }
}

static void
cli_fail_and_stall(const char *msg)
{
    g_phase = CLI_PHASE_FAILED;
    console_write(msg);
    wasmos_sys_ipc_recv_loop();
}

static void
cli_phase_init_step(int32_t proc_endpoint, int32_t home_tty_arg)
{
    cli_env_init_defaults();
    set_cwd_root();
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        cli_fail_and_stall("[cli] failed to create reply endpoint\n");
    }
    g_vt_client_endpoint = wasmos_ipc_create_endpoint();
    if (g_vt_client_endpoint < 0) {
        cli_fail_and_stall("[cli] failed to create vt endpoint\n");
    }
    g_proc_endpoint = proc_endpoint;
    g_fs_endpoint = wasmos_sys_svc_lookup_retry(g_proc_endpoint,
                                                g_reply_endpoint,
                                                "fs.vfs",
                                                1,
                                                64);
    if (g_fs_endpoint < 0) {
        g_fs_endpoint = wasmos_sys_svc_lookup_retry(g_proc_endpoint,
                                                    g_reply_endpoint,
                                                    "fs",
                                                    1,
                                                    64);
    }
    g_devmgr_endpoint = wasmos_sys_svc_lookup_retry(g_proc_endpoint,
                                                    g_reply_endpoint,
                                                    "devmgr.query",
                                                    1,
                                                    64);
    g_vt_endpoint = wasmos_sys_svc_lookup_retry(g_proc_endpoint,
                                                g_reply_endpoint,
                                                "vt",
                                                2,
                                                64);
    if (home_tty_arg >= 1 && home_tty_arg <= 3) {
        g_home_tty = home_tty_arg;
    } else {
        g_home_tty = 1;
    }
    g_vt_switch_generation = 1;
    if (g_vt_endpoint >= 0 && cli_register_vt_writer() != 0) {
        console_write("[cli] vt writer register failed; serial fallback\n");
        g_vt_endpoint = -1;
        g_vt_client_endpoint = -1;
    }
    if (g_vt_endpoint >= 0 &&
        cli_set_vt_mode((uint32_t)VT_INPUT_MODE_RAW) != 0) {
        console_write("[cli] vt mode set failed; serial fallback\n");
        g_vt_endpoint = -1;
        g_vt_client_endpoint = -1;
    }
    g_last_seen_active_tty = 0;
    if (g_vt_endpoint >= 0 && g_home_tty == 1) {
        (void)cli_switch_tty(1, 1, 0);
    }
    if (g_home_tty == 1) {
        console_write("WAMOS CLI\ncommands: help, kmaps [all], ls, cd <path>, mount, script <file>, source <file>, spawn <cmd>, export VAR=<value>, set VAR=<value>, echo [-n] [-e|-E] [--] [text|${VAR}...], tty <0-3>, halt, reboot\n");
    }
    wasmos_sys_notify_ready(g_proc_endpoint, g_reply_endpoint);
    g_phase = CLI_PHASE_PROMPT;
}

static void
cli_phase_prompt_step(void)
{
    if (!cli_is_foreground()) {
        (void)wasmos_sched_yield();
        return;
    }
    console_prompt();
    g_line_len = 0;
    g_esc_state = 0;
    cli_history_reset_nav();
    g_phase = CLI_PHASE_READ;
}

static void
cli_phase_read_step(void)
{
    if (!cli_is_foreground()) {
        (void)wasmos_sched_yield();
        return;
    }
    if (g_line_len >= (int32_t)(sizeof(g_line) - 1)) {
        console_write("\n");
        g_line_len = 0;
        g_phase = CLI_PHASE_PROMPT;
        return;
    }
    int32_t have_ch = 0;
    int32_t from_vt = 0;
    char ch = '\0';
    if (g_vt_endpoint >= 0) {
        if (g_vt_read_backoff > 0) {
            g_vt_read_backoff--;
        } else {
            have_ch = cli_vt_read_char(&ch);
            if (have_ch < 0) {
                console_write("[cli] vt read failed; serial fallback\n");
                g_vt_endpoint = -1;
                g_vt_client_endpoint = -1;
                have_ch = 0;
            } else if (have_ch == 0) {
                g_vt_read_backoff = 7;
            } else {
                from_vt = 1;
            }
        }
    }
    if (!have_ch) {
        int32_t rc = wasmos_console_read((int32_t)(uintptr_t)&ch, 1);
        if (rc < 0) {
            cli_fail_and_stall("[cli] console read failed\n");
        }
        if (rc > 0) {
            have_ch = 1;
        }
    }
    if (!have_ch) {
        (void)wasmos_sched_yield();
        return;
    }
    if (from_vt) {
        if (g_esc_state == 1) {
            g_esc_state = (ch == '[') ? 2 : 0;
            return;
        }
        if (g_esc_state == 2) {
            if (ch == 'A') {
                cli_history_nav(1);
            } else if (ch == 'B') {
                cli_history_nav(0);
            }
            g_esc_state = 0;
            return;
        }
        if ((uint8_t)ch == 0x1B) {
            g_esc_state = 1;
            return;
        }
    }
    if (ch == '\r' || ch == '\n') {
        console_write("\n");
        cli_history_store_current();
        cli_history_reset_nav();
        g_phase = cli_handle_line() ? CLI_PHASE_WAIT_IPC : CLI_PHASE_PROMPT;
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        if (g_line_len > 0) {
            g_line_len--;
            console_write("\b \b");
        }
        cli_history_reset_nav();
        return;
    }
    g_line[g_line_len++] = ch;
    cli_history_reset_nav();
    char echo_buf[2] = { ch, '\0' };
    console_write(echo_buf);
}

static void
cli_phase_wait_ipc_step(void)
{
    int32_t recv_rc = wasmos_ipc_select_one(g_reply_endpoint);
    if (recv_rc < 0) {
        cli_fail_and_stall("[cli] ipc recv failed\n");
    }
    int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if ((g_pending_kind == PENDING_LIST || g_pending_kind == PENDING_CAT) &&
        resp_type == FS_IPC_STREAM &&
        resp_req == g_pending_req) {
        int32_t args[4] = {
            wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0),
            wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1),
            wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2),
            wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3)
        };
        char out[5];
        int out_len = 0;
        for (int i = 0; i < 4; ++i) {
            char c = (char)(args[i] & 0xFF);
            if (c == '\0') {
                break;
            }
            out[out_len++] = c;
        }
        out[out_len] = '\0';
        if (out_len > 0) {
            console_write(out);
        }
        return;
    }
    int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (resp_req != g_pending_req) {
        cli_fail_and_stall("[cli] ipc response mismatch\n");
    }
    if (resp_type == PROC_IPC_ERROR) {
        console_write("exec failed\n");
    } else if (resp_type == FS_IPC_ERROR || (resp_type == FS_IPC_RESP && resp_status != 0)) {
        console_write("fs failed\n");
    } else if (resp_type != FS_IPC_RESP && resp_type != PROC_IPC_RESP) {
        cli_fail_and_stall("[cli] ipc response invalid\n");
    } else if (g_pending_kind == PENDING_EXEC && resp_type == PROC_IPC_RESP) {
        int32_t spawned_pid = resp_status;
        int32_t spawn_flags = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
#if WASMOS_TRACE
        (void)printf("spawned pid %u\n", (unsigned)spawned_pid);
#endif
        if (spawned_pid > 0 &&
            (spawn_flags & (WASMOS_SPAWN_FLAG_SERVICE | WASMOS_SPAWN_FLAG_DRIVER))) {
            /* Service/driver: PM already waited for NOTIFY_READY before responding.
             * Process is running in background; $? = 0 signals successful startup. */
            cli_env_set("?", "0", 0);
        } else if (spawned_pid > 0 &&
                   cli_send_proc(PROC_IPC_WAIT, (uint32_t)spawned_pid, 0, 0, 0) == 0) {
            g_pending_exec_pid = spawned_pid;
            g_pending_kind = PENDING_WAIT;
            return;
        } else {
            cli_env_set("?", "-1", 0);
        }
    } else if (g_pending_kind == PENDING_WAIT && resp_type == PROC_IPC_RESP) {
        int32_t exit_code = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        char ec_buf[12];
        snprintf(ec_buf, sizeof(ec_buf), "%d", (int)exit_code);
        cli_env_set("?", ec_buf, 0);
        g_pending_exec_pid = -1;
    } else if (g_pending_kind == PENDING_SPAWN && resp_type == PROC_IPC_RESP) {
        /* detached: process started in background, $? unchanged */
        (void)resp_status;
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
            return;
        }
        g_pending_kind = PENDING_CD;
        return;
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
}

/* Service entry point.  home_tty_arg (arg1) is the TTY index assigned by PM
 * (1-based).  Runs the phase state machine forever; only exits to the idle
 * recv loop on CLI_PHASE_FAILED. */
WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t home_tty_arg,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg2;
    (void)ignored_arg3;
    g_phase = CLI_PHASE_INIT;

    for (;;) {
        if (g_phase == CLI_PHASE_INIT) {
            cli_phase_init_step(proc_endpoint, home_tty_arg);
            continue;
        }

        if (g_phase == CLI_PHASE_PROMPT) {
            cli_phase_prompt_step();
            continue;
        }

        if (g_phase == CLI_PHASE_READ) {
            cli_phase_read_step();
            continue;
        }

        if (g_phase == CLI_PHASE_WAIT_IPC) {
            cli_phase_wait_ipc_step();
            continue;
        }

        console_write("[cli] failed\n");
        wasmos_sys_ipc_recv_loop();
    }
}
