#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
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
    HW_SPAWN_PCI_BUS,
    HW_SPAWN_ATA,
    HW_SPAWN_FAT,
    HW_SPAWN_FS_INIT,
    HW_SPAWN_FS_MANAGER,
    HW_SPAWN_SERIAL,
    HW_SPAWN_KEYBOARD,
    HW_SPAWN_FRAMEBUFFER
} hw_spawn_target_t;

#define DEVICE_REGISTRY_CAP 64
#define MATCH_ANY_U8 0xFFu
#define MATCH_ANY_U16 0xFFFFu

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
    hw_phase_t phase;
    hw_spawn_target_t pending;
    int32_t reply_endpoint;
    int32_t proc_endpoint;
    int32_t inventory_endpoint;
    int32_t query_endpoint;
    int32_t request_id;
    int32_t module_count;
    uint8_t need_pci_bus;
    uint8_t need_storage;
    uint8_t need_fat;
    uint8_t need_serial;
    uint8_t need_fs_init;
    uint8_t need_fs_manager;
    uint8_t need_keyboard;
    uint8_t need_framebuffer;
    uint8_t storage_retries;
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
    int32_t selected_storage_index;
    spawn_caps_t selected_storage_caps;
    spawn_caps_t selected_serial_caps;
    uint8_t selected_storage_has_record;
    pci_device_record_t selected_storage_record;
} device_manager_state_t;

static device_manager_state_t g_dm = {
    .phase = HW_PHASE_INIT,
    .pending = HW_SPAWN_NONE,
    .reply_endpoint = -1,
    .proc_endpoint = -1,
    .inventory_endpoint = -1,
    .query_endpoint = -1,
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
};

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

static int
str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
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
        if (str_eq(buf, name)) {
            return 1;
        }
    }
    return 0;
}

static int
module_index_by_name(const char *name)
{
    if (!name || g_dm.module_count <= 0) {
        return -1;
    }
    for (int32_t i = 0; i < g_dm.module_count; ++i) {
        char buf[32];
        buf[0] = '\0';
        if (wasmos_boot_module_name(i, (int32_t)(uintptr_t)buf, (int32_t)sizeof(buf)) < 0) {
            continue;
        }
        if (wasmos_sync_user_read((int32_t)(uintptr_t)buf, (int32_t)sizeof(buf)) != 0) {
            continue;
        }
        if (str_eq(buf, name)) {
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
    uint32_t flags = 0;
    uint32_t io_packed = 0;
    uint32_t irq_mask = 0;
    if (!caps) {
        return hw_spawn_driver_index(index);
    }
    flags = caps->cap_flags;
    io_packed = ((uint32_t)caps->io_port_min) | ((uint32_t)caps->io_port_max << 16);
    irq_mask = caps->irq_mask;
    if (index < 0) {
        return -1;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_SPAWN_CAPS,
                        g_dm.request_id,
                        index,
                        (int32_t)flags,
                        (int32_t)io_packed,
                        (int32_t)irq_mask) != 0) {
        return -1;
    }
    return 0;
}

static int
hw_spawn_driver_name(hw_spawn_target_t target)
{
    const char *name = 0;
    uint32_t packed[4] = {0, 0, 0, 0};
    if (target == HW_SPAWN_SERIAL) {
        name = "serial";
    } else if (target == HW_SPAWN_KEYBOARD) {
        name = "keyboard";
    } else if (target == HW_SPAWN_FRAMEBUFFER) {
        name = "framebuffer";
    }
    if (!name) {
        return -1;
    }
    for (uint32_t i = 0; name[i] && i < 16u; ++i) {
        uint32_t slot = i / 4u;
        uint32_t shift = (i % 4u) * 8u;
        packed[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
    if (wasmos_ipc_send(g_dm.proc_endpoint,
                        g_dm.reply_endpoint,
                        PROC_IPC_SPAWN_NAME,
                        g_dm.request_id,
                        (int32_t)packed[0],
                        (int32_t)packed[1],
                        (int32_t)packed[2],
                        (int32_t)packed[3]) != 0) {
        return -1;
    }
    return 0;
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
        if (msg_type == DEVMGR_PCI_SCAN_DONE) {
            console_write("[device-manager] pci-bus scan complete\n");
            break;
        }
    }
    apply_pci_matches();
}

static int
select_fallback_storage_driver(void)
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
        g_dm.selected_storage_index = module_index;
        g_dm.selected_storage_caps = caps;
        return 0;
    }
    return -1;
}

static hw_spawn_target_t
next_spawn_target(void)
{
    if (g_dm.need_pci_bus) {
        return HW_SPAWN_PCI_BUS;
    }
    if (g_dm.need_storage) {
        return HW_SPAWN_ATA;
    }
    if (g_dm.need_fs_manager) {
        return HW_SPAWN_FS_MANAGER;
    }
    if (g_dm.need_fat) {
        return HW_SPAWN_FAT;
    }
    if (g_dm.need_fs_init) {
        return HW_SPAWN_FS_INIT;
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

static int
has_pending_spawn_needs(void)
{
    return g_dm.need_pci_bus || g_dm.need_storage || g_dm.need_fat ||
           g_dm.need_fs_init || g_dm.need_fs_manager || g_dm.need_serial ||
           g_dm.need_keyboard || g_dm.need_framebuffer;
}

static void
handle_query_endpoint(void)
{
    if (g_dm.query_endpoint < 0) {
        return;
    }
    if (wasmos_ipc_recv(g_dm.query_endpoint) < 0) {
        return;
    }
    int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
    int32_t index = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (type != DEVMGR_QUERY_MOUNT_REQ) {
        (void)wasmos_ipc_send(source, g_dm.query_endpoint, FS_IPC_ERROR, req_id, type, 0, 0, 0);
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
        (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_MOUNT_INFO, req_id, 1, 0, 0, 0);
        return;
    }
    (void)wasmos_ipc_send(source, g_dm.query_endpoint, DEVMGR_QUERY_DONE, req_id, 0, 0, 0, 0);
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
        stall_forever();
    }
    if (proc_endpoint < 0 || module_count <= 0) {
        g_dm.phase = HW_PHASE_FAILED;
        console_write("[device-manager] invalid init args\n");
        stall_forever();
    }
    g_dm.proc_endpoint = proc_endpoint;
    g_dm.module_count = module_count;
    g_dm.inventory_endpoint = wasmos_ipc_create_endpoint();
    if (g_dm.inventory_endpoint < 0) {
        console_write("[device-manager] inventory endpoint create failed\n");
        stall_forever();
    }
    if (wasmos_svc_register(g_dm.proc_endpoint, g_dm.inventory_endpoint, "devmgr.inv", 1) != 0) {
        console_write("[device-manager] inventory register failed\n");
        stall_forever();
    }
    g_dm.query_endpoint = wasmos_ipc_create_endpoint();
    if (g_dm.query_endpoint < 0 ||
        wasmos_svc_register(g_dm.proc_endpoint, g_dm.query_endpoint, "devmgr.query", 1) != 0) {
        console_write("[device-manager] query register failed\n");
        stall_forever();
    }
    hw_scan_acpi();
    g_dm.pci_bus_index = module_index_by_name("pci-bus");
    g_dm.fat_index = module_index_by_name("fs-fat");
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
    (void)query_module_meta_by_path("/boot/system/drivers/framebuffer.wap", PROC_MODULE_SOURCE_FS, &g_dm.framebuffer_index);

    g_dm.need_pci_bus = (g_dm.pci_bus_index >= 0 && !proc_running("pci-bus")) ? 1 : 0;
    g_dm.need_storage = 0;
    g_dm.need_fat = 0;
    g_dm.need_fs_init = 0;
    g_dm.need_fs_manager = (g_dm.fs_manager_index >= 0 && !proc_running("fs-manager")) ? 1 : 0;
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
                if (has_pending_spawn_needs()) {
                    wasmos_sched_yield();
                    continue;
                }
                g_dm.phase = HW_PHASE_IDLE;
                continue;
            }
            if (target == HW_SPAWN_PCI_BUS) {
                spawn_caps_t pci_caps;
                pci_caps.cap_flags = DEVMGR_CAP_IO_PORT;
                pci_caps.io_port_min = 0x0CF8;
                pci_caps.io_port_max = 0x0CFF;
                pci_caps.irq_mask = 0;
                if (hw_spawn_driver_index_caps(g_dm.pci_bus_index, &pci_caps) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn pci-bus failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_SERIAL) {
                if (hw_spawn_driver_index_caps(g_dm.serial_index, &g_dm.selected_serial_caps) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn serial failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_KEYBOARD) {
                if ((g_dm.keyboard_index >= 0 && hw_spawn_driver_index(g_dm.keyboard_index) != 0) ||
                    (g_dm.keyboard_index < 0 && hw_spawn_driver_name(target) != 0)) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn keyboard failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_FRAMEBUFFER) {
                if ((g_dm.framebuffer_index >= 0 && hw_spawn_driver_index(g_dm.framebuffer_index) != 0) ||
                    (g_dm.framebuffer_index < 0 && hw_spawn_driver_name(target) != 0)) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn framebuffer failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_ATA) {
                if (hw_spawn_driver_index_caps(g_dm.selected_storage_index, &g_dm.selected_storage_caps) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn storage driver failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_FAT) {
                if (hw_spawn_driver_index(g_dm.fat_index) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn fs-fat failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_FS_INIT) {
                if (hw_spawn_driver_index(g_dm.fs_init_index) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn fs-init failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_FS_MANAGER) {
                if (hw_spawn_driver_index(g_dm.fs_manager_index) != 0) {
                    g_dm.phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn fs-manager failed\n");
                    stall_forever();
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
                stall_forever();
            }

            int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
            int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            if (resp_req != g_dm.request_id) {
                g_dm.phase = HW_PHASE_FAILED;
                console_write("[device-manager] spawn response mismatch\n");
                stall_forever();
            }
            g_dm.request_id++;
            if (resp_type == PROC_IPC_RESP) {
                if (g_dm.pending == HW_SPAWN_PCI_BUS) {
                    g_dm.need_pci_bus = 0;
                    g_dm.pending = HW_SPAWN_NONE;
                    g_dm.phase = HW_PHASE_WAIT_INVENTORY;
                    continue;
                }
                if (g_dm.pending == HW_SPAWN_SERIAL) {
                    g_dm.need_serial = 0;
                } else if (g_dm.pending == HW_SPAWN_KEYBOARD) {
                    g_dm.need_keyboard = 0;
                } else if (g_dm.pending == HW_SPAWN_FRAMEBUFFER) {
                    g_dm.need_framebuffer = 0;
                } else if (g_dm.pending == HW_SPAWN_ATA) {
                    g_dm.need_storage = 0;
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
                } else if (g_dm.pending == HW_SPAWN_ATA) {
                    g_dm.storage_retries++;
                    if (g_dm.storage_retries > 8) {
                        g_dm.need_storage = 0;
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
                }
                g_dm.pending = HW_SPAWN_NONE;
                g_dm.phase = HW_PHASE_SPAWN;
                continue;
            }

            g_dm.phase = HW_PHASE_FAILED;
            console_write("[device-manager] spawn response invalid\n");
            stall_forever();
        }

        if (g_dm.phase == HW_PHASE_WAIT_INVENTORY) {
            consume_pci_inventory();
            g_dm.need_storage = (g_dm.selected_storage_index >= 0) ? 1 : 0;
            g_dm.need_fat = (g_dm.fat_index >= 0 && g_dm.need_storage && !proc_running("fs-fat")) ? 1 : 0;
            g_dm.need_fs_init = (g_dm.fs_init_index >= 0 && !proc_running("fs-init")) ? 1 : 0;
            g_dm.need_fs_manager = (g_dm.fs_manager_index >= 0 && !proc_running("fs-manager")) ? 1 : 0;
            if (g_dm.serial_index >= 0 &&
                !proc_running("serial") &&
                select_pci_matched_driver(g_dm.serial_index, &g_dm.selected_serial_caps) == 0) {
                g_dm.need_serial = 1;
            } else {
                g_dm.need_serial = 0;
            }
            if (!g_dm.need_storage && select_fallback_storage_driver() == 0) {
                console_write("[device-manager] fallback: boot storage driver despite no PCI match\n");
                g_dm.need_storage = 1;
                g_dm.need_fat = (g_dm.fat_index >= 0 && !proc_running("fs-fat")) ? 1 : 0;
                g_dm.need_fs_init = (g_dm.fs_init_index >= 0 && !proc_running("fs-init")) ? 1 : 0;
                g_dm.need_fs_manager = (g_dm.fs_manager_index >= 0 && !proc_running("fs-manager")) ? 1 : 0;
            }
            g_dm.phase = HW_PHASE_SPAWN;
            continue;
        }

        if (g_dm.phase == HW_PHASE_IDLE) {
            if (next_spawn_target() != HW_SPAWN_NONE) {
                g_dm.phase = HW_PHASE_SPAWN;
                continue;
            }
            handle_query_endpoint();
            continue;
        }

        console_write("[device-manager] failed\n");
        stall_forever();
    }
}
