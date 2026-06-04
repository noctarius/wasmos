/* device_manager_types.h - structs, enums, and constants shared by the
 * device-manager service and its rule parser */
#ifndef WASMOS_DEVICE_MANAGER_TYPES_H
#define WASMOS_DEVICE_MANAGER_TYPES_H

#include <stdint.h>

#define DEVICE_REGISTRY_CAP 64
#define BLOCK_REGISTRY_CAP 16
#define MATCH_ANY_U8 0xFFu    /* wildcard for class/subclass byte fields */
#define MATCH_ANY_U16 0xFFFFu /* wildcard for vendor/device ID fields */
#define DEVMGR_RULES_INIT_ROOT "/init/devmgr/rules"
#define DEVMGR_RULES_BOOT_ROOT "/boot/system/devmgr/rules"
#define DEVMGR_RULE_FILE "default.rules"
#define DEVMGR_RULE_TEXT_CAP 1024
#define ALWAYS_SPAWN_RULE_CAP 8
#define BLOCK_FS_RULE_CAP 8
#define PCI_MATCH_RULE_CAP 8
#define ACPI_MATCH_RULE_CAP 8

/* ACPI RSDP v2 in-memory layout (packed; not endian-converted). */
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

/* State-machine phases of the device-manager main loop. */
typedef enum {
    HW_PHASE_INIT = 0,
    HW_PHASE_SPAWN,
    HW_PHASE_WAIT,
    HW_PHASE_WAIT_INVENTORY,       /* waiting for PCI scan-done notification */
    HW_PHASE_WAIT_ACPI_INVENTORY,  /* waiting for ACPI scan-done notification */
    HW_PHASE_IDLE,
    HW_PHASE_FAILED
} hw_phase_t;

/* Which subsystem is currently being spawned (one at a time). */
typedef enum {
    HW_SPAWN_NONE = 0,
    HW_SPAWN_RULE_PATH,  /* spawning a rule-driven driver */
    HW_SPAWN_PCI_BUS,
    HW_SPAWN_ACPI_BUS,
    HW_SPAWN_FAT,
    HW_SPAWN_FS_INIT,
    HW_SPAWN_FS_MANAGER,
} hw_spawn_target_t;

/* I/O capabilities granted to a spawned driver process. */
typedef struct {
    uint32_t cap_flags;   /* reserved capability flags */
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;    /* bitmask of IRQ lines the driver may use */
} spawn_caps_t;

/* One entry in the PCI device registry; populated from pci_bus scan messages. */
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_base;
    uint8_t mmio_hint;
    uint8_t irq_hint;
} pci_device_record_t;

/* One entry in the block-device registry; present after FAT driver reports. */
typedef struct {
    uint8_t in_use;
    uint8_t unit;           /* physical unit index (e.g. ATA drive number) */
    uint8_t present;
    uint8_t active_service; /* non-zero once a block-fs driver is running */
    uint32_t sector_count;
    char canonical_id[64];  /* stable device identifier string */
    char hash_id[17];       /* 16-char SHA-256 prefix of canonical_id + NUL */
} block_device_record_t;

/* Rule: unconditionally spawn a driver path at boot (always_spawn kind). */
typedef struct {
    uint8_t active;
    uint8_t queued;
    uint8_t spawned;
    char spawn_path[96];
} always_spawn_rule_t;

/* Rule: spawn a block-filesystem driver for a specific block-device unit. */
typedef struct {
    uint8_t active;
    uint8_t queued;
    uint8_t spawned;
    uint8_t unit;       /* ATA/block unit index this rule targets */
    char mount[16];     /* mount point name (e.g. "boot", "user") */
    char spawn_path[96];
} block_fs_rule_t;

/* Rule: spawn a driver when a PCI device matching class/vendor/device is found.
 * spawned_device_mask is a bitmask of registry[] indices already spawned. */
typedef struct {
    uint8_t active;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t spawned_device_mask;  /* registry entry index bits already spawned */
    char spawn_path[96];
} pci_match_rule_t;

/* Rule: spawn a driver when an ACPI/ISA device with matching class is found. */
typedef struct {
    uint8_t active;
    uint8_t class_code;   /* 0xFF = match any */
    uint8_t subclass;     /* 0xFF = match any */
    uint64_t spawned_device_mask;  /* registry entry index bits already spawned */
    char spawn_path[96];
} acpi_match_rule_t;

/* Full service state for the device-manager; single static instance. */
typedef struct {
    hw_phase_t phase;
    hw_spawn_target_t pending;
    int32_t reply_endpoint;
    int32_t proc_endpoint;
    int32_t inventory_endpoint;    /* endpoint receiving PCI/ACPI publish msgs */
    int32_t query_endpoint;        /* endpoint for devmgr.query queries */
    int32_t rule_reply_endpoint;   /* endpoint for rule-file FS reads */
    int32_t fs_endpoint;
    int32_t request_id;
    int32_t module_count;
    uint8_t need_pci_bus;
    uint8_t need_acpi_bus;
    uint8_t need_fat;
    uint8_t need_fs_init;
    uint8_t need_fs_manager;
    uint8_t fat_retries;
    uint8_t fs_init_retries;
    int32_t pci_bus_index;
    int32_t acpi_bus_index;
    int32_t fat_index;
    int32_t fs_init_index;
    int32_t fs_manager_index;
    pci_device_record_t registry[DEVICE_REGISTRY_CAP];
    uint32_t registry_count;
    block_device_record_t block_registry[BLOCK_REGISTRY_CAP];
    uint32_t block_registry_count;
    int32_t selected_storage_index;
    spawn_caps_t selected_storage_caps;
    spawn_caps_t active_rule_spawn_caps;
    uint8_t selected_storage_has_record;
    pci_device_record_t selected_storage_record;
    uint8_t rules_roots_logged;
    uint8_t idle_logged;
    uint8_t rules_init_fail_logged;
    uint8_t rules_init_loaded;
    uint8_t rules_boot_loaded;
    uint8_t rules_boot_request_pending;
    uint16_t rules_boot_retry_delay;
    uint8_t rules_boot_failures;
    int32_t rules_boot_request_id;
    uint16_t rules_init_active;
    uint16_t rules_boot_active;
    uint8_t rule_spawn_pending;
    uint8_t rule_spawn_retries;
    char rule_spawn_path[96];
    uint8_t active_rule_spawn_kind;    /* which rule kind is driving the spawn */
    int32_t active_rule_spawn_index;   /* index into the corresponding rule array */
    int32_t active_rule_spawn_device_index; /* device registry index for pci/acpi */
    always_spawn_rule_t always_spawn_rules[ALWAYS_SPAWN_RULE_CAP];
    uint32_t always_spawn_rule_count;
    block_fs_rule_t block_fs_rules[BLOCK_FS_RULE_CAP];
    uint32_t block_fs_rule_count;
    pci_match_rule_t pci_match_rules[PCI_MATCH_RULE_CAP];
    uint32_t pci_match_rule_count;
    acpi_match_rule_t acpi_match_rules[ACPI_MATCH_RULE_CAP];
    uint32_t acpi_match_rule_count;
    uint8_t boot_mount_ready;   /* non-zero once the boot FS mount is live */
    uint8_t user_mount_ready;   /* non-zero once the user FS mount is live */
    uint8_t ready_notified;     /* non-zero after wasmos_sys_notify_ready sent */
} device_manager_state_t;

#endif
