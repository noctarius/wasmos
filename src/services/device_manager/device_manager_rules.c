#include "string.h"
#include "wasmos/libsys.h"
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

int
dm_rules_extract_spawn_path(const char *text, char *out_path, uint32_t out_len)
{
    int32_t i = 0;
    if (!text || !out_path || out_len == 0) {
        return -1;
    }
    out_path[0] = '\0';
    for (;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char *line = 0;
        const char *key = "spawn_path=";
        uint32_t key_len = 11;
        uint32_t key_i = 0;
        const char *value = 0;
        uint32_t n = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = trim_left(&text[line_start]);
        if (*line && *line != '#') {
            while (key_i < key_len && line[key_i] == key[key_i]) {
                key_i++;
            }
            if (key_i == key_len) {
                value = line + key_len;
                while (value < &text[line_end] && wasmos_sys_is_space(*value)) {
                    value++;
                }
                while (&value[n] < &text[line_end] && value[n] && !wasmos_sys_is_space(value[n])) {
                    n++;
                }
                if (n > 0 && n + 1u < out_len) {
                    for (uint32_t j = 0; j < n; ++j) {
                        out_path[j] = value[j];
                    }
                    out_path[n] = '\0';
                    return 0;
                }
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    return -1;
}

static int
parse_block_fs_rule_line(const char *line, block_fs_rule_t *out_rule)
{
    char path[96];
    char mount[16];
    uint8_t unit = 0xFFu;
    char *cur = 0;
    char line_buf[192];
    uint32_t line_len = 0;
    if (!line || !out_rule) {
        return -1;
    }
    while (line[line_len] && line[line_len] != '\n' && line_len + 1u < sizeof(line_buf)) {
        line_buf[line_len] = line[line_len];
        line_len++;
    }
    line_buf[line_len] = '\0';
    line = trim_left(line_buf);
    if (!(strncmp(line, "block_fs", 8) == 0 && (line[8] == '\0' || wasmos_sys_is_space(line[8])))) {
        return -1;
    }
    path[0] = '\0';
    mount[0] = '\0';
    cur = (char *)(line + 8);
    while (*cur) {
        char *tok = cur;
        char *eq = 0;
        while (*tok && wasmos_sys_is_space(*tok)) {
            tok++;
        }
        if (!*tok) {
            break;
        }
        cur = tok;
        while (*cur && !wasmos_sys_is_space(*cur)) {
            cur++;
        }
        if (*cur) {
            *cur++ = '\0';
        }
        eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        if (strcmp(tok, "spawn_path") == 0) {
            wasmos_sys_strcpy(path, sizeof(path), eq);
        } else if (strcmp(tok, "mount") == 0) {
            wasmos_sys_strcpy(mount, sizeof(mount), eq);
        } else if (strcmp(tok, "unit") == 0) {
            if (strcmp(eq, "any") == 0) {
                unit = 0xFFu;
            } else if (eq[0] >= '0' && eq[0] <= '9' && eq[1] == '\0') {
                unit = (uint8_t)(eq[0] - '0');
            }
        }
    }
    if (!path[0]) {
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
