/* device_manager_rules.c - parse device-manager rule config files into state
 * tables; four rule kinds: always_spawn, block_fs, pci_match, acpi_match */
#include "string.h"
#include "wasmos/libsys_string.h"
#include "device_manager_rules.h"

/* Copy one line from text, strip inline comments (respecting quotes),
 * trim trailing whitespace; returns -1 if buf is too small. */
static int copy_rule_line(const char* line, char* out, uint32_t out_len) {
    uint32_t n = 0;
    uint8_t in_quote = 0;
    if (!line || !out || out_len < 2u) {
        return -1;
    }
    while (line[n] && line[n] != '\n' && n + 1u < out_len) {
        out[n] = line[n];
        n++;
    }
    /* A line that does not fit REFUSES. Truncating it does not produce a smaller
     * rule: a rule is a conjunction of matchers, so cutting the tail REMOVES
     * conditions, and a line whose RUN+= survives while its ATTR{partlabel} is
     * cut becomes a rule that spawns a filesystem on every partition present.
     * The parse would succeed, the rule would look written, and the mount would
     * land on whichever volume published first. */
    if (line[n] != '\0' && line[n] != '\n') {
        return -1;
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
    wasmos_sys_trim_right(out);
    return 0;
}

/* Consume the next comma-separated token from *cursor (modifies the buffer
 * in-place by NUL-terminating after the token); respects double-quoted fields.
 * Returns pointer to the token or NULL when exhausted. */
static char* next_csv_token(char** cursor) {
    char* start = 0;
    char* p = 0;
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
    wasmos_sys_trim_right(start);
    return start;
}

/* Match token against "KEY OP \"value\"" and copy the quoted value into out.
 * Returns 0 on match, -1 if the token doesn't start with key+op or the
 * value is not properly quoted / too long. */
static int extract_op_value(const char* token, const char* key, const char* op, char* out,
                            uint32_t out_len) {
    const char* p = token;
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

uint16_t dm_rules_count_active(const char* text) {
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

static int parse_u8_hex(const char* s, uint8_t* out) {
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

static int parse_u16_hex(const char* s, uint16_t* out) {
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

/* One hex nibble, or -1. */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Parse the canonical text form of a GUID into the 16 RAW bytes a GPT stores.
 *
 * GPT writes a GUID MIXED-ENDIAN: the first three fields are little-endian and
 * the last two are byte order as written. So the text
 * `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` is on disk as
 * `28 73 2A C1 1F F8 D2 11 BA 4B 00 A0 C9 3E C9 3B` -- the first three groups
 * reversed, the last two not.
 *
 * Converting HERE, once, at rule-load time is what lets matching be a 16-byte
 * memcmp against the bytes the descriptor carries verbatim. Doing it the other
 * way -- formatting the descriptor's bytes back into text per comparison --
 * would put an encoding step on every match and invite exactly the endianness
 * mistake this function exists to contain. A rule whose GUID is wrong in either
 * direction parses cleanly and then never matches, which is a miserable thing to
 * debug, so the form is validated strictly: 36 characters, hyphens in the four
 * canonical places, hex everywhere else.
 *
 * Returns 0 on success. */
static int parse_guid(const char* s, uint8_t* out) {
    /* Byte i of the output comes from hex pair GUID_ORDER[i] of the text, counted
     * left to right ignoring hyphens. The first three groups are reversed. */
    static const uint8_t GUID_ORDER[16] = {
        3,
        2,
        1,
        0, /* time_low, little-endian */
        5,
        4, /* time_mid, little-endian */
        7,
        6, /* time_hi_and_version, little-endian */
        8,
        9, /* clock_seq, as written */
        10,
        11,
        12,
        13,
        14,
        15 /* node, as written */
    };
    char hex[32];
    uint32_t n = 0;
    uint32_t i = 0;
    if (!s || !out) {
        return -1;
    }
    for (i = 0; s[i] != '\0'; ++i) {
        if (i == 8u || i == 13u || i == 18u || i == 23u) {
            if (s[i] != '-') {
                return -1;
            }
            continue;
        }
        if (i >= 36u || n >= sizeof(hex)) {
            return -1;
        }
        if (hex_nibble(s[i]) < 0) {
            return -1;
        }
        hex[n++] = s[i];
    }
    if (i != 36u || n != 32u) {
        return -1;
    }
    for (i = 0; i < 16u; ++i) {
        const uint32_t src = (uint32_t)GUID_ORDER[i] * 2u;
        out[i] = (uint8_t)((hex_nibble(hex[src]) << 4) | hex_nibble(hex[src + 1u]));
    }
    return 0;
}

/* PARTITION_SCHEME_* from its rule spelling. Returns 0 on success. */
static int parse_scheme(const char* s, uint32_t* out) {
    if (strcmp(s, "none") == 0) {
        *out = (uint32_t)PARTITION_SCHEME_NONE;
        return 0;
    }
    if (strcmp(s, "mbr") == 0) {
        *out = (uint32_t)PARTITION_SCHEME_MBR;
        return 0;
    }
    if (strcmp(s, "gpt") == 0) {
        *out = (uint32_t)PARTITION_SCHEME_GPT;
        return 0;
    }
    return -1;
}

/* FS_TYPE_* from its rule spelling. `unknown` is a legitimate thing to match on:
 * it means the superblock probe recognised nothing, which is how a rule selects
 * a volume no shipped filesystem claims. */
static int parse_fs_type(const char* s, uint32_t* out) {
    if (strcmp(s, "unknown") == 0) {
        *out = (uint32_t)FS_TYPE_UNKNOWN;
        return 0;
    }
    if (strcmp(s, "fat") == 0) {
        *out = (uint32_t)FS_TYPE_FAT;
        return 0;
    }
    if (strcmp(s, "wfs") == 0) {
        *out = (uint32_t)FS_TYPE_WFS;
        return 0;
    }
    return -1;
}

static int parse_u8_dec(const char* s, uint8_t* out) {
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

static int parse_always_spawn_rule_line(const char* line, always_spawn_rule_t* out_rule) {
    char line_buf[256];
    char path[96];
    char* cur = 0;
    char* tok = 0;
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
        tok = (char*)wasmos_sys_trim_left(tok);
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
    str_copy(out_rule->spawn_path, sizeof(out_rule->spawn_path), path);
    return 0;
}

void dm_rules_load_always_spawn(device_manager_state_t* state, const char* text) {
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
        const char* line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = wasmos_sys_trim_left(&text[line_start]);
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

/* Map a rule's DRIVER== value to the backend a publisher reports. The names are
 * the drivers' manifest package names, so a rule spells the backend the same way
 * everything else in the tree names that driver, and no separate taxonomy has to
 * be kept in step. An unrecognised name yields UNKNOWN, which the caller treats
 * as a broken rule rather than a wildcard: a rule naming a backend nobody
 * publishes should be reported, not silently matched against everything. */
static uint8_t block_backend_from_name(const char* name) {
    if (!name) {
        return (uint8_t)BLOCK_BACKEND_UNKNOWN;
    }
    if (strcmp(name, "ata") == 0) {
        return (uint8_t)BLOCK_BACKEND_ATA;
    }
    if (strcmp(name, "virtio-blk") == 0) {
        return (uint8_t)BLOCK_BACKEND_VIRTIO_BLK;
    }
    return (uint8_t)BLOCK_BACKEND_UNKNOWN;
}

static int parse_block_fs_rule_line(const char* line, block_fs_rule_t* out_rule) {
    char line_buf[512];
    char path[96];
    char mount[16];
    uint8_t backend = (uint8_t)BLOCK_BACKEND_UNKNOWN;
    uint8_t unit = 0xFFu;
    char* cur = 0;
    char* tok = 0;
    char sub[32];
    char tmp[64];
    char guid_text[40];
    char label[BLOCK_DESCRIPTOR_LABEL_MAX];
    char name[BLOCK_DESCRIPTOR_ID_MAX];
    char fslabel[VOLUME_DESCRIPTOR_LABEL_MAX];
    char uuid_text[40];
    block_fs_rule_t rule;
    if (!line || !out_rule) {
        return -1;
    }
    memset(&rule, 0, sizeof(rule));
    label[0] = '\0';
    name[0] = '\0';
    fslabel[0] = '\0';
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
        tok = (char*)wasmos_sys_trim_left(tok);
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
        if (extract_op_value(tok, "DRIVER", "==", tmp, sizeof(tmp)) == 0) {
            backend = block_backend_from_name(tmp);
            if (backend == BLOCK_BACKEND_UNKNOWN) {
                return -1; /* a named backend nobody publishes matches nothing */
            }
            continue;
        }
        /* Partition matchers. A malformed value is a rejected RULE, not an
         * ignored attribute: a rule that silently dropped the matcher it was
         * written for would match far more than its author asked, and on the
         * mount path that means a filesystem on the wrong volume. */
        if (extract_op_value(tok, "ATTR{type}", "==", guid_text, sizeof(guid_text)) == 0) {
            if (parse_guid(guid_text, rule.type_guid) != 0) {
                return -1;
            }
            rule.has_type_guid = 1;
            continue;
        }
        if (extract_op_value(tok, "ATTR{partuuid}", "==", guid_text, sizeof(guid_text)) == 0) {
            if (parse_guid(guid_text, rule.part_guid) != 0) {
                return -1;
            }
            rule.has_part_guid = 1;
            continue;
        }
        if (extract_op_value(tok, "ATTR{partlabel}", "==", label, sizeof(label)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "ATTR{name}", "==", name, sizeof(name)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "ATTR{fstype}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_fs_type(tmp, &rule.fs_type) != 0) {
                return -1;
            }
            rule.has_fs_type = 1;
            continue;
        }
        if (extract_op_value(tok, "ATTR{scheme}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_scheme(tmp, &rule.scheme) != 0) {
                return -1;
            }
            rule.has_scheme = 1;
            continue;
        }
        /* Volume matchers. ATTR{label} is the FILESYSTEM's label and is not
         * ATTR{partlabel}: the ESP carries no partition label at all while its
         * FAT boot sector says "QEMU VVFAT". */
        if (extract_op_value(tok, "ATTR{label}", "==", fslabel, sizeof(fslabel)) == 0) {
            continue;
        }
        if (extract_op_value(tok, "ATTR{uuid}", "==", uuid_text, sizeof(uuid_text)) == 0) {
            if (parse_guid(uuid_text, rule.uuid) != 0) {
                return -1;
            }
            rule.has_uuid = 1;
            continue;
        }
    }
    if (path[0] == '\0') {
        return -1;
    }
    if (strcmp(sub, "block") == 0) {
        rule.subsystem = (uint8_t)DEVMGR_BLOCK_SUBSYS_DISK;
    } else if (strcmp(sub, "partition") == 0) {
        rule.subsystem = (uint8_t)DEVMGR_BLOCK_SUBSYS_PARTITION;
    } else if (strcmp(sub, "volume") == 0) {
        rule.subsystem = (uint8_t)DEVMGR_BLOCK_SUBSYS_VOLUME;
    } else {
        return -1;
    }
    /* A volume has no partition table of its own to match on, and the block
     * layer's identity fields do not reach it: a volume is a filesystem, not a
     * table entry. Refused for the same reason a partition matcher on a disk
     * rule is -- the author meant a different subsystem, and a dead rule is
     * indistinguishable from a device that never appeared. */
    if (rule.subsystem == (uint8_t)DEVMGR_BLOCK_SUBSYS_VOLUME &&
        (rule.has_type_guid || rule.has_part_guid || rule.has_scheme || label[0])) {
        return -1;
    }
    /* Conversely, the filesystem matchers belong only to a volume. `fstype` on a
     * block or partition rule matched a descriptor field no publisher ever set
     * (see architecture/37 section 9), so it silently matched nothing. */
    if (rule.subsystem != (uint8_t)DEVMGR_BLOCK_SUBSYS_VOLUME && (fslabel[0] || rule.has_uuid)) {
        return -1;
    }
    /* A partition matcher on a disk rule is a rule that can never fire: a whole
     * disk carries no label, no PARTUUID and no partition type. Refused rather
     * than tolerated, because the author plainly meant SUBSYSTEM=="partition"
     * and a silently dead rule looks exactly like a device fault. */
    if (rule.subsystem == (uint8_t)DEVMGR_BLOCK_SUBSYS_DISK &&
        (rule.has_type_guid || rule.has_part_guid || label[0] || name[0])) {
        return -1;
    }
    if (!mount[0]) {
        str_copy(mount, sizeof(mount), "/");
    }
    rule.active = 1;
    rule.queued = 0;
    rule.spawned = 0;
    rule.backend = backend;
    rule.unit = unit;
    str_copy(rule.partlabel, sizeof(rule.partlabel), label);
    str_copy(rule.device_name, sizeof(rule.device_name), name);
    str_copy(rule.label, sizeof(rule.label), fslabel);
    str_copy(rule.mount, sizeof(rule.mount), mount);
    str_copy(rule.spawn_path, sizeof(rule.spawn_path), path);
    *out_rule = rule;
    return 0;
}

/* TODO: a refused line vanishes silently. Every loader here runs over the whole
 * rule text and ignores lines belonging to another kind, so "failed to parse" is
 * the normal case and cannot be reported as an error -- but a line REFUSED for
 * being malformed or overlong is a defect the author should see. Distinguishing
 * the two means a return code this module can produce without a console, since
 * it is deliberately pure so the host suite can link it standalone. */
void dm_rules_load_block_fs(device_manager_state_t* state, const char* text) {
    uint32_t out_count = 0;
    if (!state || !text) {
        return;
    }
    state->active_rule_spawn_index = -1;
    for (uint32_t i = 0; i < BLOCK_FS_RULE_CAP; ++i) {
        state->block_fs_rules[i].active = 0;
        state->block_fs_rules[i].queued = 0;
        state->block_fs_rules[i].spawned = 0;
    }
    for (int32_t i = 0;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char* line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = wasmos_sys_trim_left(&text[line_start]);
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

static int parse_pci_match_rule_line(const char* line, pci_match_rule_t* out_rule) {
    char line_buf[320];
    char path[96];
    char* cur = 0;
    char* tok = 0;
    char sub[32];
    char tmp[64];
    uint8_t class_code = MATCH_ANY_U8;
    uint8_t bus = MATCH_ANY_U8;
    uint8_t slot = MATCH_ANY_U8;
    uint8_t function = MATCH_ANY_U8;
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
        tok = (char*)wasmos_sys_trim_left(tok);
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
        if (extract_op_value(tok, "ATTR{bus}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u8_hex(tmp, &bus) != 0) {
                return -1;
            }
            continue;
        }
        if (extract_op_value(tok, "ATTR{slot}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u8_hex(tmp, &slot) != 0) {
                return -1;
            }
            continue;
        }
        if (extract_op_value(tok, "ATTR{function}", "==", tmp, sizeof(tmp)) == 0) {
            if (parse_u8_hex(tmp, &function) != 0) {
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
    out_rule->bus = bus;
    out_rule->slot = slot;
    out_rule->function = function;
    out_rule->class_code = class_code;
    out_rule->subclass = subclass;
    out_rule->prog_if = prog_if;
    out_rule->vendor_id = vendor_id;
    out_rule->device_id = device_id;
    out_rule->spawned_device_mask = 0;
    str_copy(out_rule->spawn_path, sizeof(out_rule->spawn_path), path);
    return 0;
}

void dm_rules_load_pci_match(device_manager_state_t* state, const char* text) {
    uint32_t out_count = 0;
    if (!state || !text) {
        return;
    }
    for (uint32_t i = 0; i < PCI_MATCH_RULE_CAP; ++i) {
        state->pci_match_rules[i].active = 0;
        state->pci_match_rules[i].spawned_device_mask = 0;
        /* The cached module descriptor belongs to the rule that occupied this
         * slot, not to the slot. Loading a rule set rebinds every slot, so the
         * cache must be dropped or the next rule inherits the previous rule's
         * declared windows and capability set. */
        state->pci_match_rules[i].meta_valid = 0;
    }
    for (int32_t i = 0;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char* line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = wasmos_sys_trim_left(&text[line_start]);
        if (line[0] && line[0] != '#' && out_count < PCI_MATCH_RULE_CAP) {
            if (parse_pci_match_rule_line(line, &state->pci_match_rules[out_count]) == 0) {
                out_count++;
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    state->pci_match_rule_count = out_count;
}

static int parse_acpi_match_rule_line(const char* line, acpi_match_rule_t* out_rule) {
    char line_buf[256];
    char path[96];
    char* cur = 0;
    char* tok = 0;
    char sub[32];
    char tmp[64];
    uint8_t class_code = MATCH_ANY_U8;
    uint8_t subclass = MATCH_ANY_U8;
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
        tok = (char*)wasmos_sys_trim_left(tok);
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
    }
    if (strcmp(sub, "acpi") != 0 || path[0] == '\0') {
        return -1;
    }
    out_rule->active = 1;
    out_rule->class_code = class_code;
    out_rule->subclass = subclass;
    out_rule->spawned_device_mask = 0;
    str_copy(out_rule->spawn_path, sizeof(out_rule->spawn_path), path);
    return 0;
}

void dm_rules_load_acpi_match(device_manager_state_t* state, const char* text) {
    uint32_t out_count = 0;
    if (!state || !text) {
        return;
    }
    for (uint32_t i = 0; i < ACPI_MATCH_RULE_CAP; ++i) {
        state->acpi_match_rules[i].active = 0;
        state->acpi_match_rules[i].subclass = MATCH_ANY_U8;
        state->acpi_match_rules[i].spawned_device_mask = 0;
    }
    for (int32_t i = 0;;) {
        int32_t line_start = i;
        int32_t line_end = i;
        const char* line = 0;
        while (text[line_end] && text[line_end] != '\n') {
            line_end++;
        }
        line = wasmos_sys_trim_left(&text[line_start]);
        if (line[0] && line[0] != '#' && out_count < ACPI_MATCH_RULE_CAP) {
            if (parse_acpi_match_rule_line(line, &state->acpi_match_rules[out_count]) == 0) {
                out_count++;
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    state->acpi_match_rule_count = out_count;
}
