#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"
#include "device_manager_types.h"
#include "device_manager_rules.h"

/*
 * device-manager coordinates early hardware startup in user space.
 * In this slice, a separate pci-bus service performs enumeration and publishes
 * records over IPC; device-manager consumes those records and selects storage
 * bootstrap (ata/fs-fat) before post-FAT drivers.
 */

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
    .rules_roots_logged = 0,
    .idle_logged = 0,
    .rules_init_fail_logged = 0,
    .rules_init_loaded = 0,
    .rules_boot_loaded = 0,
    .rules_boot_request_pending = 0,
    .rules_boot_retry_delay = 0,
    .rules_boot_failures = 0,
    .rules_boot_request_id = -1,
    .rules_init_active = 0,
    .rules_boot_active = 0,
    .rule_spawn_pending = 0,
    .rule_spawn_retries = 0,
    .rule_spawn_path = {0},
    .active_rule_spawn_kind = 0,
    .active_rule_spawn_index = -1,
    .active_rule_spawn_device_index = -1,
    .always_spawn_rules = {0},
    .always_spawn_rule_count = 0,
    .block_fs_rules = {0},
    .block_fs_rule_count = 0,
    .pci_match_rules = {0},
    .pci_match_rule_count = 0,
    .boot_mount_ready = 0,
    .user_mount_ready = 0,
};

static wasmos_sys_event_loop_t g_dm_ipc_loop;
static wasmos_sys_event_loop_t g_dm_inventory_loop;
static wasmos_sys_event_loop_t g_dm_rules_loop;

typedef struct {
    uint8_t done;
    wasmos_ipc_message_t msg;
} dm_intent_wait_t;

typedef struct {
    uint8_t pending;
    uint8_t done;
    wasmos_ipc_message_t msg;
} dm_spawn_wait_t;

static dm_spawn_wait_t g_dm_spawn_wait = {0};
static uint8_t g_dm_pci_scan_done = 0;

typedef struct {
    uint8_t pending;
    uint8_t done;
    wasmos_ipc_message_t msg;
} dm_rules_wait_t;

static dm_rules_wait_t g_dm_rules_wait = {0};

#define RULE_SPAWN_KIND_NONE 0u
#define RULE_SPAWN_KIND_ALWAYS 1u
#define RULE_SPAWN_KIND_BLOCK_FS 2u
#define RULE_SPAWN_KIND_PCI_MATCH 3u

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
static void queue_always_spawn_rules(void);
static void queue_pci_match_rule_spawns(void);
static void queue_block_fs_rule_spawns(void);
static void queue_block_fs_rules_for_known_devices(void);
static int module_index_by_name(const char *name);
static uint16_t count_loaded_active_rules(void);
static int is_mount_already_active(const char *mount);
static void handle_query_message_fields(const wasmos_ipc_message_t *msg);
static int dm_ipc_call(int32_t destination_endpoint,
                       int32_t source_endpoint,
                       int32_t msg_type,
                       int32_t request_id,
                       int32_t arg0,
                       int32_t arg1,
                       int32_t arg2,
                       int32_t arg3,
                       wasmos_ipc_message_t *out_msg,
                       int32_t max_empty_polls);
static void dm_handle_ipc_default(void *user, const wasmos_ipc_message_t *msg);
static void dm_handle_rules_reply(void *user, const wasmos_ipc_message_t *msg);
static int dm_register_ipc_handlers(void);
static int dm_register_inventory_handlers(void);
static int dm_register_rules_handlers(void);

static void
dm_intent_resolve_store(void *user, const wasmos_ipc_message_t *msg)
{
    dm_intent_wait_t *state = (dm_intent_wait_t *)user;
    if (!state || !msg) {
        return;
    }
    state->done = 1;
    state->msg = *msg;
}

static int
dm_ipc_call(int32_t destination_endpoint,
            int32_t source_endpoint,
            int32_t msg_type,
            int32_t request_id,
            int32_t arg0,
            int32_t arg1,
            int32_t arg2,
            int32_t arg3,
            wasmos_ipc_message_t *out_msg,
            int32_t max_empty_polls)
{
    dm_intent_wait_t wait_state = {0};
    int32_t empty_polls = 0;
    if (request_id <= 0 || !out_msg) {
        return -1;
    }
    if (wasmos_sys_intent_send_with_request_id(&g_dm_ipc_loop,
                                               destination_endpoint,
                                               source_endpoint,
                                               request_id,
                                               msg_type,
                                               arg0,
                                               arg1,
                                               arg2,
                                               arg3,
                                               dm_intent_resolve_store,
                                               &wait_state) != 0) {
        return -1;
    }
    if (max_empty_polls <= 0) {
        max_empty_polls = 256;
    }
    while (!wait_state.done) {
        int32_t handled = wasmos_sys_event_loop_poll(&g_dm_ipc_loop, 16);
        if (handled < 0) {
            return -1;
        }
        if (handled == 0) {
            if (empty_polls++ >= max_empty_polls) {
                return -1;
            }
            (void)wasmos_sched_yield();
        }
    }
    *out_msg = wait_state.msg;
    return 0;
}

static int
copy_mount_name_token(const char *line, char *out, uint32_t out_len)
{
    uint32_t i = 0;
    uint32_t pos = 0;
    if (!line || !out || out_len < 2u) {
        return -1;
    }
    if (line[0] != '/') {
        return -1;
    }
    for (i = 1; line[i] != '\0'; ++i) {
        if (line[i] == ' ') {
            break;
        }
        if (pos + 1u >= out_len) {
            break;
        }
        out[pos++] = line[i];
    }
    out[pos] = '\0';
    if (pos == 0) {
        return -1;
    }
    return 0;
}

static int
is_mount_already_active(const char *mount)
{
    char buf[384];
    int32_t req_id = 0;
    int32_t resp_type = 0;
    int32_t n = 0;
    uint32_t i = 0;
    char mount_name[16];
    if (!mount || mount[0] != '/') {
        return 0;
    }
    if (ensure_fs_endpoint() != 0) {
        return 0;
    }
    if (g_dm.reply_endpoint < 0) {
        return 0;
    }
    req_id = g_dm.request_id++;
    wasmos_ipc_message_t resp;
    if (dm_ipc_call(g_dm.fs_endpoint,
                    g_dm.reply_endpoint,
                    FSMGR_IPC_QUERY_MOUNTS_REQ,
                    req_id,
                    0,
                    0,
                    0,
                    0,
                    &resp,
                    128) != 0) {
        return 0;
    }
    resp_type = resp.type;
    if (resp_type != FSMGR_IPC_QUERY_MOUNTS_RESP) {
        return 0;
    }
    n = resp.arg0;
    if (n <= 0 || n >= (int32_t)sizeof(buf)) {
        return 0;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)buf, n, 0) != 0) {
        return 0;
    }
    buf[n] = '\0';
    while (mount[0] == '/') {
        mount++;
    }
    while (buf[i] != '\0') {
        char line[64];
        uint32_t pos = 0;
        while (buf[i] != '\0' && buf[i] != '\n' && pos + 1u < sizeof(line)) {
            line[pos++] = buf[i++];
        }
        line[pos] = '\0';
        if (buf[i] == '\n') {
            i++;
        }
        if (copy_mount_name_token(line, mount_name, sizeof(mount_name)) == 0 &&
            wasmos_sys_streq(mount_name, mount)) {
            return 1;
        }
    }
    return 0;
}

static void
log_mount_already_active(const char *mount)
{
    char msg[96];
    if (!mount || mount[0] == '\0') {
        return;
    }
    (void)snprintf(msg,
                   sizeof(msg),
                   "[device-manager] mount already active; skipping rule mount=%s\n",
                   mount);
    console_write(msg);
}

static uint16_t
count_loaded_active_rules(void)
{
    uint16_t count = 0;
    for (uint32_t i = 0; i < g_dm.always_spawn_rule_count; ++i) {
        if (g_dm.always_spawn_rules[i].active) {
            count++;
        }
    }
    for (uint32_t i = 0; i < g_dm.block_fs_rule_count; ++i) {
        if (g_dm.block_fs_rules[i].active) {
            count++;
        }
    }
    for (uint32_t i = 0; i < g_dm.pci_match_rule_count; ++i) {
        if (g_dm.pci_match_rules[i].active) {
            count++;
        }
    }
    return count;
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
    if (g_dm.rules_boot_retry_delay > 0) {
        g_dm.rules_boot_retry_delay--;
        return;
    }
    if (!g_dm.boot_mount_ready) {
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
    g_dm_rules_wait.pending = 1;
    g_dm_rules_wait.done = 0;
}

static void
poll_boot_rules_async(void)
{
    int32_t read_len = 0;
    char text[DEVMGR_RULE_TEXT_CAP];
    char msg[128];
    if (!g_dm.rules_boot_request_pending) {
        return;
    }
    while (!g_dm_rules_wait.done) {
        int32_t handled = wasmos_sys_event_loop_poll(&g_dm_rules_loop, 8);
        if (handled <= 0) {
            return;
        }
    }
    g_dm_rules_wait.pending = 0;
    g_dm.rules_boot_request_pending = 0;
    if (g_dm_rules_wait.msg.type != FS_IPC_RESP) {
        if (g_dm.rules_boot_failures < 255u) {
            g_dm.rules_boot_failures++;
        }
        if (g_dm.rules_boot_failures >= 8u) {
            g_dm.rules_boot_loaded = 1;
            g_dm.rules_boot_active = 0;
            console_write("[device-manager] boot rules unavailable; using bootstrap rules\n");
            return;
        }
        g_dm.rules_boot_retry_delay = 64;
        return;
    }
    read_len = g_dm_rules_wait.msg.arg0;
    if (read_len < 0) {
        if (g_dm.rules_boot_failures < 255u) {
            g_dm.rules_boot_failures++;
        }
        if (g_dm.rules_boot_failures >= 8u) {
            g_dm.rules_boot_loaded = 1;
            g_dm.rules_boot_active = 0;
            console_write("[device-manager] boot rules unavailable; using bootstrap rules\n");
            return;
        }
        g_dm.rules_boot_retry_delay = 64;
        return;
    }
    if (read_len >= (int32_t)sizeof(text)) {
        read_len = (int32_t)sizeof(text) - 1;
    }
    if (read_len > 0 && wasmos_fs_buffer_copy((int32_t)(uintptr_t)text, read_len, 0) != 0) {
        if (g_dm.rules_boot_failures < 255u) {
            g_dm.rules_boot_failures++;
        }
        if (g_dm.rules_boot_failures >= 8u) {
            g_dm.rules_boot_loaded = 1;
            g_dm.rules_boot_active = 0;
            console_write("[device-manager] boot rules unavailable; using bootstrap rules\n");
            return;
        }
        g_dm.rules_boot_retry_delay = 64;
        return;
    }
    g_dm.rules_boot_failures = 0;
    text[read_len] = '\0';
    dm_rules_load_always_spawn(&g_dm, text);
    dm_rules_load_block_fs(&g_dm, text);
    dm_rules_load_pci_match(&g_dm, text);
    g_dm.rules_boot_active = count_loaded_active_rules();
    if (g_dm.always_spawn_rule_count > 0) {
        console_write("[device-manager] boot rules loaded always_spawn\n");
        queue_always_spawn_rules();
        queue_block_fs_rule_spawns();
    }
    if (g_dm.pci_match_rule_count > 0) {
        console_write("[device-manager] boot rules loaded pci_match\n");
    }
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
            g_dm.rules_init_active = dm_rules_count_active(text);
            dm_rules_load_always_spawn(&g_dm, text);
            dm_rules_load_block_fs(&g_dm, text);
            dm_rules_load_pci_match(&g_dm, text);
            if (g_dm.always_spawn_rule_count > 0) {
                console_write("[device-manager] init rules loaded always_spawn\n");
                queue_always_spawn_rules();
                queue_block_fs_rule_spawns();
            }
            if (g_dm.pci_match_rule_count > 0) {
                console_write("[device-manager] init rules loaded pci_match\n");
            }
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
            /* Rule paths may target drivers only available on /boot. */
            char boot_path[96];
            boot_path[0] = '\0';
            wasmos_sys_strcpy(boot_path, sizeof(boot_path), "/boot/");
            wasmos_sys_str_append(boot_path, sizeof(boot_path), rule_path);
            if (hw_spawn_driver_path(boot_path) == 0) {
                return 0;
            }
            return hw_spawn_driver_path(rule_path);
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
    wasmos_ipc_message_t resp;
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
    if (dm_ipc_call(g_dm.proc_endpoint,
                    g_dm.reply_endpoint,
                    PROC_IPC_MODULE_META_PATH,
                    g_dm.request_id,
                    (int32_t)(uintptr_t)path,
                    (int32_t)path_len,
                    (int32_t)source,
                    0,
                    &resp,
                    128) != 0) {
        return -1;
    }
    g_dm.request_id++;
    if (resp.type != PROC_IPC_RESP) {
        return -1;
    }
    *out_index = resp.arg0;
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
    queue_block_fs_rule_spawns();
}

static void
queue_always_spawn_rules(void)
{
    for (uint32_t i = 0; i < g_dm.always_spawn_rule_count; ++i) {
        always_spawn_rule_t *rule = &g_dm.always_spawn_rules[i];
        if (!rule->active || rule->spawned) {
            continue;
        }
        rule->queued = 1;
    }
}

static void
queue_pci_match_rule_spawns(void)
{
    for (uint32_t ri = 0; ri < g_dm.pci_match_rule_count; ++ri) {
        pci_match_rule_t *rule = &g_dm.pci_match_rules[ri];
        if (!rule->active || rule->spawn_path[0] == '\0') {
            continue;
        }
        for (uint32_t di = 0; di < g_dm.registry_count; ++di) {
            const pci_device_record_t *rec = &g_dm.registry[di];
            if (di >= 64u || ((rule->spawned_device_mask >> di) & 1u) != 0u) {
                continue;
            }
            if ((rule->class_code != MATCH_ANY_U8 && rec->class_code != rule->class_code) ||
                (rule->bus != MATCH_ANY_U8 && rec->bus != rule->bus) ||
                (rule->slot != MATCH_ANY_U8 && rec->device != rule->slot) ||
                (rule->function != MATCH_ANY_U8 && rec->function != rule->function) ||
                (rule->subclass != MATCH_ANY_U8 && rec->subclass != rule->subclass) ||
                (rule->prog_if != MATCH_ANY_U8 && rec->prog_if != rule->prog_if) ||
                (rule->vendor_id != MATCH_ANY_U16 && rec->vendor_id != rule->vendor_id) ||
                (rule->device_id != MATCH_ANY_U16 && rec->device_id != rule->device_id)) {
                continue;
            }
            wasmos_sys_strcpy(g_dm.rule_spawn_path, sizeof(g_dm.rule_spawn_path), rule->spawn_path);
            g_dm.rule_spawn_pending = 1;
            g_dm.rule_spawn_retries = 0;
            g_dm.active_rule_spawn_kind = RULE_SPAWN_KIND_PCI_MATCH;
            g_dm.active_rule_spawn_index = (int32_t)ri;
            g_dm.active_rule_spawn_device_index = (int32_t)di;
            return;
        }
    }
}

static void
queue_block_fs_rule_spawns(void)
{
    if (g_dm.rule_spawn_pending) {
        return;
    }
    for (uint32_t i = 0; i < g_dm.always_spawn_rule_count; ++i) {
        always_spawn_rule_t *rule = &g_dm.always_spawn_rules[i];
        if (!rule->active || !rule->queued || rule->spawned) {
            continue;
        }
        wasmos_sys_strcpy(g_dm.rule_spawn_path, sizeof(g_dm.rule_spawn_path), rule->spawn_path);
        g_dm.rule_spawn_pending = 1;
        g_dm.rule_spawn_retries = 0;
        g_dm.active_rule_spawn_kind = RULE_SPAWN_KIND_ALWAYS;
        g_dm.active_rule_spawn_index = (int32_t)i;
        return;
    }
    if (g_dm.rule_spawn_pending) {
        return;
    }
    queue_pci_match_rule_spawns();
    if (g_dm.rule_spawn_pending) {
        return;
    }
    for (uint32_t i = 0; i < g_dm.block_fs_rule_count; ++i) {
        block_fs_rule_t *rule = &g_dm.block_fs_rules[i];
        if (!rule->active || !rule->queued || rule->spawned) {
            continue;
        }
        if (is_mount_already_active(rule->mount)) {
            rule->queued = 0;
            rule->spawned = 1;
            log_mount_already_active(rule->mount);
            continue;
        }
        wasmos_sys_strcpy(g_dm.rule_spawn_path, sizeof(g_dm.rule_spawn_path), rule->spawn_path);
        g_dm.rule_spawn_pending = 1;
        g_dm.rule_spawn_retries = 0;
        g_dm.active_rule_spawn_kind = RULE_SPAWN_KIND_BLOCK_FS;
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
            if (is_mount_already_active(rule->mount)) {
                rule->spawned = 1;
                log_mount_already_active(rule->mount);
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
            if (is_mount_already_active(rule->mount)) {
                rule->spawned = 1;
                log_mount_already_active(rule->mount);
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
    wasmos_ipc_message_t resp;
    if (!out_class_code || !out_subclass || !out_prog_if ||
        !out_vendor_id || !out_device_id || !out_storage_bootstrap || !out_match_count || !out_caps) {
        return -1;
    }
    if (dm_ipc_call(g_dm.proc_endpoint,
                    g_dm.reply_endpoint,
                    PROC_IPC_MODULE_META,
                    g_dm.request_id,
                    module_index,
                    (int32_t)match_index,
                    0,
                    0,
                    &resp,
                    128) != 0) {
        return -1;
    }
    g_dm.request_id++;
    if (resp.type != PROC_IPC_RESP) {
        return -1;
    }
    uint32_t arg0 = (uint32_t)resp.arg0;
    uint32_t arg1 = (uint32_t)resp.arg1;
    uint32_t arg2 = (uint32_t)resp.arg2;
    uint32_t arg3 = (uint32_t)resp.arg3;
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

static void
dm_handle_proc_spawn_reply(void *user, const wasmos_ipc_message_t *msg)
{
    (void)user;
    if (!msg || !g_dm_spawn_wait.pending) {
        return;
    }
    if (msg->request_id != g_dm.request_id) {
        return;
    }
    if (msg->type != PROC_IPC_RESP && msg->type != PROC_IPC_ERROR) {
        return;
    }
    g_dm_spawn_wait.msg = *msg;
    g_dm_spawn_wait.done = 1;
}

static void
dm_handle_ipc_default(void *user, const wasmos_ipc_message_t *msg)
{
    (void)user;
    (void)msg;
}

static int
dm_register_ipc_handlers(void)
{
    if (wasmos_sys_event_register(&g_dm_ipc_loop, PROC_IPC_RESP, dm_handle_proc_spawn_reply, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_register(&g_dm_ipc_loop, PROC_IPC_ERROR, dm_handle_proc_spawn_reply, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_set_default(&g_dm_ipc_loop, dm_handle_ipc_default, 0) != 0) {
        return -1;
    }
    return 0;
}

static void
dm_handle_inventory_message(void *user, const wasmos_ipc_message_t *msg)
{
    (void)user;
    if (!msg) {
        return;
    }
    if (msg->type == DEVMGR_PUBLISH_BLOCK_DEVICE) {
        registry_add_block_from_ipc(msg->arg0, msg->arg1, msg->arg2, msg->arg3);
        return;
    }
    if (msg->type == DEVMGR_PUBLISH_DEVICE) {
        registry_add_from_ipc(msg->arg0, msg->arg1, msg->arg2, msg->arg3);
        return;
    }
    if (msg->type == DEVMGR_PCI_SCAN_DONE) {
        g_dm_pci_scan_done = 1;
        return;
    }
    if (msg->type == DEVMGR_QUERY_MOUNT_REQ || msg->type == DEVMGR_QUERY_BLOCK_MOUNT_REQ) {
        handle_query_message_fields(msg);
        return;
    }
}

static int
dm_register_inventory_handlers(void)
{
    if (wasmos_sys_event_register(&g_dm_inventory_loop, DEVMGR_PUBLISH_BLOCK_DEVICE, dm_handle_inventory_message, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_register(&g_dm_inventory_loop, DEVMGR_PUBLISH_DEVICE, dm_handle_inventory_message, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_register(&g_dm_inventory_loop, DEVMGR_PCI_SCAN_DONE, dm_handle_inventory_message, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_register(&g_dm_inventory_loop, DEVMGR_QUERY_MOUNT_REQ, dm_handle_inventory_message, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_register(&g_dm_inventory_loop, DEVMGR_QUERY_BLOCK_MOUNT_REQ, dm_handle_inventory_message, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_set_default(&g_dm_inventory_loop, dm_handle_ipc_default, 0) != 0) {
        return -1;
    }
    return 0;
}

static int
dm_register_rules_handlers(void)
{
    if (wasmos_sys_event_register(&g_dm_rules_loop, FS_IPC_RESP, dm_handle_rules_reply, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_register(&g_dm_rules_loop, FS_IPC_ERROR, dm_handle_rules_reply, 0) != 0) {
        return -1;
    }
    if (wasmos_sys_event_set_default(&g_dm_rules_loop, dm_handle_ipc_default, 0) != 0) {
        return -1;
    }
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
    g_dm_pci_scan_done = 0;
    console_write("[device-manager] waiting for pci-bus inventory\n");
    while (!g_dm_pci_scan_done) {
        int32_t handled = wasmos_sys_event_loop_poll(&g_dm_inventory_loop, 16);
        if (handled < 0) {
            console_write("[device-manager] pci inventory recv failed\n");
            break;
        }
        if (handled == 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (g_dm_pci_scan_done) {
        console_write("[device-manager] pci-bus scan complete\n");
    }
    apply_pci_matches();
    queue_block_fs_rule_spawns();
}

static void
consume_inventory_events_nonblocking(void)
{
    if (g_dm.inventory_endpoint < 0) {
        return;
    }
    (void)wasmos_sys_event_loop_poll(&g_dm_inventory_loop, 16);
}

static hw_spawn_target_t
next_spawn_target(void)
{
    if (g_dm.need_pci_bus) {
        return HW_SPAWN_PCI_BUS;
    }
    if (g_dm.rule_spawn_pending && g_dm.rule_spawn_path[0] != '\0') {
        return HW_SPAWN_RULE_PATH;
    }
    if (g_dm.need_fat) {
        return HW_SPAWN_FAT;
    }
    if (!g_dm.boot_mount_ready) {
        return HW_SPAWN_NONE;
    }
    if (g_dm.need_serial) {
        return HW_SPAWN_SERIAL;
    }
    return HW_SPAWN_NONE;
}

static void
handle_query_message_fields(const wasmos_ipc_message_t *msg)
{
    int32_t type = msg ? msg->type : 0;
    int32_t req_id = msg ? msg->request_id : 0;
    int32_t source = msg ? msg->source : 0;
    int32_t index = msg ? msg->arg0 : 0;
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
    (void)wasmos_sys_event_loop_poll(&g_dm_inventory_loop, 16);
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
    wasmos_sys_event_loop_init(&g_dm_ipc_loop, g_dm.reply_endpoint, 0xD000);
    if (dm_register_ipc_handlers() != 0) {
        g_dm.phase = HW_PHASE_FAILED;
        console_write("[device-manager] ipc handler registration failed\n");
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
    wasmos_sys_event_loop_init(&g_dm_inventory_loop, g_dm.inventory_endpoint, 0xE000);
    if (dm_register_inventory_handlers() != 0) {
        console_write("[device-manager] inventory handler registration failed\n");
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
    wasmos_sys_event_loop_init(&g_dm_rules_loop, g_dm.rule_reply_endpoint, 0xE100);
    if (dm_register_rules_handlers() != 0) {
        console_write("[device-manager] rules handler registration failed\n");
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
    (void)query_module_meta_by_path("system/drivers/serial.wap", PROC_MODULE_SOURCE_FS, &g_dm.serial_index);
    if (g_dm.serial_index < 0) {
        g_dm.serial_index = module_index_by_name("serial");
    }
    /* Bootstrap policy: always bring up pci-bus when present in initfs.
     * Runtime dedup/restart policy is handled by later lifecycle phases. */
    g_dm.need_pci_bus = (g_dm.pci_bus_index >= 0) ? 1 : 0;
    g_dm.need_fat = 0;
    g_dm.need_fs_init = 0;
    g_dm.need_fs_manager = 0;
    /* Serial is resolved from PCI inventory matches when pci-bus is started
     * by this service; if pci-bus is already running, keep legacy fallback. */
    g_dm.need_serial = g_dm.need_pci_bus ? 0 : (!proc_running("serial") ? 1 : 0);
    g_dm.phase = HW_PHASE_SPAWN;

    for (;;) {
        handle_query_endpoint();
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
            } else if (target == HW_SPAWN_FAT) {
                if (hw_spawn_driver_index(g_dm.fat_index) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn fs-fat failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
            }
            g_dm.pending = target;
            g_dm_spawn_wait.pending = 1;
            g_dm_spawn_wait.done = 0;
            g_dm.phase = HW_PHASE_WAIT;
            continue;
        }

        if (g_dm.phase == HW_PHASE_WAIT) {
            while (!g_dm_spawn_wait.done) {
                int32_t handled = wasmos_sys_event_loop_poll(&g_dm_ipc_loop, 16);
                if (handled < 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn recv failed\n");
                    wasmos_sys_ipc_recv_loop();
                }
                if (handled == 0) {
                    (void)wasmos_sched_yield();
                }
            }
            g_dm_spawn_wait.pending = 0;

            int32_t resp_type = g_dm_spawn_wait.msg.type;
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
                    if (g_dm.active_rule_spawn_kind == RULE_SPAWN_KIND_ALWAYS) {
                        if (g_dm.active_rule_spawn_index >= 0 &&
                            g_dm.active_rule_spawn_index < (int32_t)g_dm.always_spawn_rule_count) {
                            always_spawn_rule_t *rule = &g_dm.always_spawn_rules[g_dm.active_rule_spawn_index];
                            rule->spawned = 1;
                            rule->queued = 0;
                        }
                    } else if (g_dm.active_rule_spawn_kind == RULE_SPAWN_KIND_BLOCK_FS) {
                        if (g_dm.active_rule_spawn_index >= 0 &&
                            g_dm.active_rule_spawn_index < (int32_t)g_dm.block_fs_rule_count) {
                            block_fs_rule_t *rule = &g_dm.block_fs_rules[g_dm.active_rule_spawn_index];
                            rule->spawned = 1;
                            rule->queued = 0;
                            if (wasmos_sys_streq(rule->mount, "/boot")) {
                                g_dm.boot_mount_ready = 1;
                                queue_always_spawn_rules();
                            } else if (wasmos_sys_streq(rule->mount, "/user")) {
                                g_dm.user_mount_ready = 1;
                            }
                        }
                    } else if (g_dm.active_rule_spawn_kind == RULE_SPAWN_KIND_PCI_MATCH) {
                        if (g_dm.active_rule_spawn_index >= 0 &&
                            g_dm.active_rule_spawn_index < (int32_t)g_dm.pci_match_rule_count) {
                            if (g_dm.active_rule_spawn_device_index >= 0 &&
                                g_dm.active_rule_spawn_device_index < 64) {
                                g_dm.pci_match_rules[g_dm.active_rule_spawn_index].spawned_device_mask |=
                                    (uint64_t)1u << (uint32_t)g_dm.active_rule_spawn_device_index;
                            }
                        }
                    }
                    g_dm.active_rule_spawn_kind = RULE_SPAWN_KIND_NONE;
                    g_dm.active_rule_spawn_index = -1;
                    g_dm.active_rule_spawn_device_index = -1;
                    queue_block_fs_rule_spawns();
                }
                if (g_dm.pending == HW_SPAWN_SERIAL) {
                    g_dm.need_serial = 0;
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
                int32_t resp_code = g_dm_spawn_wait.msg.arg1;
                if (resp_code == -2) {
                    if (g_dm.pending == HW_SPAWN_RULE_PATH &&
                        g_dm.active_rule_spawn_kind == RULE_SPAWN_KIND_ALWAYS &&
                        g_dm.active_rule_spawn_index >= 0 &&
                        g_dm.active_rule_spawn_index < (int32_t)g_dm.always_spawn_rule_count) {
                        g_dm.always_spawn_rules[g_dm.active_rule_spawn_index].queued = 0;
                    }
                    g_dm.rule_spawn_pending = 0;
                    g_dm.active_rule_spawn_kind = RULE_SPAWN_KIND_NONE;
                    g_dm.active_rule_spawn_index = -1;
                    g_dm.active_rule_spawn_device_index = -1;
                    queue_block_fs_rule_spawns();
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
                        if (g_dm.active_rule_spawn_kind == RULE_SPAWN_KIND_ALWAYS) {
                            if (g_dm.active_rule_spawn_index >= 0 &&
                                g_dm.active_rule_spawn_index < (int32_t)g_dm.always_spawn_rule_count) {
                                g_dm.always_spawn_rules[g_dm.active_rule_spawn_index].queued = 0;
                            }
                        } else if (g_dm.active_rule_spawn_kind == RULE_SPAWN_KIND_BLOCK_FS) {
                            if (g_dm.active_rule_spawn_index >= 0 &&
                                g_dm.active_rule_spawn_index < (int32_t)g_dm.block_fs_rule_count) {
                                g_dm.block_fs_rules[g_dm.active_rule_spawn_index].queued = 0;
                            }
                        }
                        g_dm.active_rule_spawn_kind = RULE_SPAWN_KIND_NONE;
                        g_dm.active_rule_spawn_index = -1;
                        g_dm.active_rule_spawn_device_index = -1;
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
            (void)wasmos_sys_event_loop_poll(&g_dm_ipc_loop, 4);
            if (next_spawn_target() != HW_SPAWN_NONE) {
                g_dm.phase = HW_PHASE_SPAWN;
                continue;
            }
            wasmos_sched_yield();
            continue;
        }

        console_write("[device-manager] failed\n");
        wasmos_sys_ipc_recv_loop();
    }
}
static void
dm_handle_rules_reply(void *user, const wasmos_ipc_message_t *msg)
{
    (void)user;
    if (!msg || !g_dm_rules_wait.pending) {
        return;
    }
    if (msg->request_id != g_dm.rules_boot_request_id) {
        return;
    }
    g_dm_rules_wait.msg = *msg;
    g_dm_rules_wait.done = 1;
}
