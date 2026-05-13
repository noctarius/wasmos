#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

/*
 * device-manager is currently a bootstrap sequencer more than a full device
 * manager. It verifies the ACPI RSDP is present, finds the bootstrap storage
 * driver modules, and asks the process manager to start ATA/FAT in dependency
 * order. Once FAT is available, it starts display and input drivers by name
 * from disk; the kernel early framebuffer is sufficient before that point.
 * TODO: Grow this into a real hardware inventory/policy service instead of a
 * storage-bootstrap sequencer with hardcoded ATA/FAT assumptions.
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
    HW_PHASE_IDLE,
    HW_PHASE_FAILED
} hw_phase_t;

typedef enum {
    HW_SPAWN_NONE = 0,
    HW_SPAWN_SERIAL,
    HW_SPAWN_KEYBOARD,
    HW_SPAWN_FRAMEBUFFER,
    HW_SPAWN_ATA,
    HW_SPAWN_FAT
} hw_spawn_target_t;

static hw_phase_t g_phase = HW_PHASE_INIT;
static hw_spawn_target_t g_pending = HW_SPAWN_NONE;
static int32_t g_reply_endpoint = -1;
static int32_t g_proc_endpoint = -1;
static int32_t g_request_id = 1;
static int32_t g_module_count = 0;
static uint8_t g_need_ata = 0;
static uint8_t g_need_fat = 0;
static uint8_t g_need_serial = 0;
static uint8_t g_need_keyboard = 0;
static uint8_t g_need_framebuffer = 0;
static uint8_t g_ata_retries = 0;
static uint8_t g_fat_retries = 0;
static uint8_t g_serial_retries = 0;
static uint8_t g_keyboard_retries = 0;
static uint8_t g_framebuffer_retries = 0;
static int32_t g_ata_index = -1;
static int32_t g_fat_index = -1;

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

static uint32_t
str_len(const char *s)
{
    return (uint32_t)strlen(s);
}

static void
console_write(const char *s)
{
    if (!s) {
        return;
    }
    uint32_t len = str_len(s);
    if (len == 0) {
        return;
    }
    wasmos_console_write((int32_t)(uintptr_t)s, (int32_t)len);
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
    if (!name || g_module_count <= 0) {
        return -1;
    }
    for (int32_t i = 0; i < g_module_count; ++i) {
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
    /* For now the service only validates that the bootloader passed a usable
     * RSDP. Richer ACPI parsing and device publication are future work. */
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
    if (wasmos_ipc_send(g_proc_endpoint,
                        g_reply_endpoint,
                        PROC_IPC_SPAWN,
                        g_request_id,
                        index,
                        0,
                        0,
                        0) != 0) {
        return -1;
    }
    return 0;
}

static void
pack_name_args(const char *name, uint32_t out[4])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    if (!name) {
        return;
    }
    for (uint32_t i = 0; name[i] && i < 16; ++i) {
        uint32_t slot = i / 4;
        uint32_t shift = (i % 4) * 8;
        out[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
}

static const char *
target_name(hw_spawn_target_t target)
{
    if (target == HW_SPAWN_SERIAL) {
        return "serial";
    }
    if (target == HW_SPAWN_KEYBOARD) {
        return "keyboard";
    }
    if (target == HW_SPAWN_FRAMEBUFFER) {
        return "framebuffer";
    }
    return "";
}

static int
hw_spawn_driver_name(hw_spawn_target_t target)
{
    const char *name = target_name(target);
    uint32_t packed[4];
    if (!name || name[0] == '\0') {
        return -1;
    }
    pack_name_args(name, packed);
    if (wasmos_ipc_send(g_proc_endpoint,
                        g_reply_endpoint,
                        PROC_IPC_SPAWN_NAME,
                        g_request_id,
                        (int32_t)packed[0],
                        (int32_t)packed[1],
                        (int32_t)packed[2],
                        (int32_t)packed[3]) != 0) {
        return -1;
    }
    return 0;
}

static hw_spawn_target_t
next_spawn_target(void)
{
    if (g_need_ata) {
        return HW_SPAWN_ATA;
    }
    if (g_need_fat) {
        return HW_SPAWN_FAT;
    }
    if (g_need_serial) {
        return HW_SPAWN_SERIAL;
    }
    if (g_need_keyboard) {
        return HW_SPAWN_KEYBOARD;
    }
    if (g_need_framebuffer) {
        return HW_SPAWN_FRAMEBUFFER;
    }
    return HW_SPAWN_NONE;
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t module_count,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        g_phase = HW_PHASE_FAILED;
        console_write("[device-manager] failed to create reply endpoint\n");
        stall_forever();
    }
    if (proc_endpoint < 0 || module_count <= 0) {
        g_phase = HW_PHASE_FAILED;
        console_write("[device-manager] invalid init args\n");
        stall_forever();
    }
    g_proc_endpoint = proc_endpoint;
    g_module_count = module_count;
    hw_scan_acpi();
    g_ata_index = module_index_by_name("ata");
    g_fat_index = module_index_by_name("fs-fat");
    g_need_ata = (g_ata_index >= 0 && !proc_running("ata")) ? 1 : 0;
    g_need_fat = (g_fat_index >= 0 && !proc_running("fs-fat")) ? 1 : 0;
    g_need_serial = !proc_running("serial") ? 1 : 0;
    g_need_keyboard = !proc_running("keyboard") ? 1 : 0;
    g_need_framebuffer = !proc_running("framebuffer") ? 1 : 0;
    g_phase = HW_PHASE_SPAWN;

    for (;;) {
        if (g_phase == HW_PHASE_SPAWN) {
            hw_spawn_target_t target = next_spawn_target();
            if (target == HW_SPAWN_NONE) {
                g_phase = HW_PHASE_IDLE;
                continue;
            }
            if (target == HW_SPAWN_SERIAL) {
                if (hw_spawn_driver_name(target) != 0) {
                    g_phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn serial failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_KEYBOARD) {
                if (hw_spawn_driver_name(target) != 0) {
                    g_phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn keyboard failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_FRAMEBUFFER) {
                if (hw_spawn_driver_name(target) != 0) {
                    g_phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn framebuffer failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_ATA) {
                if (hw_spawn_driver_index(g_ata_index) != 0) {
                    g_phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn ata failed\n");
                    stall_forever();
                }
            } else if (target == HW_SPAWN_FAT) {
                if (hw_spawn_driver_index(g_fat_index) != 0) {
                    g_phase = HW_PHASE_FAILED;
                    console_write("[device-manager] spawn fs-fat failed\n");
                    stall_forever();
                }
            }
            g_pending = target;
            g_phase = HW_PHASE_WAIT;
            continue;
        }

        if (g_phase == HW_PHASE_WAIT) {
            int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
            if (recv_rc < 0) {
                g_phase = HW_PHASE_FAILED;
                console_write("[device-manager] spawn recv failed\n");
                stall_forever();
            }

            int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
            int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            if (resp_req != g_request_id) {
                g_phase = HW_PHASE_FAILED;
                console_write("[device-manager] spawn response mismatch\n");
                stall_forever();
            }
            g_request_id++;
            if (resp_type == PROC_IPC_RESP) {
                if (g_pending == HW_SPAWN_SERIAL) {
                    g_need_serial = 0;
                } else if (g_pending == HW_SPAWN_KEYBOARD) {
                    g_need_keyboard = 0;
                } else if (g_pending == HW_SPAWN_FRAMEBUFFER) {
                    g_need_framebuffer = 0;
                } else if (g_pending == HW_SPAWN_ATA) {
                    g_need_ata = 0;
                } else if (g_pending == HW_SPAWN_FAT) {
                    g_need_fat = 0;
                }
                g_pending = HW_SPAWN_NONE;
                g_phase = HW_PHASE_SPAWN;
                continue;
            }
            if (resp_type == PROC_IPC_ERROR) {
                if (g_pending == HW_SPAWN_SERIAL) {
                    g_serial_retries++;
                    if (g_serial_retries > 8) {
                        g_need_serial = 0;
                    }
                } else if (g_pending == HW_SPAWN_KEYBOARD) {
                    g_keyboard_retries++;
                    if (g_keyboard_retries > 8) {
                        g_need_keyboard = 0;
                    }
                } else if (g_pending == HW_SPAWN_FRAMEBUFFER) {
                    g_framebuffer_retries++;
                    if (g_framebuffer_retries > 8) {
                        g_need_framebuffer = 0;
                    }
                } else if (g_pending == HW_SPAWN_ATA) {
                    g_ata_retries++;
                    if (g_ata_retries > 8) {
                        g_need_ata = 0;
                    }
                } else if (g_pending == HW_SPAWN_FAT) {
                    g_fat_retries++;
                    if (g_fat_retries > 8) {
                        g_need_fat = 0;
                    }
                }
                g_pending = HW_SPAWN_NONE;
                g_phase = HW_PHASE_SPAWN;
                continue;
            }

            g_phase = HW_PHASE_FAILED;
            console_write("[device-manager] spawn response invalid\n");
            stall_forever();
        }

        if (g_phase == HW_PHASE_IDLE) {
            (void)wasmos_ipc_recv(g_reply_endpoint);
            continue;
        }

        console_write("[device-manager] failed\n");
        stall_forever();
    }
}
