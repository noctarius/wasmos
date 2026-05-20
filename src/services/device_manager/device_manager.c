#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"

/*
 * device-manager coordinates early hardware startup in user space.
 * In this slice, a separate pci-bus service performs enumeration and publishes
 * records over IPC; device-manager consumes those records and selects storage
 * bootstrap (ata/fs-fat) before post-FAT drivers.
 */

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

typedef enum {
    HW_PHASE_INIT = 0,
    HW_PHASE_SPAWN,
    HW_PHASE_WAIT,
    HW_PHASE_WAIT_INVENTORY,
    HW_PHASE_IDLE,
    HW_PHASE_FAILED
} hw_phase_t;

typedef enum {
    HW_SPAWN_NONE = 0,
    HW_SPAWN_RULE_PATH,
    HW_SPAWN_PCI_BUS,
    HW_SPAWN_FAT,
    HW_SPAWN_FS_INIT,
    HW_SPAWN_FS_MANAGER,
    HW_SPAWN_SERIAL,
    HW_SPAWN_KEYBOARD,
    HW_SPAWN_FRAMEBUFFER
} hw_spawn_target_t;

#define DEVICE_REGISTRY_CAP 64
#define BLOCK_REGISTRY_CAP 16
#define MATCH_ANY_U8 0xFFu
#define MATCH_ANY_U16 0xFFFFu
#define DEVMGR_RULES_INIT_ROOT "/init/devmgr/rules"
#define DEVMGR_RULES_BOOT_ROOT "/boot/system/devmgr/rules"
#define DEVMGR_RULE_FILE "default.rules"
#define DEVMGR_RULE_TEXT_CAP 1024
#define BLOCK_FS_RULE_CAP 8

typedef struct {
    uint32_t cap_flags;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
} spawn_caps_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t mmio_hint;
    uint8_t irq_hint;
} pci_device_record_t;

typedef struct {
    uint8_t in_use;
    uint8_t unit;
    uint8_t present;
    uint8_t active_service;
    uint32_t sector_count;
    char canonical_id[64];
    char hash_id[17];
} block_device_record_t;

typedef struct {
    uint8_t active;
    uint8_t queued;
    uint8_t spawned;
    uint8_t unit;
    char mount[16];
    char spawn_path[96];
} block_fs_rule_t;

typedef struct {
    hw_phase_t phase;
    hw_spawn_target_t pending;
    int32_t reply_endpoint;
    int32_t proc_endpoint;
    int32_t inventory_endpoint;
    int32_t query_endpoint;
    int32_t rule_reply_endpoint;
    int32_t fs_endpoint;
    int32_t request_id;
    int32_t module_count;
    uint8_t need_pci_bus;
    uint8_t need_fat;
    uint8_t need_serial;
    uint8_t need_fs_init;
    uint8_t need_fs_manager;
    uint8_t need_keyboard;
    uint8_t need_framebuffer;
    uint8_t fat_retries;
    uint8_t serial_retries;
    uint8_t fs_init_retries;
    uint8_t keyboard_retries;
    uint8_t framebuffer_retries;
    int32_t pci_bus_index;
    int32_t fat_index;
    int32_t serial_index;
    int32_t fs_init_index;
    int32_t fs_manager_index;
    int32_t keyboard_index;
    int32_t framebuffer_index;
    pci_device_record_t registry[DEVICE_REGISTRY_CAP];
    uint32_t registry_count;
    block_device_record_t block_registry[BLOCK_REGISTRY_CAP];
    uint32_t block_registry_count;
    int32_t selected_storage_index;
    spawn_caps_t selected_storage_caps;
    spawn_caps_t selected_serial_caps;
    uint8_t selected_storage_has_record;
    pci_device_record_t selected_storage_record;
    uint8_t rules_roots_logged;
    uint8_t idle_logged;
    uint8_t rules_init_fail_logged;
    uint8_t rules_init_loaded;
    uint8_t rules_boot_loaded;
    uint8_t rules_boot_request_pending;
    int32_t rules_boot_request_id;
    uint16_t rules_init_active;
    uint16_t rules_boot_active;
    uint8_t rule_spawn_pending;
    uint8_t rule_spawn_retries;
    char rule_spawn_path[96];
    int32_t active_rule_spawn_index;
    block_fs_rule_t block_fs_rules[BLOCK_FS_RULE_CAP];
    uint32_t block_fs_rule_count;
    uint8_t boot_mount_ready;
    uint8_t user_mount_ready;
} device_manager_state_t;

static device_manager_state_t g_dm = {
    .phase = HW_PHASE_INIT,
    .pending = HW_SPAWN_NONE,
    .reply_endpoint = -1,
    .proc_endpoint = -1,
    .inventory_endpoint = -1,
    .query_endpoint = -1,
    .rule_reply_endpoint = -1,
    .fs_endpoint = -1,
    .request_id = 1,
    .module_count = 0,
    .selected_storage_index = -1,
    .pci_bus_index = -1,
    .fat_index = -1,
    .serial_index = -1,
    .fs_init_index = -1,
    .fs_manager_index = -1,
    .keyboard_index = -1,
    .framebuffer_index = -1,
    .rules_roots_logged = 0,
    .idle_logged = 0,
    .rules_init_fail_logged = 0,
    .rules_init_loaded = 0,
    .rules_boot_loaded = 0,
    .rules_boot_request_pending = 0,
    .rules_boot_request_id = -1,
    .rules_init_active = 0,
    .rules_boot_active = 0,
    .rule_spawn_pending = 0,
    .rule_spawn_retries = 0,
    .rule_spawn_path = {0},
    .active_rule_spawn_index = -1,
    .block_fs_rules = {0},
    .block_fs_rule_count = 0,
    .boot_mount_ready = 0,
    .user_mount_ready = 0,
};

static void
console_write(const char *s)
{
    if (!s) {
        return;
    }
    if (s[0] == '\0') {
        return;
    }
    (void)printf("%s", s);
}

static void
hex16_from_sha256(const char *in, char out[17])
{
    wasmos_sha256_hex16_prefix(in, out);
}

static void
log_rule_roots_once(void)
{
    if (g_dm.rules_roots_logged) {
        return;
    }
    g_dm.rules_roots_logged = 1;
    console_write("[device-manager] rule roots: " DEVMGR_RULES_INIT_ROOT " (bootstrap), "
                  DEVMGR_RULES_BOOT_ROOT " (override)\n");
    /* TODO: rule actions are still informational; wire parsed rules into
     * runtime bind/unbind/mount policy decisions in the next slice. */
}

static int proc_running(const char *name);
static int ensure_fs_endpoint(void);
static int query_module_meta_by_path(const char *path, uint32_t source, int32_t *out_index);
static void queue_block_fs_rule_spawns(void);
static void queue_block_fs_rules_for_known_devices(void);
static int module_index_by_name(const char *name);

static uint16_t
count_active_rules(const char *text)
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

static int
extract_rule_spawn_path(const char *text, char *out_path, uint32_t out_len)
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

static void
load_block_fs_rules(const char *text)
{
    uint32_t out_count = 0;
    if (!text) {
        return;
    }
    g_dm.active_rule_spawn_index = -1;
    g_dm.boot_mount_ready = 0;
    g_dm.user_mount_ready = 0;
    for (uint32_t i = 0; i < BLOCK_FS_RULE_CAP; ++i) {
        g_dm.block_fs_rules[i].active = 0;
        g_dm.block_fs_rules[i].queued = 0;
        g_dm.block_fs_rules[i].spawned = 0;
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
            if (parse_block_fs_rule_line(line, &g_dm.block_fs_rules[out_count]) == 0) {
                out_count++;
            }
        }
        if (text[line_end] == '\0') {
            break;
        }
        i = line_end + 1;
    }
    g_dm.block_fs_rule_count = out_count;
}

static int
read_rules_file(const char *path, char *out_text, uint32_t out_text_len)
{
    int32_t read_len = -1;
    if (!path || !out_text || out_text_len < 2u) {
        return -1;
    }
    if (ensure_fs_endpoint() != 0) {
        return -1;
    }
    read_len = wasmos_sys_fs_read_path(g_dm.fs_endpoint,
                                       g_dm.reply_endpoint,
                                       g_dm.request_id++,
                                       path,
                                       out_text,
                                       (int32_t)out_text_len);
    if (read_len < 0) {
        return -1;
    }
    return read_len;
}

static int
ensure_fs_endpoint(void)
{
    if (g_dm.fs_endpoint >= 0) {
        return 0;
    }
    g_dm.fs_endpoint = wasmos_svc_lookup(g_dm.proc_endpoint, g_dm.reply_endpoint, "fs.vfs", 1);
    if (g_dm.fs_endpoint < 0) {
        g_dm.fs_endpoint = wasmos_svc_lookup(g_dm.proc_endpoint, g_dm.reply_endpoint, "fs", 1);
    }
    return (g_dm.fs_endpoint >= 0) ? 0 : -1;
}

static void
kick_boot_rules_read_async(void)
{
    char path[96];
    int32_t path_len = 0;
    if (g_dm.rules_boot_loaded || g_dm.rules_boot_request_pending) {
        return;
    }
    if (!proc_running("fs-fat")) {
        return;
    }
    if (ensure_fs_endpoint() != 0) {
        return;
    }
    path[0] = '\0';
    wasmos_sys_strcpy(path, sizeof(path), DEVMGR_RULES_BOOT_ROOT);
    wasmos_sys_str_append(path, sizeof(path), "/");
    wasmos_sys_str_append(path, sizeof(path), DEVMGR_RULE_FILE);
    path_len = wasmos_sys_strlen(path);
    if (path_len <= 0 || path_len > 95 || path_len >= wasmos_fs_buffer_size()) {
        g_dm.rules_boot_loaded = 1;
        g_dm.rules_boot_active = 0;
        console_write("[device-manager] boot rules invalid path; skipping\n");
        return;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, path_len, 0) != 0) {
        g_dm.rules_boot_loaded = 1;
        g_dm.rules_boot_active = 0;
        console_write("[device-manager] boot rules buffer write failed; skipping\n");
        return;
    }
    g_dm.rules_boot_request_id = g_dm.request_id++;
    if (wasmos_ipc_send(g_dm.fs_endpoint,
                        g_dm.rule_reply_endpoint,
                        FS_IPC_READ_PATH_REQ,
                        g_dm.rules_boot_request_id,
                        path_len,
                        0,
                        0,
                        0) != 0) {
        g_dm.rules_boot_loaded = 1;
        g_dm.rules_boot_active = 0;
        console_write("[device-manager] boot rules request send failed; skipping\n");
        return;
    }
    g_dm.rules_boot_request_pending = 1;
}

static void
poll_boot_rules_async(void)
{
    int32_t recv_rc = 0;
    int32_t resp_req = 0;
    int32_t resp_type = 0;
    int32_t read_len = 0;
    char text[DEVMGR_RULE_TEXT_CAP];
    char msg[128];
    if (!g_dm.rules_boot_request_pending) {
        return;
    }
    recv_rc = wasmos_ipc_try_recv(g_dm.rule_reply_endpoint);
    if (recv_rc <= 0) {
        return;
    }
    resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if (resp_req != g_dm.rules_boot_request_id) {
        return;
    }
    g_dm.rules_boot_request_pending = 0;
    resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    if (resp_type != FS_IPC_RESP) {
        g_dm.rules_boot_loaded = 1;
        g_dm.rules_boot_active = 0;
        console_write("[device-manager] boot rules unavailable; skipping\n");
        return;
    }
    read_len = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (read_len < 0) {
        g_dm.rules_boot_loaded = 1;
        g_dm.rules_boot_active = 0;
        console_write("[device-manager] boot rules read length invalid; skipping\n");
        return;
    }
    if (read_len >= (int32_t)sizeof(text)) {
        read_len = (int32_t)sizeof(text) - 1;
    }
    if (read_len > 0 && wasmos_fs_buffer_copy((int32_t)(uintptr_t)text, read_len, 0) != 0) {
        g_dm.rules_boot_loaded = 1;
        g_dm.rules_boot_active = 0;
        console_write("[device-manager] boot rules copy failed; skipping\n");
        return;
    }
    text[read_len] = '\0';
    g_dm.rules_boot_active = count_active_rules(text);
    if (extract_rule_spawn_path(text, g_dm.rule_spawn_path, sizeof(g_dm.rule_spawn_path)) == 0) {
        g_dm.rule_spawn_pending = 1;
        console_write("[device-manager] boot rules queued spawn_path\n");
    }
    load_block_fs_rules(text);
    if (g_dm.block_fs_rule_count > 0) {
        console_write("[device-manager] boot rules loaded block_fs\n");
        queue_block_fs_rules_for_known_devices();
    }
    g_dm.rules_boot_loaded = 1;
    snprintf(msg, sizeof(msg), "[device-manager] loaded boot rules: %d active\n", (int)g_dm.rules_boot_active);
    console_write(msg);
}

static void
load_rules_if_available(void)
{
    if (!g_dm.rules_init_loaded) {
        char path[96];
        char text[DEVMGR_RULE_TEXT_CAP];
        int32_t read_len = -1;
        path[0] = '\0';
        wasmos_sys_strcpy(path, sizeof(path), DEVMGR_RULES_INIT_ROOT);
        wasmos_sys_str_append(path, sizeof(path), "/");
        wasmos_sys_str_append(path, sizeof(path), DEVMGR_RULE_FILE);
        read_len = read_rules_file(path, text, sizeof(text));
        if (read_len >= 0) {
            g_dm.rules_init_loaded = 1;
            g_dm.rules_init_active = count_active_rules(text);
            if (extract_rule_spawn_path(text, g_dm.rule_spawn_path, sizeof(g_dm.rule_spawn_path)) == 0) {
                g_dm.rule_spawn_pending = 1;
                console_write("[device-manager] init rules queued spawn_path\n");
            }
            load_block_fs_rules(text);
            if (g_dm.block_fs_rule_count > 0) {
                console_write("[device-manager] init rules loaded block_fs\n");
                queue_block_fs_rules_for_known_devices();
            }
        } else if (!g_dm.rules_init_fail_logged) {
            g_dm.rules_init_fail_logged = 1;
            console_write("[device-manager] init rules read failed (will retry)\n");
        }
    }
    kick_boot_rules_read_async();
    poll_boot_rules_async();
}

static int
proc_running(const char *name)
{
    int32_t count = wasmos_proc_count();
    if (count <= 0) {
        return 0;
    }
    if (count > 64) {
        count = 64;
    }
    for (int32_t i = 0; i < count; ++i) {
        char buf[32];
        buf[0] = '\0';
        int32_t pid = wasmos_proc_info(i, (int32_t)(uintptr_t)buf, (int32_t)sizeof(buf));
        if (pid <= 0) {
            continue;
        }
        if (wasmos_sys_streq(buf, name)) {
            return 1;
        }
    }
    return 0;
}

static int
module_index_by_name(const char *name)
{
    int32_t scan_limit = g_dm.module_count;
    if (!name) {
        return -1;
    }
    if (scan_limit <= 0) {
        /* Fallback when module.count startup binding is unavailable/malformed. */
        scan_limit = 64;
    }
    for (int32_t i = 0; i < scan_limit; ++i) {
        char buf[32];
        buf[0] = '\0';
        if (wasmos_boot_module_name(i, (int32_t)(uintptr_t)buf, (int32_t)sizeof(buf)) < 0) {
            continue;
        }
        if (wasmos_sync_user_read((int32_t)(uintptr_t)buf, (int32_t)sizeof(buf)) != 0) {
            continue;
        }
        if (wasmos_sys_streq(buf, name)) {
            return i;
        }
    }
    return -1;
}


static void
hw_scan_acpi(void)
{
    acpi_rsdp_t rsdp;
    uint32_t length = 0;
    int32_t rc = wasmos_acpi_rsdp_info((int32_t)(uintptr_t)&rsdp,
                                       (int32_t)(uintptr_t)&length,
                                       (int32_t)sizeof(rsdp));
    if (rc != 0) {
        console_write("[device-manager] ACPI RSDP not found\n");
        return;
    }
    if (wasmos_sync_user_read((int32_t)(uintptr_t)&length, (int32_t)sizeof(length)) != 0 ||
        wasmos_sync_user_read((int32_t)(uintptr_t)&rsdp, (int32_t)sizeof(rsdp)) != 0) {
        console_write("[device-manager] ACPI sync failed\n");
        return;
    }
    if (length < 20) {
        console_write("[device-manager] ACPI RSDP too small\n");
        return;
    }
    if (!(rsdp.signature[0] == 'R' && rsdp.signature[1] == 'S' &&
          rsdp.signature[2] == 'D' && rsdp.signature[3] == ' ' &&
          rsdp.signature[4] == 'P' && rsdp.signature[5] == 'T' &&
          rsdp.signature[6] == 'R' && rsdp.signature[7] == ' ')) {
        console_write("[device-manager] ACPI RSDP signature mismatch\n");
        return;
    }
    console_write("[device-manager] ACPI RSDP ok\n");
}

static int
hw_spawn_driver_index(int32_t index)
{
    if (index < 0) {
        return -1;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_SPAWN,
                        g_dm.request_id,
                        index,
                        0,
                        0,
                        0) != 0) {
        return -1;
    }
    return 0;
}

static int
hw_spawn_driver_index_caps(int32_t index, const spawn_caps_t *caps)
{
    uint32_t io_packed = 0;
    if (!caps) {
        return hw_spawn_driver_index(index);
    }
    if (index < 0) {
        return -1;
    }
    io_packed = ((uint32_t)caps->io_port_min) | ((uint32_t)caps->io_port_max << 16);
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_SPAWN_CAPS,
                        g_dm.request_id,
                        index,
                        (int32_t)caps->cap_flags,
                        (int32_t)io_packed,
                        (int32_t)caps->irq_mask) != 0) {
        return -1;
    }
    return 0;
}

static int
hw_spawn_driver_name(hw_spawn_target_t target)
{
    const char *path = 0;
    uint32_t path_len = 0;
    if (target == HW_SPAWN_SERIAL) {
        path = "/boot/system/drivers/serial.wap";
    } else if (target == HW_SPAWN_KEYBOARD) {
        path = "/boot/system/drivers/keyboard.wap";
    } else if (target == HW_SPAWN_FRAMEBUFFER) {
        path = "/boot/system/drivers/framebuf.wap";
    }
    if (!path) {
        return -1;
    }
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 95u) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_SPAWN_PATH,
                        g_dm.request_id,
                        0,
                        (int32_t)path_len,
                        0,
                        0) != 0) {
        return -1;
    }
    return 0;
}

static int
hw_spawn_driver_path(const char *path)
{
    uint32_t path_len = 0;
    if (!path || path[0] == '\0') {
        return -1;
    }
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 95u) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_SPAWN_PATH,
                        g_dm.request_id,
                        0,
                        (int32_t)path_len,
                        0,
                        0) != 0) {
        return -1;
    }
    return 0;
}

static int
hw_spawn_rule_target(const char *rule_path)
{
    int32_t module_index = -1;
    if (!rule_path || rule_path[0] == '\0') {
        return -1;
    }
    if (rule_path[0] == '/') {
        return hw_spawn_driver_path(rule_path);
    }
    if (query_module_meta_by_path(rule_path, PROC_MODULE_SOURCE_INITFS, &module_index) != 0 || module_index < 0) {
        const char *tail = rule_path;
        char name[32];
        uint32_t n = 0;
        for (uint32_t i = 0; rule_path[i] != '\0'; ++i) {
            if (rule_path[i] == '/') {
                tail = &rule_path[i + 1];
            }
        }
        while (tail[n] && tail[n] != '.' && n + 1u < sizeof(name)) {
            name[n] = tail[n];
            n++;
        }
        name[n] = '\0';
        if (name[0] == '\0') {
            return -1;
        }
        module_index = module_index_by_name(name);
        if (module_index < 0) {
            for (uint32_t i = 0; name[i]; ++i) {
                if (name[i] == '_') {
                    name[i] = '-';
                }
            }
            module_index = module_index_by_name(name);
        }
        if (module_index < 0) {
            return -1;
        }
    }
    if (module_index == g_dm.selected_storage_index && g_dm.selected_storage_caps.cap_flags != 0) {
        return hw_spawn_driver_index_caps(module_index, &g_dm.selected_storage_caps);
    }
    return hw_spawn_driver_index(module_index);
}

static int
query_module_meta_by_path(const char *path, uint32_t source, int32_t *out_index)
{
    uint32_t path_len = 0;
    if (!path || !out_index) {
        return -1;
    }
    *out_index = -1;
    while (path[path_len] != '\0') {
        path_len++;
    }
    if (path_len == 0 || path_len > 95u) {
        return -1;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_MODULE_META_PATH,
                        g_dm.request_id,
                        (int32_t)(uintptr_t)path,
                        (int32_t)path_len,
                        (int32_t)source,
                        0) != 0) {
        return -1;
    }
    if (wasmos_ipc_recv(g_dm.reply_endpoint) < 0) {
        return -1;
    }
    int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if (resp_req != g_dm.request_id) {
        return -1;
    }
    g_dm.request_id++;
    if (resp_type != PROC_IPC_RESP) {
        return -1;
    }
    *out_index = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    return (*out_index >= 0) ? 0 : -1;
}

static void
registry_add_from_ipc(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3)
{
    if (g_dm.registry_count >= DEVICE_REGISTRY_CAP) {
        return;
    }
    pci_device_record_t *rec = &g_dm.registry[g_dm.registry_count++];
    uint32_t v0 = (uint32_t)arg0;
    uint32_t v1 = (uint32_t)arg1;
    uint32_t v2 = (uint32_t)arg2;
    rec->bus = (uint8_t)((v0 >> 24) & 0xFFu);
    rec->device = (uint8_t)((v0 >> 16) & 0xFFu);
    rec->function = (uint8_t)((v0 >> 8) & 0xFFu);
    rec->class_code = (uint8_t)(v0 & 0xFFu);
    rec->subclass = (uint8_t)((v1 >> 24) & 0xFFu);
    rec->prog_if = (uint8_t)((v1 >> 16) & 0xFFu);
    rec->vendor_id = (uint16_t)(v1 & 0xFFFFu);
    rec->device_id = (uint16_t)(v2 & 0xFFFFu);
    rec->mmio_hint = (uint8_t)((uint32_t)arg3 & 0xFFu);
    rec->irq_hint = (uint8_t)(((uint32_t)arg3 >> 8) & 0xFFu);
}

static void
queue_block_fs_rule_spawns(void)
{
    if (g_dm.rule_spawn_pending) {
        return;
    }
    for (uint32_t i = 0; i < g_dm.block_fs_rule_count; ++i) {
        block_fs_rule_t *rule = &g_dm.block_fs_rules[i];
        if (!rule->active || !rule->queued || rule->spawned) {
            continue;
        }
        wasmos_sys_strcpy(g_dm.rule_spawn_path, sizeof(g_dm.rule_spawn_path), rule->spawn_path);
        g_dm.rule_spawn_pending = 1;
        g_dm.rule_spawn_retries = 0;
        g_dm.active_rule_spawn_index = (int32_t)i;
        return;
    }
}

static void
queue_block_fs_rules_for_known_devices(void)
{
    for (uint32_t bi = 0; bi < g_dm.block_registry_count; ++bi) {
        const block_device_record_t *rec = &g_dm.block_registry[bi];
        if (!rec->in_use || !rec->present) {
            continue;
        }
        for (uint32_t ri = 0; ri < g_dm.block_fs_rule_count; ++ri) {
            block_fs_rule_t *rule = &g_dm.block_fs_rules[ri];
            if (!rule->active || rule->spawned || rule->queued) {
                continue;
            }
            if (rule->unit == 0xFFu || rule->unit == rec->unit) {
                rule->queued = 1;
            }
        }
    }
    queue_block_fs_rule_spawns();
}

static void
registry_add_block_from_ipc(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3)
{
    (void)arg3;
    uint8_t unit = (uint8_t)((uint32_t)arg0 & 0xFFu);
    uint32_t sectors = (uint32_t)arg1;
    uint8_t present = (uint8_t)(((uint32_t)arg2 & 1u) != 0u);
    uint8_t active = (uint8_t)((((uint32_t)arg2 >> 1) & 1u) != 0u);
    block_device_record_t *rec = 0;
    for (uint32_t i = 0; i < g_dm.block_registry_count; ++i) {
        if (g_dm.block_registry[i].in_use && g_dm.block_registry[i].unit == unit) {
            rec = &g_dm.block_registry[i];
            break;
        }
    }
    if (!rec) {
        if (g_dm.block_registry_count >= BLOCK_REGISTRY_CAP) {
            return;
        }
        rec = &g_dm.block_registry[g_dm.block_registry_count++];
        rec->in_use = 1;
    }
    rec->unit = unit;
    rec->present = present;
    rec->active_service = active;
    rec->sector_count = sectors;
    if (g_dm.selected_storage_has_record) {
        const pci_device_record_t *p = &g_dm.selected_storage_record;
        (void)snprintf(rec->canonical_id,
                       sizeof(rec->canonical_id),
                       "block:pci:%02X:%02X.%02X:ata%u",
                       (unsigned)p->bus,
                       (unsigned)p->device,
                       (unsigned)p->function,
                       (unsigned)unit);
    } else {
        (void)snprintf(rec->canonical_id,
                       sizeof(rec->canonical_id),
                       "block:pci:??:??.??:ata%u",
                       (unsigned)unit);
    }
    hex16_from_sha256(rec->canonical_id, rec->hash_id);
    {
        char msg[224];
        (void)snprintf(msg,
                       sizeof(msg),
                       "[device-manager] block add id=%s hash=%s present=%u sectors=%u\n",
                       rec->canonical_id,
                       rec->hash_id,
                       (unsigned)rec->present,
                       (unsigned)rec->sector_count);
        console_write(msg);
    }
    if (rec->present) {
        uint8_t queued_any = 0;
        for (uint32_t i = 0; i < g_dm.block_fs_rule_count; ++i) {
            block_fs_rule_t *rule = &g_dm.block_fs_rules[i];
            if (!rule->active || rule->spawned || rule->queued) {
                continue;
            }
            if (rule->unit == 0xFFu || rule->unit == rec->unit) {
                rule->queued = 1;
                queued_any = 1;
            }
        }
        if (queued_any) {
            queue_block_fs_rule_spawns();
            g_dm.need_fat = 0;
            console_write("[device-manager] block_fs rule queued spawn\n");
        }
    }
}

static void
reset_selected_storage(void)
{
    g_dm.selected_storage_index = -1;
    g_dm.selected_storage_has_record = 0;
    g_dm.selected_storage_caps.cap_flags = 0;
    g_dm.selected_storage_caps.io_port_min = 0;
    g_dm.selected_storage_caps.io_port_max = 0;
    g_dm.selected_storage_caps.irq_mask = 0;
}

static int
query_driver_module_meta(int32_t module_index,
                         uint32_t match_index,
                         uint8_t *out_class_code,
                         uint8_t *out_subclass,
                         uint8_t *out_prog_if,
                         uint16_t *out_vendor_id,
                         uint16_t *out_device_id,
                         uint8_t *out_storage_bootstrap,
                         uint8_t *out_match_count,
                         spawn_caps_t *out_caps)
{
    if (!out_class_code || !out_subclass || !out_prog_if ||
        !out_vendor_id || !out_device_id || !out_storage_bootstrap || !out_match_count || !out_caps) {
        return -1;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_MODULE_META,
                        g_dm.request_id,
                        module_index,
                        (int32_t)match_index,
                        0,
                        0) != 0) {
        return -1;
    }
    if (wasmos_ipc_recv(g_dm.reply_endpoint) < 0) {
        return -1;
    }
    int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if (resp_req != g_dm.request_id) {
        return -1;
    }
    g_dm.request_id++;
    if (resp_type != PROC_IPC_RESP) {
        return -1;
    }
    uint32_t arg0 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    uint32_t arg1 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    uint32_t arg2 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    uint32_t arg3 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
    *out_class_code = (uint8_t)((arg1 >> 24) & 0xFFu);
    *out_subclass = (uint8_t)((arg1 >> 16) & 0xFFu);
    *out_prog_if = (uint8_t)((arg1 >> 8) & 0xFFu);
    *out_storage_bootstrap = (uint8_t)(arg1 & 0x1u);
    *out_match_count = (uint8_t)((arg1 >> 1) & 0x7Fu);
    *out_vendor_id = (uint16_t)((arg2 >> 16) & 0xFFFFu);
    *out_device_id = (uint16_t)(arg2 & 0xFFFFu);
    out_caps->cap_flags = arg3 & 0xFFFFu;
    out_caps->io_port_min = (uint16_t)(arg0 & 0xFFFFu);
    out_caps->io_port_max = (uint16_t)((arg0 >> 16) & 0xFFFFu);
    out_caps->irq_mask = (uint16_t)((1u << 14) | (1u << 15));
    return 0;
}

static int
match_any_or_u8(uint8_t actual, uint8_t expected)
{
    return (expected == MATCH_ANY_U8 || actual == expected) ? 1 : 0;
}

static int
match_any_or_u16(uint16_t actual, uint16_t expected)
{
    return (expected == MATCH_ANY_U16 || actual == expected) ? 1 : 0;
}

static int
select_pci_matched_driver(int32_t module_index, spawn_caps_t *out_caps)
{
    uint8_t class_code = 0, subclass = 0, prog_if = 0, storage_bootstrap = 0, match_count = 0;
    uint16_t vendor_id = 0, device_id = 0;
    spawn_caps_t caps;
    if (module_index < 0 || !out_caps) {
        return -1;
    }
    if (query_driver_module_meta(module_index, 0,
                                 &class_code, &subclass, &prog_if,
                                 &vendor_id, &device_id,
                                 &storage_bootstrap, &match_count, &caps) != 0 ||
        match_count == 0) {
        return -1;
    }
    for (uint32_t m = 0; m < match_count; ++m) {
        if (m != 0) {
            if (query_driver_module_meta(module_index, m,
                                         &class_code, &subclass, &prog_if,
                                         &vendor_id, &device_id,
                                         &storage_bootstrap, &match_count, &caps) != 0) {
                continue;
            }
        }
        for (uint32_t i = 0; i < g_dm.registry_count; ++i) {
            const pci_device_record_t *rec = &g_dm.registry[i];
            if (!match_any_or_u8(rec->class_code, class_code) ||
                !match_any_or_u8(rec->subclass, subclass) ||
                !match_any_or_u8(rec->prog_if, prog_if) ||
                !match_any_or_u16(rec->vendor_id, vendor_id) ||
                !match_any_or_u16(rec->device_id, device_id)) {
                continue;
            }
            *out_caps = caps;
            if ((out_caps->cap_flags & DEVMGR_CAP_IRQ) != 0 && rec->irq_hint < 16u) {
                out_caps->irq_mask = (uint16_t)(1u << rec->irq_hint);
            }
            return 0;
        }
    }
    return -1;
}

static void
apply_pci_matches(void)
{
    reset_selected_storage();
    for (int32_t module_index = 0; module_index < g_dm.module_count; ++module_index) {
        uint8_t class_code = 0, subclass = 0, prog_if = 0, storage_bootstrap = 0, match_count = 0;
        uint16_t vendor_id = 0, device_id = 0;
        spawn_caps_t caps;
        if (query_driver_module_meta(module_index, 0,
                                     &class_code, &subclass, &prog_if,
                                     &vendor_id, &device_id,
                                     &storage_bootstrap, &match_count, &caps) != 0 ||
            !storage_bootstrap || match_count == 0) {
            continue;
        }
        for (uint32_t m = 0; m < match_count; ++m) {
            if (m != 0) {
                if (query_driver_module_meta(module_index, m,
                                             &class_code, &subclass, &prog_if,
                                             &vendor_id, &device_id,
                                             &storage_bootstrap, &match_count, &caps) != 0) {
                    continue;
                }
            }
            for (uint32_t i = 0; i < g_dm.registry_count; ++i) {
                const pci_device_record_t *rec = &g_dm.registry[i];
                if (!match_any_or_u8(rec->class_code, class_code) ||
                    !match_any_or_u8(rec->subclass, subclass) ||
                    !match_any_or_u8(rec->prog_if, prog_if) ||
                    !match_any_or_u16(rec->vendor_id, vendor_id) ||
                    !match_any_or_u16(rec->device_id, device_id)) {
                    continue;
                }
                g_dm.selected_storage_index = module_index;
                g_dm.selected_storage_caps = caps;
                g_dm.selected_storage_record = *rec;
                g_dm.selected_storage_has_record = 1;
                if ((g_dm.selected_storage_caps.cap_flags & DEVMGR_CAP_IRQ) != 0 &&
                    rec->irq_hint < 16u) {
                    g_dm.selected_storage_caps.irq_mask = (uint16_t)(1u << rec->irq_hint);
                }
                return;
            }
        }
    }
}

static void
consume_pci_inventory(void)
{
    if (g_dm.inventory_endpoint < 0) {
        return;
    }
    g_dm.registry_count = 0;
    console_write("[device-manager] waiting for pci-bus inventory\n");
    for (;;) {
        if (wasmos_ipc_recv(g_dm.inventory_endpoint) < 0) {
            console_write("[device-manager] pci inventory recv failed\n");
            break;
        }
        int32_t msg_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        if (msg_type == DEVMGR_PUBLISH_DEVICE) {
            int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
            int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
            int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
            registry_add_from_ipc(arg0, arg1, arg2, arg3);
            continue;
        }
        if (msg_type == DEVMGR_PUBLISH_BLOCK_DEVICE) {
            int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
            int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
            int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
            registry_add_block_from_ipc(arg0, arg1, arg2, arg3);
            continue;
        }
        if (msg_type == DEVMGR_PCI_SCAN_DONE) {
            console_write("[device-manager] pci-bus scan complete\n");
            break;
        }
    }
    apply_pci_matches();
}

static void
consume_inventory_events_nonblocking(void)
{
    if (g_dm.inventory_endpoint < 0) {
        return;
    }
    for (;;) {
        if (wasmos_ipc_try_recv(g_dm.inventory_endpoint) <= 0) {
            return;
        }
        int32_t msg_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        if (msg_type == DEVMGR_PUBLISH_BLOCK_DEVICE) {
            int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
            int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
            int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
            registry_add_block_from_ipc(arg0, arg1, arg2, arg3);
        }
    }
}

static hw_spawn_target_t
next_spawn_target(void)
{
    uint8_t fat_ready = proc_running("fs-fat") ? 1u : 0u;
    if (g_dm.need_pci_bus) {
        return HW_SPAWN_PCI_BUS;
    }
    if (g_dm.rule_spawn_pending && g_dm.rule_spawn_path[0] != '\0') {
        return HW_SPAWN_RULE_PATH;
    }
    if (g_dm.need_fat) {
        return HW_SPAWN_FAT;
    }
    if (!fat_ready) {
        return HW_SPAWN_NONE;
    }
    if (g_dm.need_serial) {
        return HW_SPAWN_SERIAL;
    }
    if (g_dm.need_keyboard) {
        return HW_SPAWN_KEYBOARD;
    }
    if (g_dm.need_framebuffer) {
        return HW_SPAWN_FRAMEBUFFER;
    }
    return HW_SPAWN_NONE;
}

static void
handle_query_message_fields(void)
{
    int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
    int32_t index = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (type != DEVMGR_QUERY_MOUNT_REQ && type != DEVMGR_QUERY_BLOCK_MOUNT_REQ) {
        (void)wasmos_ipc_send(source, g_dm.query_endpoint, FS_IPC_ERROR, req_id, type, 0, 0, 0);
        return;
    }
    if (type == DEVMGR_QUERY_BLOCK_MOUNT_REQ) {
        uint8_t unit = (uint8_t)((uint32_t)index & 0xFFu);
        const char *mount = 0;
        uint32_t packed[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < g_dm.block_fs_rule_count; ++i) {
            block_fs_rule_t *rule = &g_dm.block_fs_rules[i];
            if (!rule->active) {
                continue;
            }
            if (rule->unit == unit || rule->unit == 0xFFu) {
                mount = rule->mount;
                break;
            }
        }
        if (!mount || mount[0] == '\0') {
            (void)wasmos_ipc_send(source, g_dm.query_endpoint, FS_IPC_ERROR, req_id, -1, 0, 0, 0);
            return;
        }
        for (uint32_t i = 0; mount[i] && i < 16u; ++i) {
            uint32_t slot = i / 4u;
            uint32_t shift = (i % 4u) * 8u;
            packed[slot] |= ((uint32_t)(uint8_t)mount[i]) << shift;
        }
        (void)wasmos_ipc_send(source,
                              g_dm.query_endpoint,
                              DEVMGR_BLOCK_MOUNT_INFO,
                              req_id,
                              (int32_t)packed[0],
                              (int32_t)packed[1],
                              (int32_t)packed[2],
                              (int32_t)packed[3]);
        return;
    }
    if (index == 0) {
        uint32_t a1 = 0;
        uint32_t a2 = 0;
        uint32_t a3 = 0;
        if (g_dm.selected_storage_has_record) {
            const pci_device_record_t *rec = &g_dm.selected_storage_record;
            a1 = ((uint32_t)rec->bus << 24) | ((uint32_t)rec->device << 16) |
                 ((uint32_t)rec->function << 8) | (uint32_t)rec->class_code;
            a2 = ((uint32_t)rec->subclass << 24) | ((uint32_t)rec->prog_if << 16) |
                 (uint32_t)rec->vendor_id;
            a3 = (uint32_t)rec->device_id | ((uint32_t)1u << 31);
        }
        (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_MOUNT_INFO, req_id, 0, (int32_t)a1, (int32_t)a2, (int32_t)a3);
        return;
    }
    if (index == 1) {
        if (g_dm.boot_mount_ready) {
            (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_MOUNT_INFO, req_id, 1, 0, 0, 0);
        } else {
            (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_QUERY_DONE, req_id, 0, 0, 0, 0);
        }
        return;
    }
    if (index == 2) {
        if (g_dm.user_mount_ready) {
            uint32_t a1 = 0;
            uint32_t a2 = 0;
            uint32_t a3 = 0;
            if (g_dm.selected_storage_has_record) {
                const pci_device_record_t *rec = &g_dm.selected_storage_record;
                a1 = ((uint32_t)rec->bus << 24) | ((uint32_t)rec->device << 16) |
                     ((uint32_t)rec->function << 8) | (uint32_t)rec->class_code;
                a2 = ((uint32_t)rec->subclass << 24) | ((uint32_t)rec->prog_if << 16) |
                     (uint32_t)rec->vendor_id;
                a3 = (uint32_t)rec->device_id | ((uint32_t)1u << 31);
            }
            (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_MOUNT_INFO, req_id, 2, (int32_t)a1, (int32_t)a2, (int32_t)a3);
        } else {
            (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_QUERY_DONE, req_id, 0, 0, 0, 0);
        }
        return;
    }
    (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_QUERY_DONE, req_id, 0, 0, 0, 0);
}

static void
handle_query_endpoint(void)
{
    if (g_dm.query_endpoint < 0) {
        return;
    }
    if (wasmos_ipc_try_recv(g_dm.query_endpoint) <= 0) {
        return;
    }
    handle_query_message_fields();
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t module_count,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_dm.reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_dm.reply_endpoint < 0) {
        g_dm.phase = HW_PHASE_FAILED;
        console_write("[device-manager] failed to create reply endpoint\n");
        wasmos_sys_ipc_recv_loop();
    }
    if (proc_endpoint < 0 || module_count <= 0) {
        g_dm.phase = HW_PHASE_FAILED;
        console_write("[device-manager] invalid init args\n");
        wasmos_sys_ipc_recv_loop();
    }
    g_dm.proc_endpoint = proc_endpoint;
    g_dm.module_count = module_count;
    g_dm.inventory_endpoint = wasmos_ipc_create_endpoint();
    if (g_dm.inventory_endpoint < 0) {
        console_write("[device-manager] inventory endpoint create failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    if (wasmos_svc_register(g_dm.proc_endpoint, g_dm.inventory_endpoint, "devmgr.inv", 1) != 0) {
        console_write("[device-manager] inventory register failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    g_dm.query_endpoint = g_dm.inventory_endpoint;
    if (wasmos_svc_register(g_dm.proc_endpoint, g_dm.query_endpoint, "devmgr.query", 1) != 0) {
        console_write("[device-manager] query register failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    g_dm.rule_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_dm.rule_reply_endpoint < 0) {
        console_write("[device-manager] rules reply endpoint create failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    log_rule_roots_once();
    load_rules_if_available();
    hw_scan_acpi();
    (void)query_module_meta_by_path("system/services/pci_bus.wap", PROC_MODULE_SOURCE_INITFS, &g_dm.pci_bus_index);
    if (g_dm.pci_bus_index < 0) {
        g_dm.pci_bus_index = module_index_by_name("pci-bus");
    }
    (void)query_module_meta_by_path("system/drivers/fs_fat.wap", PROC_MODULE_SOURCE_INITFS, &g_dm.fat_index);
    if (g_dm.fat_index < 0) {
        g_dm.fat_index = module_index_by_name("fs-fat");
    }
    (void)query_module_meta_by_path("system/drivers/fs_init.wap", PROC_MODULE_SOURCE_INITFS, &g_dm.fs_init_index);
    if (g_dm.fs_init_index < 0) {
        g_dm.fs_init_index = module_index_by_name("fs-init");
    }
    (void)query_module_meta_by_path("system/services/fs_manager.wap", PROC_MODULE_SOURCE_INITFS, &g_dm.fs_manager_index);
    if (g_dm.fs_manager_index < 0) {
        g_dm.fs_manager_index = module_index_by_name("fs-manager");
    }
    /* Boot-time hardware drivers live on /boot and are not part of initfs. */
    (void)query_module_meta_by_path("/boot/system/drivers/serial.wap", PROC_MODULE_SOURCE_FS, &g_dm.serial_index);
    (void)query_module_meta_by_path("/boot/system/drivers/keyboard.wap", PROC_MODULE_SOURCE_FS, &g_dm.keyboard_index);
    (void)query_module_meta_by_path("/boot/system/drivers/framebuf.wap", PROC_MODULE_SOURCE_FS, &g_dm.framebuffer_index);

    /* Bootstrap policy: always bring up pci-bus when present in initfs.
     * Runtime dedup/restart policy is handled by later lifecycle phases. */
    g_dm.need_pci_bus = (g_dm.pci_bus_index >= 0) ? 1 : 0;
    g_dm.need_fat = 0;
    g_dm.need_fs_init = 0;
    g_dm.need_fs_manager = 0;
    /* Serial is resolved from PCI inventory matches when pci-bus is started
     * by this service; if pci-bus is already running, keep legacy fallback. */
    g_dm.need_serial = g_dm.need_pci_bus ? 0 : (!proc_running("serial") ? 1 : 0);
    g_dm.need_keyboard = !proc_running("keyboard") ? 1 : 0;
    g_dm.need_framebuffer = !proc_running("framebuffer") ? 1 : 0;
    g_dm.phase = HW_PHASE_SPAWN;

    for (;;) {
        if (g_dm.phase == HW_PHASE_SPAWN) {
            hw_spawn_target_t target = next_spawn_target();
            if (target == HW_SPAWN_NONE) {
                g_dm.idle_logged = 1;
                g_dm.phase = HW_PHASE_IDLE;
                continue;
            }
            g_dm.idle_logged = 0;
            if (target == HW_SPAWN_PCI_BUS) {
                spawn_caps_t pci_caps;
                pci_caps.cap_flags = DEVMGR_CAP_IO_PORT;
                pci_caps.io_port_min = 0x0CF8;
                pci_caps.io_port_max = 0x0CFF;
                pci_caps.irq_mask = 0;
                if (hw_spawn_driver_index_caps(g_dm.pci_bus_index, &pci_caps) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn pci-bus failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            } else if (target == HW_SPAWN_RULE_PATH) {
                if (hw_spawn_rule_target(g_dm.rule_spawn_path) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn rule path failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            } else if (target == HW_SPAWN_SERIAL) {
                if (hw_spawn_driver_index_caps(g_dm.serial_index, &g_dm.selected_serial_caps) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn serial failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            } else if (target == HW_SPAWN_KEYBOARD) {
                if ((g_dm.keyboard_index >= 0 && hw_spawn_driver_index(g_dm.keyboard_index) != 0) ||
                    (g_dm.keyboard_index < 0 && hw_spawn_driver_name(target) != 0)) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn keyboard failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            } else if (target == HW_SPAWN_FRAMEBUFFER) {
                if ((g_dm.framebuffer_index >= 0 && hw_spawn_driver_index(g_dm.framebuffer_index) != 0) ||
                    (g_dm.framebuffer_index < 0 && hw_spawn_driver_name(target) != 0)) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn framebuffer failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            } else if (target == HW_SPAWN_FAT) {
                if (hw_spawn_driver_index(g_dm.fat_index) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn fs-fat failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            }
            g_dm.pending = target;
            g_dm.phase = HW_PHASE_WAIT;
            continue;
        }

        if (g_dm.phase == HW_PHASE_WAIT) {
            int32_t recv_rc = wasmos_ipc_recv(g_dm.reply_endpoint);
            if (recv_rc < 0) {
                g_dm.phase = HW_PHASE_FAILED;
                console_write("[device-manager] spawn recv failed\n");
                wasmos_sys_ipc_recv_loop();
            }

            int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
            int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            if (resp_req != g_dm.request_id) {
                g_dm.phase = HW_PHASE_FAILED;
                console_write("[device-manager] spawn response mismatch\n");
                wasmos_sys_ipc_recv_loop();
            }
            g_dm.request_id++;
            if (resp_type == PROC_IPC_RESP) {
                if (g_dm.pending == HW_SPAWN_PCI_BUS) {
                    g_dm.need_pci_bus = 0;
                    g_dm.pending = HW_SPAWN_NONE;
                    g_dm.phase = HW_PHASE_WAIT_INVENTORY;
                    continue;
                }
                if (g_dm.pending == HW_SPAWN_RULE_PATH) {
                    g_dm.rule_spawn_pending = 0;
                    if (g_dm.active_rule_spawn_index >= 0 &&
                        g_dm.active_rule_spawn_index < (int32_t)g_dm.block_fs_rule_count) {
                        block_fs_rule_t *rule = &g_dm.block_fs_rules[g_dm.active_rule_spawn_index];
                        rule->spawned = 1;
                        rule->queued = 0;
                        if (rule->unit == 0) {
                            g_dm.boot_mount_ready = 1;
                        } else if (rule->unit == 1) {
                            g_dm.user_mount_ready = 1;
                        }
                        g_dm.active_rule_spawn_index = -1;
                    }
                    queue_block_fs_rule_spawns();
                }
                if (g_dm.pending == HW_SPAWN_SERIAL) {
                    g_dm.need_serial = 0;
                } else if (g_dm.pending == HW_SPAWN_KEYBOARD) {
                    g_dm.need_keyboard = 0;
                } else if (g_dm.pending == HW_SPAWN_FRAMEBUFFER) {
                    g_dm.need_framebuffer = 0;
                } else if (g_dm.pending == HW_SPAWN_FAT) {
                    g_dm.need_fat = 0;
                } else if (g_dm.pending == HW_SPAWN_FS_INIT) {
                    g_dm.need_fs_init = 0;
                } else if (g_dm.pending == HW_SPAWN_FS_MANAGER) {
                    g_dm.need_fs_manager = 0;
                }
                g_dm.pending = HW_SPAWN_NONE;
                g_dm.phase = HW_PHASE_SPAWN;
                continue;
            }
            if (resp_type == PROC_IPC_ERROR) {
                int32_t resp_code = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
                if (resp_code == -2) {
                    wasmos_sched_yield();
                    g_dm.pending = HW_SPAWN_NONE;
                    g_dm.phase = HW_PHASE_SPAWN;
                    continue;
                }
                if (g_dm.pending == HW_SPAWN_SERIAL) {
                    g_dm.serial_retries++;
                    if (g_dm.serial_retries > 8) {
                        g_dm.need_serial = 0;
                    }
                } else if (g_dm.pending == HW_SPAWN_KEYBOARD) {
                    g_dm.keyboard_retries++;
                    if (g_dm.keyboard_retries > 8) {
                        g_dm.need_keyboard = 0;
                    }
                } else if (g_dm.pending == HW_SPAWN_FRAMEBUFFER) {
                    g_dm.framebuffer_retries++;
                    if (g_dm.framebuffer_retries > 8) {
                        g_dm.need_framebuffer = 0;
                    }
                } else if (g_dm.pending == HW_SPAWN_FAT) {
                    g_dm.fat_retries++;
                    if (g_dm.fat_retries > 8) {
                        g_dm.need_fat = 0;
                    }
                } else if (g_dm.pending == HW_SPAWN_FS_INIT) {
                    g_dm.fs_init_retries++;
                    if (g_dm.fs_init_retries > 8) {
                        g_dm.need_fs_init = 0;
                    }
                } else if (g_dm.pending == HW_SPAWN_FS_MANAGER) {
                    g_dm.need_fs_manager = 0;
                } else if (g_dm.pending == HW_SPAWN_PCI_BUS) {
                    g_dm.need_pci_bus = 0;
                } else if (g_dm.pending == HW_SPAWN_RULE_PATH) {
                    g_dm.rule_spawn_retries++;
                    if (g_dm.rule_spawn_retries > 8) {
                        g_dm.rule_spawn_pending = 0;
                        if (g_dm.active_rule_spawn_index >= 0 &&
                            g_dm.active_rule_spawn_index < (int32_t)g_dm.block_fs_rule_count) {
                            g_dm.block_fs_rules[g_dm.active_rule_spawn_index].queued = 0;
                        }
                        g_dm.active_rule_spawn_index = -1;
                    } else {
                        g_dm.rule_spawn_pending = 1;
                    }
                }
                g_dm.pending = HW_SPAWN_NONE;
                g_dm.phase = HW_PHASE_SPAWN;
                continue;
            }

            g_dm.phase = HW_PHASE_FAILED;
            console_write("[device-manager] spawn response invalid\n");
            wasmos_sys_ipc_recv_loop();
        }

        if (g_dm.phase == HW_PHASE_WAIT_INVENTORY) {
            consume_pci_inventory();
            g_dm.need_fat = 0;
            g_dm.need_fs_init = 0;
            g_dm.need_fs_manager = 0;
            if (g_dm.serial_index >= 0 &&
                !proc_running("serial") &&
                select_pci_matched_driver(g_dm.serial_index, &g_dm.selected_serial_caps) == 0) {
                g_dm.need_serial = 1;
            } else {
                g_dm.need_serial = 0;
            }
            g_dm.phase = HW_PHASE_SPAWN;
            continue;
        }

        if (g_dm.phase == HW_PHASE_IDLE) {
            load_rules_if_available();
            consume_inventory_events_nonblocking();
            if (next_spawn_target() != HW_SPAWN_NONE) {
                g_dm.phase = HW_PHASE_SPAWN;
                continue;
            }
            if (g_dm.rules_boot_request_pending) {
                handle_query_endpoint();
                wasmos_sched_yield();
                continue;
            }
            if (wasmos_ipc_recv(g_dm.query_endpoint) >= 0) {
                int32_t msg_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
                if (msg_type == DEVMGR_PUBLISH_BLOCK_DEVICE) {
                    int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
                    int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
                    int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
                    int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
                    registry_add_block_from_ipc(arg0, arg1, arg2, arg3);
                } else if (msg_type == DEVMGR_PUBLISH_DEVICE || msg_type == DEVMGR_PCI_SCAN_DONE) {
                    /* Inventory events may share this endpoint once idle. */
                    if (msg_type == DEVMGR_PUBLISH_DEVICE) {
                        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
                        int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
                        int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
                        int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
                        registry_add_from_ipc(arg0, arg1, arg2, arg3);
                    }
                } else {
                    handle_query_message_fields();
                }
                continue;
            }
            wasmos_sched_yield();
            continue;
        }

        console_write("[device-manager] failed\n");
        wasmos_sys_ipc_recv_loop();
    }
}
