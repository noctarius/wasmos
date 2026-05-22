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

static int
parse_always_spawn_rule_line(const char *line, always_spawn_rule_t *out_rule)
{
    char path[96];
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
    if (!(strncmp(line, "always_spawn", 12) == 0 &&
          (line[12] == '\0' || wasmos_sys_is_space(line[12])))) {
        return -1;
    }
    path[0] = '\0';
    cur = (char *)(line + 12);
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
        }
    }
    if (!path[0]) {
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

static int
parse_hex_u8(const char *s, uint8_t *out)
{
    uint32_t v = 0;
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    for (uint32_t i = 0; s[i] != '\0'; ++i) {
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
parse_hex_u16(const char *s, uint16_t *out)
{
    uint32_t v = 0;
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    for (uint32_t i = 0; s[i] != '\0'; ++i) {
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
parse_pci_fb_rule_line(const char *line, pci_fb_rule_t *out_rule)
{
    char path[96];
    char *cur = 0;
    char line_buf[224];
    uint32_t line_len = 0;
    uint8_t class_code = MATCH_ANY_U8;
    uint8_t subclass = MATCH_ANY_U8;
    uint8_t prog_if = MATCH_ANY_U8;
    uint16_t vendor_id = MATCH_ANY_U16;
    uint16_t device_id = MATCH_ANY_U16;
    if (!line || !out_rule) {
        return -1;
    }
    while (line[line_len] && line[line_len] != '\n' && line_len + 1u < sizeof(line_buf)) {
        line_buf[line_len] = line[line_len];
        line_len++;
    }
    line_buf[line_len] = '\0';
    line = trim_left(line_buf);
    if (!(strncmp(line, "pci_framebuffer", 15) == 0 &&
          (line[15] == '\0' || wasmos_sys_is_space(line[15])))) {
        return -1;
    }
    path[0] = '\0';
    cur = (char *)(line + 15);
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
        } else if (strcmp(tok, "class") == 0) {
            if (parse_hex_u8(eq, &class_code) != 0) return -1;
        } else if (strcmp(tok, "subclass") == 0) {
            if (parse_hex_u8(eq, &subclass) != 0) return -1;
        } else if (strcmp(tok, "prog_if") == 0) {
            if (parse_hex_u8(eq, &prog_if) != 0) return -1;
        } else if (strcmp(tok, "vendor") == 0) {
            if (parse_hex_u16(eq, &vendor_id) != 0) return -1;
        } else if (strcmp(tok, "device") == 0) {
            if (parse_hex_u16(eq, &device_id) != 0) return -1;
        }
    }
    if (!path[0]) {
        return -1;
    }
    out_rule->active = 1;
    out_rule->class_code = class_code;
    out_rule->subclass = subclass;
    out_rule->prog_if = prog_if;
    out_rule->vendor_id = vendor_id;
    out_rule->device_id = device_id;
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
