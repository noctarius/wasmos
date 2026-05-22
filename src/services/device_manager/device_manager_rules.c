#include "string.h"
#include "wasmos/libsys_string.h"
#include "device_manager_rules.h"

static const char *
trim_left(const char *s)
{
    if (!s) {
        return s;
    }
    while (*s && wasmos_sys_is_space(*s)) {
        s++;
    }
    return s;
}

static void
trim_right(char *s)
{
    int32_t i = 0;
    if (!s) {
        return;
    }
    while (s[i] != '\0') {
        i++;
    }
    while (i > 0 && wasmos_sys_is_space(s[i - 1])) {
        s[i - 1] = '\0';
        i--;
    }
}

static int
copy_rule_line(const char *line, char *out, uint32_t out_len)
{
    uint32_t n = 0;
    uint8_t in_quote = 0;
    if (!line || !out || out_len < 2u) {
        return -1;
    }
    while (line[n] && line[n] != '\n' && n + 1u < out_len) {
        out[n] = line[n];
        n++;
    }
    out[n] = '\0';
    for (uint32_t i = 0; out[i] != '\0'; ++i) {
        if (out[i] == '"') {
            in_quote = (uint8_t)!in_quote;
            continue;
        }
        if (!in_quote && out[i] == '#') {
            out[i] = '\0';
            break;
        }
    }
    trim_right(out);
    return 0;
}

static char *
next_csv_token(char **cursor)
{
    char *start = 0;
    char *p = 0;
    uint8_t in_quote = 0;
    if (!cursor || !*cursor) {
        return 0;
    }
    p = *cursor;
    while (*p && wasmos_sys_is_space(*p)) {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return 0;
    }
    start = p;
    while (*p) {
        if (*p == '"') {
            in_quote = (uint8_t)!in_quote;
            p++;
            continue;
        }
        if (!in_quote && *p == ',') {
            *p = '\0';
            p++;
            break;
        }
        p++;
    }
    *cursor = p;
    trim_right(start);
    return start;
}

static int
extract_op_value(const char *token,
                 const char *key,
                 const char *op,
                 char *out,
                 uint32_t out_len)
{
    const char *p = token;
    uint32_t key_len = 0;
    uint32_t op_len = 0;
    uint32_t n = 0;
    if (!token || !key || !op || !out || out_len < 2u) {
        return -1;
    }
    while (key[key_len]) {
        key_len++;
    }
    while (op[op_len]) {
        op_len++;
    }
    for (uint32_t i = 0; i < key_len; ++i) {
        if (p[i] != key[i]) {
            return -1;
        }
    }
    p += key_len;
    for (uint32_t i = 0; i < op_len; ++i) {
        if (p[i] != op[i]) {
            return -1;
        }
    }
    p += op_len;
    while (*p && wasmos_sys_is_space(*p)) {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;
    while (p[n] && p[n] != '"') {
        n++;
    }
    if (p[n] != '"' || n + 1u >= out_len) {
        return -1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = p[i];
    }
    out[n] = '\0';
    return 0;
}

uint16_t
dm_rules_count_active(const char *text)
{
    uint16_t count = 0;
    uint8_t saw_non_space = 0;
    uint8_t line_comment = 0;
    if (!text) {
        return 0;
    }
    for (int32_t i = 0;; ++i) {
        char c = text[i];
        if (c == '\0' || c == '\n') {
            if (saw_non_space && !line_comment) {
                count++;
            }
            saw_non_space = 0;
            line_comment = 0;
            if (c == '\0') {
                break;
            }
            continue;
        }
        if (!saw_non_space && wasmos_sys_is_space(c)) {
            continue;
        }
        if (!saw_non_space && c == '#') {
            line_comment = 1;
            saw_non_space = 1;
            continue;
        }
        if (!saw_non_space) {
            saw_non_space = 1;
        }
    }
    return count;
}

static int
parse_u8_hex(const char *s, uint8_t *out)
{
    uint32_t i = 0;
    uint32_t v = 0;
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
    }
    for (; s[i] != '\0'; ++i) {
        char c = s[i];
        uint8_t n = 0;
        if (c >= '0' && c <= '9') {
            n = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            n = (uint8_t)(10 + c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            n = (uint8_t)(10 + c - 'A');
        } else {
            return -1;
        }
        v = (v << 4) | n;
        if (v > 0xFFu) {
            return -1;
        }
    }
    *out = (uint8_t)v;
    return 0;
}

static int
parse_u16_hex(const char *s, uint16_t *out)
{
    uint32_t i = 0;
    uint32_t v = 0;
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
    }
    for (; s[i] != '\0'; ++i) {
        char c = s[i];
        uint8_t n = 0;
        if (c >= '0' && c <= '9') {
            n = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            n = (uint8_t)(10 + c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            n = (uint8_t)(10 + c - 'A');
        } else {
            return -1;
        }
        v = (v << 4) | n;
        if (v > 0xFFFFu) {
            return -1;
        }
    }
    *out = (uint16_t)v;
    return 0;
}

static int
parse_u8_dec(const char *s, uint8_t *out)
{
    uint32_t v = 0;
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    for (uint32_t i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 255u) {
            return -1;
        }
    }
    *out = (uint8_t)v;
    return 0;
}

static int
parse_always_spawn_rule_line(const char *line, always_spawn_rule_t *out_rule)
{
    char line_buf[256];
    char path[96];
    char *cur = 0;
    char *tok = 0;
    char sub[32];
    if (!line || !out_rule) {
        return -1;
    }
    if (copy_rule_line(line, line_buf, sizeof(line_buf)) != 0) {
        return -1;
    }
    if (line_buf[0] == '\0') {
        return -1;
    }
    path[0] = '\0';
    sub[0] = '\0';
    cur = line_buf;
    while ((tok = next_csv_token(&cur)) != 0) {
        tok = (char *)trim_left(tok);
        if (extract_op_value(tok, "SUBSYSTEM", "==", sub, sizeof(sub)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "RUN", "+=", path, sizeof(path)) == 0) {
            continue;
        }
    }
    if (strcmp(sub, "boot") != 0 || path[0] == '\0') {
        return -1;
    }
    out_rule->active = 1;
    out_rule->queued = 1;
    out_rule->spawned = 0;
    wasmos_sys_strcpy(out_rule->spawn_path, sizeof(out_rule->spawn_path), path);
    return 0;
}

void
dm_rules_load_always_spawn(device_manager_state_t *state, const char *text)
{
    uint32_t out_count = 0;
    if (!state || !text) {
        return;
    }
    for (uint32_t i = 0; i < ALWAYS_SPAWN_RULE_CAP; ++i) {
        state->always_spawn_rules[i].active = 0;
        state->always_spawn_rules[i].queued = 0;
        state->always_spawn_rules[i].spawned = 0;
    }
    for (int32_t i = 0;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char *line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = trim_left(&text[line_start]);
        if (line[0] && line[0] != '#' && out_count < ALWAYS_SPAWN_RULE_CAP) {
            if (parse_always_spawn_rule_line(line, &state->always_spawn_rules[out_count]) == 0) {
                out_count++;
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    state->always_spawn_rule_count = out_count;
}

static int
parse_block_fs_rule_line(const char *line, block_fs_rule_t *out_rule)
{
    char line_buf[256];
    char path[96];
    char mount[16];
    uint8_t unit = 0xFFu;
    char *cur = 0;
    char *tok = 0;
    char sub[32];
    char tmp[64];
    if (!line || !out_rule) {
        return -1;
    }
    if (copy_rule_line(line, line_buf, sizeof(line_buf)) != 0) {
        return -1;
    }
    if (line_buf[0] == '\0') {
        return -1;
    }
    path[0] = '\0';
    mount[0] = '\0';
    sub[0] = '\0';
    cur = line_buf;
    while ((tok = next_csv_token(&cur)) != 0) {
        tok = (char *)trim_left(tok);
        if (extract_op_value(tok, "SUBSYSTEM", "==", sub, sizeof(sub)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "RUN", "+=", path, sizeof(path)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "ENV{MOUNT}", "=", mount, sizeof(mount)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "ATTR{unit}", "==", tmp, sizeof(tmp)) == 0) {
            if (strcmp(tmp, "any") == 0) {
                unit = 0xFFu;
            } else if (parse_u8_dec(tmp, &unit) != 0) {
                return -1;
            }
        }
    }
    if (strcmp(sub, "block") != 0 || path[0] == '\0') {
        return -1;
    }
    if (!mount[0]) {
        wasmos_sys_strcpy(mount, sizeof(mount), "/");
    }
    out_rule->active = 1;
    out_rule->queued = 0;
    out_rule->spawned = 0;
    out_rule->unit = unit;
    wasmos_sys_strcpy(out_rule->mount, sizeof(out_rule->mount), mount);
    wasmos_sys_strcpy(out_rule->spawn_path, sizeof(out_rule->spawn_path), path);
    return 0;
}

void
dm_rules_load_block_fs(device_manager_state_t *state, const char *text)
{
    uint32_t out_count = 0;
    if (!state || !text) {
        return;
    }
    state->active_rule_spawn_index = -1;
    state->boot_mount_ready = 0;
    state->user_mount_ready = 0;
    for (uint32_t i = 0; i < BLOCK_FS_RULE_CAP; ++i) {
        state->block_fs_rules[i].active = 0;
        state->block_fs_rules[i].queued = 0;
        state->block_fs_rules[i].spawned = 0;
    }
    for (int32_t i = 0;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char *line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = trim_left(&text[line_start]);
        if (line[0] && line[0] != '#' && out_count < BLOCK_FS_RULE_CAP) {
            if (parse_block_fs_rule_line(line, &state->block_fs_rules[out_count]) == 0) {
                out_count++;
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    state->block_fs_rule_count = out_count;
}

static int
parse_pci_fb_rule_line(const char *line, pci_fb_rule_t *out_rule)
{
    char line_buf[320];
    char path[96];
    char *cur = 0;
    char *tok = 0;
    char sub[32];
    char tmp[64];
    uint8_t class_code = MATCH_ANY_U8;
    uint8_t subclass = MATCH_ANY_U8;
    uint8_t prog_if = MATCH_ANY_U8;
    uint16_t vendor_id = MATCH_ANY_U16;
    uint16_t device_id = MATCH_ANY_U16;
    if (!line || !out_rule) {
        return -1;
    }
    if (copy_rule_line(line, line_buf, sizeof(line_buf)) != 0) {
        return -1;
    }
    if (line_buf[0] == '\0') {
        return -1;
    }
    path[0] = '\0';
    sub[0] = '\0';
    cur = line_buf;
    while ((tok = next_csv_token(&cur)) != 0) {
        tok = (char *)trim_left(tok);
        if (extract_op_value(tok, "SUBSYSTEM", "==", sub, sizeof(sub)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "RUN", "+=", path, sizeof(path)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "ATTR{class}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u8_hex(tmp, &class_code) != 0) {
                return -1;
            }
            continue;
        }
        if (extract_op_value(tok, "ATTR{subclass}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u8_hex(tmp, &subclass) != 0) {
                return -1;
            }
            continue;
        }
        if (extract_op_value(tok, "ATTR{prog_if}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u8_hex(tmp, &prog_if) != 0) {
                return -1;
            }
            continue;
        }
        if (extract_op_value(tok, "ATTR{vendor}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u16_hex(tmp, &vendor_id) != 0) {
                return -1;
            }
            continue;
        }
        if (extract_op_value(tok, "ATTR{device}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u16_hex(tmp, &device_id) != 0) {
                return -1;
            }
            continue;
        }
    }
    if (strcmp(sub, "pci") != 0 || path[0] == '\0') {
        return -1;
    }
    out_rule->active = 1;
    out_rule->class_code = class_code;
    out_rule->subclass = subclass;
    out_rule->prog_if = prog_if;
    out_rule->vendor_id = vendor_id;
    out_rule->device_id = device_id;
    out_rule->spawned_device_mask = 0;
    wasmos_sys_strcpy(out_rule->spawn_path, sizeof(out_rule->spawn_path), path);
    return 0;
}

void
dm_rules_load_pci_fb(device_manager_state_t *state, const char *text)
{
    uint32_t out_count = 0;
    if (!state || !text) {
        return;
    }
    for (uint32_t i = 0; i < PCI_FB_RULE_CAP; ++i) {
        state->pci_fb_rules[i].active = 0;
        state->pci_fb_rules[i].spawned_device_mask = 0;
    }
    for (int32_t i = 0;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char *line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = trim_left(&text[line_start]);
        if (line[0] && line[0] != '#' && out_count < PCI_FB_RULE_CAP) {
            if (parse_pci_fb_rule_line(line, &state->pci_fb_rules[out_count]) == 0) {
                out_count++;
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    state->pci_fb_rule_count = out_count;
}
