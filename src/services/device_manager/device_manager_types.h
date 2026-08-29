/* device_manager_types.h - structs, enums, and constants shared by the
 * device-manager service and its rule parser */
#ifndef WASMOS_DEVICE_MANAGER_TYPES_H
#define WASMOS_DEVICE_MANAGER_TYPES_H

#include <stdint.h>
#include "wasmos_driver_abi.h" /* wasmos_pci_bar_t, wasmos_block_descriptor_t */

/* PCI/ACPI functions the registry can hold.  64 is not arbitrary: the
 * per-rule spawned_device_mask below is a uint64_t whose bit i marks registry
 * index i as already spawned, so raising this past 64 needs a wider mask.
 * Publishes past the cap are dropped. */
#define DEVICE_REGISTRY_CAP 64
/* Block devices (ATA/AHCI units) the registry can hold. */
#define BLOCK_REGISTRY_CAP 16
/* Volumes the inventory holds. At most one per block device, and usually fewer:
 * a partitioned disk publishes none. */
#define VOLUME_REGISTRY_CAP 16
/* Value a rule field carries when the corresponding ATTR{} was absent from the
 * rule line, meaning "match any device".  A device that genuinely reports 0xFF /
 * 0xFFFF in that field is therefore indistinguishable from the wildcard and
 * cannot be matched on alone — match it on another attribute instead. */
#define MATCH_ANY_U8 0xFFu    /* wildcard for class/subclass byte fields */
#define MATCH_ANY_U16 0xFFFFu /* wildcard for vendor/device ID fields */
/* The two rule roots, read in this order: the initfs copy is available before
 * any storage driver exists and carries the bootstrap rules; the boot-FAT copy
 * is read once storage is online and replaces the matching tables (each
 * dm_rules_load_* call clears its table first, so loading is a replace, not a
 * merge). */
#define DEVMGR_RULES_INIT_ROOT "/init/devmgr/rules"
#define DEVMGR_RULES_BOOT_ROOT "/boot/system/devmgr/rules"
#define DEVMGR_RULE_FILE "default.rules"
/* Fixed read/parse buffer for the rules file. Sized for the partition matchers:
 * a rule naming a type GUID and a label runs to roughly twice the length of one
 * naming a driver and a unit.
 * TODO: a rules file larger than this is silently truncated, dropping its
 * trailing rules with no diagnostic — raising the cap postpones that rather than
 * fixing it. Size the read from FS_IPC_STAT_REQ, or stream it, so the file has
 * no size limit (see docs/TASKS.md). */
#define DEVMGR_RULE_TEXT_CAP 8192
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
    HW_PHASE_WAIT_INVENTORY,      /* waiting for PCI scan-done notification */
    HW_PHASE_WAIT_ACPI_INVENTORY, /* waiting for ACPI scan-done notification */
    HW_PHASE_IDLE,
    HW_PHASE_FAILED
} hw_phase_t;

/* Which subsystem is currently being spawned (one at a time). */
typedef enum {
    HW_SPAWN_NONE = 0,
    HW_SPAWN_RULE_PATH, /* spawning a rule-driven driver */
    HW_SPAWN_PCI_BUS,
    HW_SPAWN_ACPI_BUS,
    HW_SPAWN_FAT,
    HW_SPAWN_FS_INIT,
    HW_SPAWN_FS_MANAGER,
} hw_spawn_target_t;

/* I/O capabilities granted to a spawned driver process. */
typedef struct {
    uint32_t cap_flags; /* reserved capability flags */
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask; /* bitmask of IRQ lines the driver may use */
    /* Windows resolved from the driver's declared regions, in declaration order
     * (which is the region index it addresses at runtime). Non-zero count
     * supersedes io_port_min/max and routes the spawn through the descriptor
     * form, the only one that can describe more than a single window. */
    uint32_t io_range_count;
    wasmos_io_range_t io_ranges[WASMOS_IO_RANGE_LIMIT];
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
    uint8_t irq_pin;
    /* Every region the function decodes, as pci-bus resolved them. Kept whole
     * rather than flattened to a single "io_port_base", because which BAR holds
     * what is device-specific: legacy IDE leaves BAR0 empty and puts its
     * bus-master registers in BAR4. */
    wasmos_pci_bar_t bars[WASMOS_PCI_BAR_COUNT];
    uint16_t msi_cap_offset;
    uint16_t msix_cap_offset;
} pci_device_record_t;

/* One entry in the block-device registry.
 *
 * The device is identified by `desc.canonical_id`, which its PUBLISHER assigns:
 * a backend knows where its disks are and this service does not. Synthesizing an
 * identity here was how the ATA controller's PCI address came to be attached to
 * virtio disks that were nowhere near it.
 *
 * Every attribute -- backend, unit, capacity, partition scheme, filesystem, the
 * GPT identity fields -- lives in the descriptor rather than being unpacked into
 * fields here, so a new attribute reaches a rule without a change to this
 * struct.
 *
 * `active_service` stays outside it because it is this service's own bookkeeping,
 * not something the backend reported: the descriptor's own ACTIVE_SERVICE flag
 * is the publisher's opinion at publish time, and a mount that happens later
 * cannot go back and change it. */
typedef struct {
    uint8_t in_use;
    uint8_t active_service; /* non-zero once a block-fs driver is running */
    wasmos_block_descriptor_t desc;
    char hash_id[17]; /* 16-char SHA-256 prefix of desc.canonical_id + NUL */
} block_device_record_t;

/* One VOLUME, as the volume manager describes it.
 *
 * Separate from block_device_record_t rather than a flag on it, because the two
 * answer different questions and differ in both directions: a partition-table
 * entry may hold no filesystem, and a disk with no table may hold one. A rule
 * matching SUBSYSTEM=="volume" is asking what can be MOUNTED; one matching
 * SUBSYSTEM=="block" is asking what storage exists.
 *
 * The descriptor names its backing device by class instance. Resolving that to a
 * canonical id is done against block_registry, where the backend that assigned
 * the id already put it -- see devmgr_backing_id_for_volume. */
typedef struct {
    uint8_t in_use;
    uint8_t active_service; /* non-zero once a filesystem is mounted on it */
    wasmos_volume_descriptor_t desc;
} volume_record_t;

/* Rule: unconditionally spawn a driver path at boot (always_spawn kind). */
typedef struct {
    uint8_t active;
    uint8_t queued;
    uint8_t spawned;
    char spawn_path[96];
} always_spawn_rule_t;

/* Which kind of block device a rule is willing to match.
 *
 * A partition is a block device with the same backend and unit as the disk it
 * lives on -- `block:ata:0p1` reports backend ata, unit 0, exactly as
 * `block:ata:0` does -- so a rule naming (backend, unit) alone matches BOTH, and
 * only the order the two happen to publish in decides which one a filesystem is
 * mounted on. The subsystem is what separates them, and it is not optional:
 * `SUBSYSTEM=="block"` means a whole disk and `SUBSYSTEM=="partition"` means a
 * partition of one. */
#define DEVMGR_BLOCK_SUBSYS_DISK 0u
#define DEVMGR_BLOCK_SUBSYS_PARTITION 1u
/* A volume: something with a filesystem on it, whether that is a partition or a
 * whole unpartitioned device. The distinction the two above draw -- disk versus
 * partition -- is invisible here on purpose, which is the whole point of the
 * layer: a rule says what a volume IS, not where it sits. */
#define DEVMGR_BLOCK_SUBSYS_VOLUME 2u

/* Rule: spawn a block-filesystem driver for a specific block device.
 *
 * A DISK rule names the device by (backend, unit), because a unit alone is
 * ambiguous across backends -- an unqualified `ATTR{unit}=="0"` would match both
 * the ATA boot disk and a virtio-blk device's only disk, and spawn a filesystem
 * twice on the same mount. BLOCK_BACKEND_UNKNOWN in `backend` means the rule
 * named no DRIVER and matches any, which is kept only so an existing unqualified
 * rule still parses.
 *
 * A PARTITION rule matches on what a partition table says about the volume
 * instead: its PARTUUID, its label, its type GUID, the filesystem probed in it,
 * or the scheme of the table it came from. Those come from the disk rather than
 * from this file, which is the point -- a GPT partition labelled with a path
 * names its own mount and needs no rule naming a device at all.
 *
 * Every matcher is optional and an omitted one matches anything; a rule with no
 * matcher at all matches every device of its subsystem. Matching is exact, with
 * no globbing: a label or a UUID is an exact thing, and a pattern engine here is
 * easy to add later and hard to remove. */
typedef struct {
    uint8_t active;
    uint8_t queued;
    uint8_t spawned;
    uint8_t subsystem; /* DEVMGR_BLOCK_SUBSYS_* */
    uint8_t backend;   /* BLOCK_BACKEND_*, or UNKNOWN to match any */
    uint8_t unit;      /* unit index within that backend; 0xFF matches any */
    /* Partition matchers. `has_*` distinguishes "the rule did not say" from a
     * value that happens to be zero -- FS_TYPE_UNKNOWN and PARTITION_SCHEME_NONE
     * are both legitimate things to match on. */
    uint8_t has_type_guid;
    uint8_t has_part_guid;
    uint8_t has_fs_type;
    uint8_t has_scheme;
    uint8_t type_guid[16]; /* raw on-disk bytes, as the descriptor carries them */
    uint8_t part_guid[16];
    uint32_t fs_type; /* FS_TYPE_* */
    uint32_t scheme;  /* PARTITION_SCHEME_* */
    char partlabel[BLOCK_DESCRIPTOR_LABEL_MAX];
    char device_name[BLOCK_DESCRIPTOR_ID_MAX]; /* ATTR{name}: the canonical id */
    /* Volume matchers. ATTR{label} is the FILESYSTEM's own label, which is not
     * ATTR{partlabel}: the ESP's partition table carries no label at all while
     * its FAT boot sector says "QEMU VVFAT", and make_gpt_image writes the GPT
     * name `user` beside a FAT volume label `USER`. Matching the wrong one finds
     * nothing, so they are separate matchers rather than one spelling. */
    uint8_t has_uuid;
    uint8_t uuid[VOLUME_DESCRIPTOR_UUID_MAX];
    char label[VOLUME_DESCRIPTOR_LABEL_MAX];
    /* The device that actually matched, filled in when the rule is queued. The
     * filesystem driver is told about THIS, not about the rule's pattern: a
     * wildcard rule has no unit of its own to pass on, and passing the pattern
     * handed the driver 0xFF as though it were a unit number. */
    uint8_t matched_backend;
    uint8_t matched_unit;
    /* The matched device's canonical id, copied verbatim so the filesystem
     * driver can fingerprint the SAME string its backend registered under. The
     * driver is given the id rather than (driver, unit) to rebuild it from,
     * because a second place that spells the id is a second place that can
     * disagree with the publisher -- and a disagreement means the filesystem
     * looks up a class instance nothing holds. */
    char matched_id[BLOCK_DESCRIPTOR_ID_MAX];
    /* Where the volume mounts, as a full VFS path. Delivered to the filesystem
     * driver as `mount=` in its startup arguments; nothing overrides it
     * afterwards, and nothing derives it from the device.
     *
     * A partition's LABEL is a MATCHER, not a mount path: `/user` is mounted by
     * a rule matching ATTR{partlabel}=="user", which is what lets a GPT volume
     * be found without naming a disk -- but the rule still says where it goes.
     * Keeping the two apart is what allows one labelled volume to be mounted
     * somewhere else by editing a rule rather than rewriting a partition
     * table. */
    char mount[16];
    char spawn_path[96];
} block_fs_rule_t;

/* Rule: spawn a driver when a PCI device matching class/vendor/device is found.
 * spawned_device_mask is a bitmask of registry[] indices already spawned.
 *
 * `meta` caches what the module this rule names declares about itself, fetched
 * once when the PCI inventory is consumed. It is cached rather than queried on
 * demand because the queue walk runs inside event-handler dispatch, where a
 * blocking request would be a nested receive; with the descriptor in hand,
 * resolving a device's windows is pure. `meta_valid` is 0 when the module could
 * not be resolved, in which case the rule still spawns, just without declared
 * windows. */
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
    uint64_t spawned_device_mask; /* registry entry index bits already spawned */
    char spawn_path[96];
    uint8_t meta_valid;
    wasmos_module_meta_desc_t meta;
} pci_match_rule_t;

/* Rule: spawn a driver when an ACPI/ISA device with matching class is found. */
typedef struct {
    uint8_t active;
    uint8_t class_code;           /* 0xFF = match any */
    uint8_t subclass;             /* 0xFF = match any */
    uint64_t spawned_device_mask; /* registry entry index bits already spawned */
    char spawn_path[96];
} acpi_match_rule_t;

/* Full service state for the device-manager; single static instance. */
typedef struct {
    hw_phase_t phase;
    hw_spawn_target_t pending;
    int32_t reply_endpoint;
    int32_t proc_endpoint;
    int32_t inventory_endpoint;  /* endpoint receiving PCI/ACPI publish msgs */
    int32_t query_endpoint;      /* endpoint for devmgr.query queries */
    int32_t rule_reply_endpoint; /* endpoint for rule-file FS reads */
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
    volume_record_t volume_registry[VOLUME_REGISTRY_CAP];
    uint32_t volume_registry_count;
    spawn_caps_t active_rule_spawn_caps;
    /* The PCI function the storage-bootstrap driver was matched to. Kept because
     * the block registry names its units by that address and the mount-info
     * replies report it; the driver's own grants come from the rule spawn like
     * every other driver's. */
    uint8_t selected_storage_has_record;
    pci_device_record_t selected_storage_record;
    /* A rule set was loaded and the per-rule module descriptors need refetching.
     * Refreshing takes blocking requests to process-manager, which must not run
     * while a rule spawn is in flight -- draining the reply endpoint mid-spawn
     * corrupts active_rule_spawn_* state -- so the refresh is deferred to a
     * point where nothing is pending. */
    uint8_t pci_rule_meta_dirty;
    uint8_t rules_roots_logged;
    uint8_t idle_logged;
    uint8_t rules_init_fail_logged;
    uint8_t rules_init_loaded;
    uint8_t rules_boot_loaded;
    uint8_t rules_boot_request_pending;
    uint16_t rules_boot_retry_delay;
    uint8_t rules_boot_failures;
    int32_t rules_boot_request_id;
    int32_t rules_boot_bid; /* owner-push xfer buffer_id in flight for boot rules read */
    uint16_t rules_init_active;
    uint16_t rules_boot_active;
    uint8_t rule_spawn_pending;
    uint8_t rule_spawn_retries;
    char rule_spawn_path[96];
    uint8_t active_rule_spawn_kind;         /* which rule kind is driving the spawn */
    int32_t active_rule_spawn_index;        /* index into the corresponding rule array */
    int32_t active_rule_spawn_device_index; /* device registry index for pci/acpi */
    always_spawn_rule_t always_spawn_rules[ALWAYS_SPAWN_RULE_CAP];
    uint32_t always_spawn_rule_count;
    block_fs_rule_t block_fs_rules[BLOCK_FS_RULE_CAP];
    uint32_t block_fs_rule_count;
    pci_match_rule_t pci_match_rules[PCI_MATCH_RULE_CAP];
    uint32_t pci_match_rule_count;
    acpi_match_rule_t acpi_match_rules[ACPI_MATCH_RULE_CAP];
    uint32_t acpi_match_rule_count;
    uint8_t boot_mount_ready; /* non-zero once the boot FS mount is live */
    uint8_t user_mount_ready; /* non-zero once the user FS mount is live */
    uint8_t ready_notified;   /* non-zero after wasmos_sys_notify_ready sent */
} device_manager_state_t;

#endif
