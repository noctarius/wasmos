/* script.c - .rc script interpreter: if/else/endif plus the start, spawn, exec,
 * wait-svc, export, set, script, source/., and echo commands. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wasmos/api.h"
#include "wasmos/script.h"

void wasmos_script_state_init(wasmos_script_state_t* state) {
    memset(state, 0, sizeof(*state));
}

/* Look up a variable in a linked-list env table; returns value or NULL. */
static const char* script_table_get(const wasmos_script_env_node_t* table, const char* name) {
    const wasmos_script_env_node_t* it = table;
    while (it) {
        if (strcmp(it->pair.name, name) == 0) {
            return it->pair.value;
        }
        it = it->next;
    }
    return 0;
}

/* Set or delete a variable in a linked-list env table using malloc/free.
 * Passing NULL or empty value removes the entry. */
static int script_table_set(wasmos_script_env_node_t** table, const char* name, const char* value) {
    wasmos_script_env_node_t* it = 0;
    wasmos_script_env_node_t* prev = 0;
    if (!name || !name[0]) {
        return -1;
    }
    if (!table) {
        return -1;
    }
    for (it = *table; it; prev = it, it = it->next) {
        if (strcmp(it->pair.name, name) == 0) {
            if (!value || !value[0]) {
                if (prev) {
                    prev->next = it->next;
                } else {
                    *table = it->next;
                }
                free(it);
                return 0;
            }
            (void)snprintf(it->pair.value, sizeof(it->pair.value), "%s", value);
            return 0;
        }
    }
    if (!value || !value[0]) {
        return 0;
    }
    it = (wasmos_script_env_node_t*)malloc(sizeof(*it));
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

static void script_table_dispose(wasmos_script_env_node_t** table) {
    wasmos_script_env_node_t* it = 0;
    if (!table) {
        return;
    }
    it = *table;
    while (it) {
        wasmos_script_env_node_t* next = it->next;
        free(it);
        it = next;
    }
    *table = 0;
}

static int script_table_clone(wasmos_script_env_node_t** out, const wasmos_script_env_node_t* in) {
    wasmos_script_env_node_t* head = 0;
    wasmos_script_env_node_t* tail = 0;
    const wasmos_script_env_node_t* it = in;
    if (!out) {
        return -1;
    }
    *out = 0;
    while (it) {
        wasmos_script_env_node_t* node = (wasmos_script_env_node_t*)malloc(sizeof(*node));
        if (!node) {
            script_table_dispose(&head);
            return -1;
        }
        memset(node, 0, sizeof(*node));
        (void)snprintf(node->pair.name, sizeof(node->pair.name), "%s", it->pair.name);
        (void)snprintf(node->pair.value, sizeof(node->pair.value), "%s", it->pair.value);
        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
        it = it->next;
    }
    *out = head;
    return 0;
}

static const char* script_scope_get(wasmos_script_state_t* state, const char* name) {
    const char* val = script_table_get(state->locals, name);
    if (val) {
        return val;
    }
    return script_table_get(state->exports, name);
}

void wasmos_script_state_init_child(wasmos_script_state_t* child,
                                    const wasmos_script_state_t* parent) {
    if (!child) {
        return;
    }
    memset(child, 0, sizeof(*child));
    if (!parent) {
        return;
    }
    child->last_exit_code = parent->last_exit_code;
    if (script_table_clone(&child->exports, parent->exports) != 0) {
        script_table_dispose(&child->exports);
    }
}

void wasmos_script_state_dispose(wasmos_script_state_t* state) {
    if (!state) {
        return;
    }
    script_table_dispose(&state->locals);
    script_table_dispose(&state->exports);
    state->exec_depth = 0;
    state->total_depth = 0;
}

static int script_expand(wasmos_script_state_t* state, const char* in, char* out, int out_len) {
    int ri = 0;
    int wi = 0;
    if (!in || !out || out_len <= 0) {
        return -1;
    }
    while (in[ri] != '\0') {
        if (in[ri] == '$' && in[ri + 1] == '{') {
            ri += 2;
            char name[WASMOS_SCRIPT_ENV_NAME_MAX];
            int nlen = 0;
            while (in[ri] && in[ri] != '}' && nlen + 1 < WASMOS_SCRIPT_ENV_NAME_MAX) {
                name[nlen++] = in[ri++];
            }
            name[nlen] = '\0';
            if (in[ri] == '}') {
                ri++;
            }
            if (nlen == 1 && name[0] == '?') {
                int32_t code = state->last_exit_code;
                char tmp[16];
                int tpos = 0;
                int negative = 0;
                if (code < 0) {
                    negative = 1;
                    code = -code;
                }
                if (code == 0) {
                    tmp[tpos++] = '0';
                } else {
                    char rev[16];
                    int rpos = 0;
                    int32_t v = code;
                    while (v > 0 && rpos < (int)sizeof(rev)) {
                        rev[rpos++] = (char)('0' + (v % 10));
                        v /= 10;
                    }
                    if (negative && tpos + 1 < (int)sizeof(tmp)) {
                        tmp[tpos++] = '-';
                    }
                    for (int i = rpos - 1; i >= 0 && tpos < (int)sizeof(tmp); i--) {
                        tmp[tpos++] = rev[i];
                    }
                }
                for (int i = 0; i < tpos; i++) {
                    if (wi + 1 >= out_len) {
                        return -1;
                    }
                    out[wi++] = tmp[i];
                }
            } else {
                const char* val = script_scope_get(state, name);
                if (val) {
                    int vi = 0;
                    while (val[vi]) {
                        if (wi + 1 >= out_len) {
                            return -1;
                        }
                        out[wi++] = val[vi++];
                    }
                }
            }
        } else {
            if (wi + 1 >= out_len) {
                return -1;
            }
            out[wi++] = in[ri++];
        }
    }
    out[wi] = '\0';
    return 0;
}

static int echo_append_char(char* out, int32_t out_len, int32_t* io_pos, char c) {
    if (!out || !io_pos || *io_pos < 0 || *io_pos + 1 >= out_len) {
        return -1;
    }
    out[*io_pos] = c;
    (*io_pos)++;
    return 0;
}

int wasmos_script_echo_expand(const char* expr, wasmos_script_echo_resolve_var_fn resolve_var,
                              void* resolve_user, char* out, int32_t out_len, int* out_newline) {
    int32_t i = 0;
    int32_t out_pos = 0;
    int parse_flags = 1;
    int no_newline = 0;
    int escape_mode = 0;
    int wrote_any = 0;
    if (!expr || !out || out_len <= 0 || !out_newline) {
        return -1;
    }

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
        while (expr[i] == ' ' || expr[i] == '\t') {
            i++;
        }
        if (expr[i] == '\0') {
            break;
        }
        if (wrote_any) {
            if (echo_append_char(out, out_len, &out_pos, ' ') != 0) {
                return -1;
            }
        }
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
            if (c == '\\' && !in_single) {
                char next = expr[i + 1];
                char emit = next;
                token_started = 1;
                if (next == '\0') {
                    i++;
                    continue;
                }
                if (escape_mode) {
                    if (next == 'n') {
                        emit = '\n';
                    } else if (next == 't') {
                        emit = '\t';
                    } else if (next == 'r') {
                        emit = '\r';
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
                if (emit != '\0' && echo_append_char(out, out_len, &out_pos, emit) != 0) {
                    return -1;
                }
                i += 2;
                continue;
            }
            if (!in_single && c == '$' && expr[i + 1] == '{') {
                char name[WASMOS_SCRIPT_ENV_NAME_MAX];
                char vbuf[WASMOS_SCRIPT_ENV_VAL_MAX];
                int32_t nlen = 0;
                int32_t vi = 0;
                i += 2;
                while (expr[i] && expr[i] != '}' && nlen + 1 < (int32_t)sizeof(name)) {
                    name[nlen++] = expr[i++];
                }
                name[nlen] = '\0';
                if (expr[i] == '}') {
                    i++;
                    token_started = 1;
                    if (nlen > 0 && resolve_var &&
                        resolve_var(resolve_user, name, nlen, vbuf, (int32_t)sizeof(vbuf)) == 0) {
                        while (vbuf[vi]) {
                            if (echo_append_char(out, out_len, &out_pos, vbuf[vi++]) != 0) {
                                return -1;
                            }
                        }
                    }
                    continue;
                }
                if (echo_append_char(out, out_len, &out_pos, '$') != 0 ||
                    echo_append_char(out, out_len, &out_pos, '{') != 0) {
                    return -1;
                }
                for (int32_t ni = 0; ni < nlen; ++ni) {
                    if (echo_append_char(out, out_len, &out_pos, name[ni]) != 0) {
                        return -1;
                    }
                }
                continue;
            }
            token_started = 1;
            if (echo_append_char(out, out_len, &out_pos, c) != 0) {
                return -1;
            }
            i++;
        }
        if (in_single || in_double) {
            return -1;
        }
        if (token_started) {
            wrote_any = 1;
        }
    }

    out[out_pos] = '\0';
    *out_newline = no_newline ? 0 : 1;
    return 0;
}

static int script_echo_resolve_var(void* user, const char* name, int32_t name_len, char* out,
                                   int32_t out_len) {
    wasmos_script_state_t* state = (wasmos_script_state_t*)user;
    const char* val = 0;
    char name_buf[WASMOS_SCRIPT_ENV_NAME_MAX];
    if (!state || !name || name_len <= 0 || !out || out_len <= 0) {
        return -1;
    }
    if (name_len == 1 && name[0] == '?') {
        (void)snprintf(out, (size_t)out_len, "%d", (int)state->last_exit_code);
        return 0;
    }
    if (name_len >= (int32_t)sizeof(name_buf)) {
        out[0] = '\0';
        return 0;
    }
    memcpy(name_buf, name, (size_t)name_len);
    name_buf[name_len] = '\0';
    val = script_scope_get(state, name_buf);
    if (!val) {
        out[0] = '\0';
        return 0;
    }
    (void)snprintf(out, (size_t)out_len, "%s", val);
    return 0;
}

static int parse_int64(const char* s, int64_t* out) {
    if (!s || !s[0]) {
        return -1;
    }
    int negative = 0;
    int i = 0;
    if (s[i] == '-') {
        negative = 1;
        i++;
    }
    if (s[i] == '\0') {
        return -1;
    }
    int64_t val = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + (s[i] - '0');
        i++;
    }
    if (s[i] != '\0') {
        return -1;
    }
    *out = negative ? -val : val;
    return 0;
}

static int script_eval_condition(wasmos_script_state_t* state, const char* cond_str) {
    char buf[WASMOS_SCRIPT_LINE_MAX];
    int start = 0;
    int negate = 0;

    while (cond_str[start] == ' ' || cond_str[start] == '\t') {
        start++;
    }
    if (cond_str[start] == '!') {
        negate = 1;
        start++;
        while (cond_str[start] == ' ' || cond_str[start] == '\t') {
            start++;
        }
    }

    const char* cond = &cond_str[start];

    if (cond[0] == '-' && cond[1] == 'f' && (cond[2] == ' ' || cond[2] == '\t')) {
        const char* path = &cond[3];
        struct stat st;
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return negate ? 1 : 0;
        }
        int result = (stat(expanded, &st) == 0) && ((st.st_mode & S_IFDIR) == 0);
        return negate ? !result : result;
    }

    if (cond[0] == '-' && cond[1] == 'd' && (cond[2] == ' ' || cond[2] == '\t')) {
        const char* path = &cond[3];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return negate ? 1 : 0;
        }
        struct stat st;
        int result = (stat(expanded, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) ? 1 : 0;
        return negate ? !result : result;
    }

    /* Find operator: scan for 2-char ops first, then 1-char ops */
    const char* op = 0;
    int op_len = 0;
    for (int i = 0; cond[i]; i++) {
        if ((cond[i] == '<' && cond[i + 1] == '=') || (cond[i] == '>' && cond[i + 1] == '=') ||
            (cond[i] == '=' && cond[i + 1] == '=') || (cond[i] == '!' && cond[i + 1] == '=')) {
            op = &cond[i];
            op_len = 2;
            break;
        }
    }
    if (!op) {
        for (int i = 0; cond[i]; i++) {
            if (cond[i] == '<' || cond[i] == '>') {
                op = &cond[i];
                op_len = 1;
                break;
            }
        }
    }
    if (!op) {
        return negate ? 1 : 0;
    }

    int lhs_end = (int)(op - cond);
    while (lhs_end > 0 && (cond[lhs_end - 1] == ' ' || cond[lhs_end - 1] == '\t')) {
        lhs_end--;
    }
    char lhs_raw[WASMOS_SCRIPT_LINE_MAX];
    int j = 0;
    for (int i = 0; i < lhs_end && i < WASMOS_SCRIPT_LINE_MAX - 1; i++) {
        lhs_raw[j++] = cond[i];
    }
    lhs_raw[j] = '\0';

    const char* rhs_start = op + op_len;
    while (*rhs_start == ' ' || *rhs_start == '\t') {
        rhs_start++;
    }
    char rhs_raw[WASMOS_SCRIPT_LINE_MAX];
    j = 0;
    while (rhs_start[j] && j < WASMOS_SCRIPT_LINE_MAX - 1) {
        rhs_raw[j] = rhs_start[j];
        j++;
    }
    rhs_raw[j] = '\0';
    while (j > 0 && (rhs_raw[j - 1] == ' ' || rhs_raw[j - 1] == '\t')) {
        rhs_raw[--j] = '\0';
    }

    char lhs[WASMOS_SCRIPT_LINE_MAX];
    char rhs[WASMOS_SCRIPT_LINE_MAX];
    if (script_expand(state, lhs_raw, lhs, (int)sizeof(lhs)) != 0) {
        return negate ? 1 : 0;
    }
    if (script_expand(state, rhs_raw, rhs, (int)sizeof(rhs)) != 0) {
        return negate ? 1 : 0;
    }

    int64_t lnum = 0, rnum = 0;
    int is_numeric = (parse_int64(lhs, &lnum) == 0 && parse_int64(rhs, &rnum) == 0);
    int result = 0;
    if (is_numeric) {
        if (op_len == 2 && op[0] == '=' && op[1] == '=') {
            result = (lnum == rnum);
        } else if (op_len == 2 && op[0] == '!' && op[1] == '=') {
            result = (lnum != rnum);
        } else if (op_len == 2 && op[0] == '<' && op[1] == '=') {
            result = (lnum <= rnum);
        } else if (op_len == 2 && op[0] == '>' && op[1] == '=') {
            result = (lnum >= rnum);
        } else if (op_len == 1 && op[0] == '<') {
            result = (lnum < rnum);
        } else if (op_len == 1 && op[0] == '>') {
            result = (lnum > rnum);
        }
    } else {
        if (op_len == 2 && op[0] == '=' && op[1] == '=') {
            result = (strcmp(lhs, rhs) == 0);
        } else if (op_len == 2 && op[0] == '!' && op[1] == '=') {
            result = (strcmp(lhs, rhs) != 0);
        }
    }
    (void)buf;
    return negate ? !result : result;
}

static void parse_name_value(const char* line, char* name, int name_cap, char* value, int val_cap) {
    int eq = -1;
    int i = 0;
    name[0] = '\0';
    value[0] = '\0';
    while (line[i] && line[i] != '=') {
        i++;
    }
    if (line[i] != '=') {
        return;
    }
    eq = i;
    int nlen = eq;
    while (nlen > 0 && (line[nlen - 1] == ' ' || line[nlen - 1] == '\t')) {
        nlen--;
    }
    if (nlen <= 0 || nlen >= name_cap) {
        return;
    }
    for (int j = 0; j < nlen; j++) {
        name[j] = line[j];
    }
    name[nlen] = '\0';

    const char* vstart = &line[eq + 1];
    while (*vstart == ' ' || *vstart == '\t') {
        vstart++;
    }
    int vlen = 0;
    while (vstart[vlen]) {
        vlen++;
    }
    while (vlen > 0 && (vstart[vlen - 1] == ' ' || vstart[vlen - 1] == '\t')) {
        vlen--;
    }
    if (vlen >= 2 && vstart[0] == '"' && vstart[vlen - 1] == '"') {
        vstart++;
        vlen -= 2;
    }
    if (vlen < 0) {
        vlen = 0;
    }
    if (vlen >= val_cap) {
        vlen = val_cap - 1;
    }
    for (int j = 0; j < vlen; j++) {
        value[j] = vstart[j];
    }
    value[vlen] = '\0';
}

/* The command a runner is waiting on (wasmos_script_runner_t.pending). */
enum {
    SCRIPT_PENDING_NONE = 0,
    SCRIPT_PENDING_START,
    SCRIPT_PENDING_SPAWN,
    SCRIPT_PENDING_EXEC,
    SCRIPT_PENDING_WAIT_SVC,
    SCRIPT_PENDING_SERVICE,
};

/* What script_exec_line reports besides 0 (done) and -1 (fail the script). */
#define SCRIPT_LINE_PENDING 1

/* Opens `path` as a new top frame. A `script` frame (own_child) runs on a child
 * of `state`; a `source` frame runs on `state` itself. Returns 0, or -1 when the
 * nesting limit is reached or the file cannot be opened. */
static int script_push_frame(wasmos_script_runner_t* runner, wasmos_script_state_t* state,
                             const char* path, int own_child) {
    wasmos_script_frame_t* frame;
    FILE* f;
    if (runner->depth >= WASMOS_SCRIPT_NEST_MAX) {
        return -1;
    }
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    frame = &runner->frames[runner->depth];
    frame->file = f;
    frame->owns_child = own_child ? 1u : 0u;
    if (own_child) {
        wasmos_script_state_init_child(&frame->child, state);
        frame->state = &frame->child;
    } else {
        frame->state = state;
    }
    runner->depth++;
    return 0;
}

/* Closes the top frame. A `script` frame hands its exit code to the state
 * below and frees its child state. */
static void script_pop_frame(wasmos_script_runner_t* runner) {
    wasmos_script_frame_t* frame = &runner->frames[runner->depth - 1];
    fclose(frame->file);
    frame->file = 0;
    runner->depth--;
    if (frame->owns_child) {
        if (runner->depth > 0) {
            runner->frames[runner->depth - 1].state->last_exit_code = frame->child.last_exit_code;
        }
        wasmos_script_state_dispose(&frame->child);
        frame->owns_child = 0;
    }
}

static int script_exec_line(wasmos_script_runner_t* runner, wasmos_script_state_t* state,
                            const char* line) {
    const wasmos_script_ops_t* ops = runner->ops;
    int executing = (state->exec_depth == state->total_depth);

    /* Handle structural keywords regardless of executing state */
    if (line[0] == 'i' && line[1] == 'f' && (line[2] == ' ' || line[2] == '\t')) {
        const char* rest = &line[3];
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        /* Find " then" suffix */
        int rlen = 0;
        while (rest[rlen]) {
            rlen++;
        }
        int has_then = 0;
        if (rlen >= 5) {
            const char* tail = &rest[rlen - 5];
            if (tail[0] == ' ' && tail[1] == 't' && tail[2] == 'h' && tail[3] == 'e' &&
                tail[4] == 'n') {
                has_then = 1;
                rlen -= 5;
                while (rlen > 0 && (rest[rlen - 1] == ' ' || rest[rlen - 1] == '\t')) {
                    rlen--;
                }
            }
        }
        int cond_true = 0;
        if (executing && has_then) {
            char cond_buf[WASMOS_SCRIPT_LINE_MAX];
            int j = 0;
            while (j < rlen && j < WASMOS_SCRIPT_LINE_MAX - 1) {
                cond_buf[j] = rest[j];
                j++;
            }
            cond_buf[j] = '\0';
            cond_true = script_eval_condition(state, cond_buf);
        }
        if (state->total_depth < WASMOS_SCRIPT_IF_DEPTH) {
            state->seen_else[state->total_depth] = 0;
        }
        state->total_depth++;
        if (executing && cond_true) {
            state->exec_depth++;
        }
        return 0;
    }

    if (line[0] == 'e' && line[1] == 'l' && line[2] == 's' && line[3] == 'e' &&
        (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
        if (state->total_depth <= 0) {
            return 0;
        }
        int depth_idx = state->total_depth - 1;
        if (depth_idx < WASMOS_SCRIPT_IF_DEPTH && state->seen_else[depth_idx]) {
            return 0;
        }
        if (depth_idx < WASMOS_SCRIPT_IF_DEPTH) {
            state->seen_else[depth_idx] = 1;
        }
        if (state->exec_depth == state->total_depth) {
            state->exec_depth--;
        } else if (state->exec_depth == state->total_depth - 1) {
            state->exec_depth++;
        }
        return 0;
    }

    if (line[0] == 'e' && line[1] == 'n' && line[2] == 'd' && line[3] == 'i' && line[4] == 'f' &&
        (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
        if (state->total_depth <= 0) {
            return 0;
        }
        if (state->exec_depth == state->total_depth) {
            state->exec_depth--;
        }
        state->total_depth--;
        return 0;
    }

    if (!executing) {
        return 0;
    }

    /* Command dispatch */
    if (line[0] == 's' && line[1] == 't' && line[2] == 'a' && line[3] == 'r' && line[4] == 't' &&
        (line[5] == ' ' || line[5] == '\t')) {
        const char* path = &line[6];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return -1;
        }
        int rc = ops->on_start(ops->user, expanded);
        if (rc == WASMOS_SCRIPT_HOOK_PENDING) {
            runner->pending = SCRIPT_PENDING_START;
            return SCRIPT_LINE_PENDING;
        }
        if (rc != 0) {
            return -1;
        }
        return 0;
    }

    if (line[0] == 's' && line[1] == 'p' && line[2] == 'a' && line[3] == 'w' && line[4] == 'n' &&
        (line[5] == ' ' || line[5] == '\t')) {
        const char* path = &line[6];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return 0;
        }
        if (ops->on_spawn(ops->user, expanded) == WASMOS_SCRIPT_HOOK_PENDING) {
            runner->pending = SCRIPT_PENDING_SPAWN;
            return SCRIPT_LINE_PENDING;
        }
        return 0;
    }

    if (line[0] == 'e' && line[1] == 'x' && line[2] == 'e' && line[3] == 'c' &&
        (line[4] == ' ' || line[4] == '\t')) {
        const char* rest = &line[5];
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        /* Find first space to split path from args */
        int plen = 0;
        while (rest[plen] && rest[plen] != ' ' && rest[plen] != '\t') {
            plen++;
        }
        char path_raw[WASMOS_SCRIPT_LINE_MAX];
        int j = 0;
        while (j < plen && j < WASMOS_SCRIPT_LINE_MAX - 1) {
            path_raw[j] = rest[j];
            j++;
        }
        path_raw[j] = '\0';
        char expanded_path[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path_raw, expanded_path, (int)sizeof(expanded_path)) != 0) {
            return 0;
        }
        const char* args_raw = &rest[plen];
        while (*args_raw == ' ' || *args_raw == '\t') {
            args_raw++;
        }
        char expanded_args[WASMOS_SCRIPT_LINE_MAX];
        expanded_args[0] = '\0';
        if (args_raw[0] != '\0') {
            if (script_expand(state, args_raw, expanded_args, (int)sizeof(expanded_args)) != 0) {
                expanded_args[0] = '\0';
            }
        }
        int32_t exit_code = 0;
        if (ops->on_exec(
                ops->user, expanded_path, expanded_args[0] ? expanded_args : 0, &exit_code) ==
            WASMOS_SCRIPT_HOOK_PENDING) {
            runner->pending = SCRIPT_PENDING_EXEC;
            return SCRIPT_LINE_PENDING;
        }
        state->last_exit_code = exit_code;
        return 0;
    }

    if (line[0] == 'w' && line[1] == 'a' && line[2] == 'i' && line[3] == 't' && line[4] == '-' &&
        line[5] == 's' && line[6] == 'v' && line[7] == 'c' && (line[8] == ' ' || line[8] == '\t')) {
        const char* name = &line[9];
        while (*name == ' ' || *name == '\t') {
            name++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, name, expanded, (int)sizeof(expanded)) != 0) {
            return 0;
        }
        if (ops->on_wait_svc(ops->user, expanded) == WASMOS_SCRIPT_HOOK_PENDING) {
            runner->pending = SCRIPT_PENDING_WAIT_SVC;
            return SCRIPT_LINE_PENDING;
        }
        return 0;
    }

    if (strncmp(line, "service", 7) == 0 && (line[7] == ' ' || line[7] == '\t')) {
        const char* spec = &line[8];
        while (*spec == ' ' || *spec == '\t') {
            spec++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (!ops->on_service || script_expand(state, spec, expanded, (int)sizeof(expanded)) != 0) {
            return 0;
        }
        if (ops->on_service(ops->user, expanded) == WASMOS_SCRIPT_HOOK_PENDING) {
            runner->pending = SCRIPT_PENDING_SERVICE;
            return SCRIPT_LINE_PENDING;
        }
        return 0;
    }

    if (line[0] == 'e' && line[1] == 'x' && line[2] == 'p' && line[3] == 'o' && line[4] == 'r' &&
        line[5] == 't' && (line[6] == ' ' || line[6] == '\t')) {
        const char* rest = &line[7];
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        char name[WASMOS_SCRIPT_ENV_NAME_MAX];
        char value_raw[WASMOS_SCRIPT_ENV_VAL_MAX];
        parse_name_value(rest, name, (int)sizeof(name), value_raw, (int)sizeof(value_raw));
        if (name[0] == '\0') {
            return 0;
        }
        char value[WASMOS_SCRIPT_ENV_VAL_MAX];
        if (script_expand(state, value_raw, value, (int)sizeof(value)) != 0) {
            return 0;
        }
        (void)script_table_set(&state->exports, name, value);
        if (ops->on_export) {
            ops->on_export(ops->user, name, value);
        }
        return 0;
    }

    if (line[0] == 's' && line[1] == 'e' && line[2] == 't' && (line[3] == ' ' || line[3] == '\t')) {
        const char* rest = &line[4];
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        char name[WASMOS_SCRIPT_ENV_NAME_MAX];
        char value_raw[WASMOS_SCRIPT_ENV_VAL_MAX];
        parse_name_value(rest, name, (int)sizeof(name), value_raw, (int)sizeof(value_raw));
        if (name[0] == '\0') {
            return 0;
        }
        char value[WASMOS_SCRIPT_ENV_VAL_MAX];
        if (script_expand(state, value_raw, value, (int)sizeof(value)) != 0) {
            return 0;
        }
        (void)script_table_set(&state->locals, name, value);
        return 0;
    }

    if (line[0] == 's' && line[1] == 'c' && line[2] == 'r' && line[3] == 'i' && line[4] == 'p' &&
        line[5] == 't' && (line[6] == ' ' || line[6] == '\t')) {
        const char* path = &line[7];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return -1;
        }
        if (script_push_frame(runner, state, expanded, 1) != 0) {
            state->last_exit_code = -1;
            return -1;
        }
        return 0;
    }

    if (line[0] == 's' && line[1] == 'o' && line[2] == 'u' && line[3] == 'r' && line[4] == 'c' &&
        line[5] == 'e' && (line[6] == ' ' || line[6] == '\t')) {
        const char* path = &line[7];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return -1;
        }
        if (script_push_frame(runner, state, expanded, 0) != 0) {
            state->last_exit_code = -1;
            return -1;
        }
        return 0;
    }

    if (line[0] == '.' && (line[1] == ' ' || line[1] == '\t')) {
        const char* path = &line[2];
        while (*path == ' ' || *path == '\t') {
            path++;
        }
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        if (script_expand(state, path, expanded, (int)sizeof(expanded)) != 0) {
            return -1;
        }
        if (script_push_frame(runner, state, expanded, 0) != 0) {
            state->last_exit_code = -1;
            return -1;
        }
        return 0;
    }

    if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' && line[3] == 'o' &&
        (line[4] == ' ' || line[4] == '\t')) {
        const char* text = &line[5];
        char expanded[WASMOS_SCRIPT_LINE_MAX];
        int newline = 1;
        if (wasmos_script_echo_expand(text,
                                      script_echo_resolve_var,
                                      state,
                                      expanded,
                                      (int32_t)sizeof(expanded),
                                      &newline) != 0) {
            return 0;
        }
        if (ops->on_echo_ex) {
            ops->on_echo_ex(ops->user, expanded, newline);
        } else if (ops->on_echo) {
            ops->on_echo(ops->user, expanded);
        }
        return 0;
    }

    return 0;
}

int wasmos_script_runner_start(wasmos_script_runner_t* runner, wasmos_script_state_t* state,
                               const wasmos_script_ops_t* ops, const char* path) {
    if (!runner || !state || !ops || !path) {
        return -1;
    }
    memset(runner, 0, sizeof(*runner));
    runner->ops = ops;
    return script_push_frame(runner, state, path, 0);
}

void wasmos_script_runner_abort(wasmos_script_runner_t* runner) {
    if (!runner) {
        return;
    }
    while (runner->depth > 0) {
        script_pop_frame(runner);
    }
    runner->pending = SCRIPT_PENDING_NONE;
    runner->failed = 0;
}

void wasmos_script_runner_complete(wasmos_script_runner_t* runner, int32_t rc, int32_t exit_code) {
    if (!runner || runner->pending == SCRIPT_PENDING_NONE || runner->depth <= 0) {
        return;
    }
    switch (runner->pending) {
    case SCRIPT_PENDING_START:
        if (rc != 0) {
            runner->failed = 1;
        }
        break;
    case SCRIPT_PENDING_EXEC:
        runner->frames[runner->depth - 1].state->last_exit_code = exit_code;
        break;
    default:
        /* spawn, wait-svc and service results do not affect the script, as in
         * the synchronous path. */
        break;
    }
    runner->pending = SCRIPT_PENDING_NONE;
}

/* Reads lines from the top frame until one runs, popping frames that end. A
 * frame's end is a step of its own (MORE) unless it was the last. */
int wasmos_script_runner_step(wasmos_script_runner_t* runner) {
    char line[WASMOS_SCRIPT_LINE_MAX];
    if (!runner) {
        return WASMOS_SCRIPT_STEP_ERROR;
    }
    if (runner->failed) {
        wasmos_script_runner_abort(runner);
        return WASMOS_SCRIPT_STEP_ERROR;
    }
    if (runner->pending != SCRIPT_PENDING_NONE) {
        return WASMOS_SCRIPT_STEP_PENDING;
    }
    for (;;) {
        wasmos_script_frame_t* frame;
        if (runner->depth == 0) {
            return WASMOS_SCRIPT_STEP_DONE;
        }
        frame = &runner->frames[runner->depth - 1];
        if (!fgets(line, (int)sizeof(line), frame->file)) {
            /* TODO: an `if` left unclosed at end of file is neither reported nor
             * reset, so a `source`d script leaks its open depth into the parent. */
            script_pop_frame(runner);
            return runner->depth == 0 ? WASMOS_SCRIPT_STEP_DONE : WASMOS_SCRIPT_STEP_MORE;
        }
        /* Trim leading whitespace */
        int start = 0;
        while (line[start] == ' ' || line[start] == '\t') {
            start++;
        }
        /* Find end, strip newline and trailing whitespace */
        int end = start;
        while (line[end] && line[end] != '\n' && line[end] != '\r') {
            end++;
        }
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
            end--;
        }
        line[end] = '\0';
        /* Skip empty lines and comments */
        if (line[start] == '\0' || line[start] == '#') {
            continue;
        }
        int rc = script_exec_line(runner, frame->state, &line[start]);
        if (rc == -1) {
            wasmos_script_runner_abort(runner);
            return WASMOS_SCRIPT_STEP_ERROR;
        }
        return rc == SCRIPT_LINE_PENDING ? WASMOS_SCRIPT_STEP_PENDING : WASMOS_SCRIPT_STEP_MORE;
    }
}

int wasmos_script_run(wasmos_script_state_t* state, const wasmos_script_ops_t* ops,
                      const char* path) {
    wasmos_script_runner_t runner;
    if (wasmos_script_runner_start(&runner, state, ops, path) != 0) {
        return -1;
    }
    for (;;) {
        int step = wasmos_script_runner_step(&runner);
        if (step == WASMOS_SCRIPT_STEP_DONE) {
            return 0;
        }
        if (step == WASMOS_SCRIPT_STEP_ERROR) {
            return -1;
        }
        if (step == WASMOS_SCRIPT_STEP_PENDING) {
            /* Nothing on this path can complete a pending command. */
            wasmos_script_runner_abort(&runner);
            return -1;
        }
    }
}
