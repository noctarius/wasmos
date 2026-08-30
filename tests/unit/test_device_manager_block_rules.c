/* test_device_manager_block_rules.c — the device manager's block-rule matcher,
 * driven on the host against the REAL service source.
 *
 * device_manager.c is ordinary C whose block path touches no hostcall except
 * the console, so the suite includes the translation unit and reaches its file
 * statics directly. printf is redirected into a buffer before the include, so
 * console_write lands in a capture the assertions can read: what the service
 * REPORTS is the contract here, not an incidental detail. Everything else the
 * service calls is stubbed below, inertly -- nothing in the block path depends
 * on an IPC or a spawn actually happening.
 *
 * Regression: 2026-08-28-block-rule-match-silent-on-rescan -- a block device
 * that published BEFORE the rule set naming it was loaded was matched only by
 * the re-scan of already-registered devices, and that path reported nothing.
 * The mount succeeded, so the system was correct and silent; every observer --
 * the integration suite, and anyone reading a boot log -- concluded the rule had
 * never matched. Which of the two orderings a boot takes depends on whether
 * /boot mounts before the second disk publishes, so it flipped under load and
 * presented as an unrelated flake in tests/test_virtio_blk.py, twice over: the
 * first test timed out waiting for a marker that would never come, and its
 * forward scan consumed the output the next test needed, failing that one with
 * a message naming a device refusal that never happened.
 */

#include <stdarg.h>
#include <stdint.h>
/* NO <string.h> here. The compile line routes `#include "string.h"` to the
 * project's libc through -iquote, and device_manager.c includes it below. On a
 * host whose system header fortifies (macOS at _USE_FORTIFY_LEVEL 2) that macro-
 * defines strcpy, which then expands inside our libc's plain declaration and
 * fails to parse. Everything this file needs -- strlen, strstr, memset, snprintf
 * -- is declared by the headers device_manager.c pulls in, and every use is
 * after that include. */

/* vsnprintf/printf come from the service's own "stdio.h", pulled in by the
 * included translation unit; the host libc supplies the definitions at link
 * time. Declared here because the capture runs before that include. */
int vsnprintf(char* buffer, unsigned long size, const char* format, va_list args);

/* Capture console output. console_write is a static in the included TU and
 * writes through printf; intercepting here keeps the service source unmodified. */
static char g_out[65536];
static uint32_t g_out_len;

static int test_capture_printf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return n;
    }
    for (int i = 0; line[i] != '\0' && g_out_len + 1u < sizeof(g_out); ++i) {
        g_out[g_out_len++] = line[i];
    }
    g_out[g_out_len] = '\0';
    return n;
}

#define printf test_capture_printf
#include "device_manager.c"
#undef printf

/* ------------------------------------------------------------------ harness */

static int g_failures;
static int g_checks;

/* Reported through puts-with-no-newline so the suite's own output never enters
 * the capture buffer the assertions read. */
static void report(const char* s) {
    (void)putsn(s, strlen(s));
}

static void check(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        report("  [FAIL] ");
        report(what);
        report("\n");
    }
}

/* Defined in stubs_device_manager_host.c: what `boot.partition` reads back as. */
extern char g_test_env_boot_partition[64];

static int out_has(const char* needle) {
    return strstr(g_out, needle) != 0;
}

static void harness_reset(void) {
    memset(&g_dm, 0, sizeof(g_dm));
    g_out_len = 0;
    g_out[0] = '\0';
}

/* A rule as the parser would leave it: active, naming one backend and unit. */
static void add_block_fs_rule(uint8_t backend, uint8_t unit, const char* mount) {
    block_fs_rule_t* rule = &g_dm.block_fs_rules[g_dm.block_fs_rule_count++];
    memset(rule, 0, sizeof(*rule));
    rule->active = 1;
    rule->backend = backend;
    rule->unit = unit;
    (void)snprintf(rule->mount, sizeof(rule->mount), "%s", mount);
    (void)snprintf(rule->spawn_path, sizeof(rule->spawn_path), "system/drivers/fs_fat.wap");
}

/* The publish an ATA disk sends: present, one unit, a plausible capacity.
 *
 * Built as a descriptor and handed to the matcher directly. The transfer-buffer
 * decode in front of it is deliberately skipped: what these cases exercise is
 * WHICH RULES MATCH a device, and routing that through a stubbed host call would
 * test the stub as much as the matcher. */
static void publish_block_device(uint8_t backend, uint8_t unit) {
    wasmos_block_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = BLOCK_DESCRIPTOR_VERSION;
    desc.backend = backend;
    desc.unit = unit;
    desc.sector_bytes = 512u;
    desc.lba_count = 1032192u;
    desc.flags = BLOCK_DESCRIPTOR_FLAG_PRESENT;
    (void)snprintf(desc.canonical_id,
                   sizeof(desc.canonical_id),
                   "block:%s:%u",
                   backend == (uint8_t)BLOCK_BACKEND_ATA ? "ata" : "virtio-blk",
                   (unsigned)unit);
    registry_add_block(&desc);
}

/* A partition of `unit`, as the partition manager publishes one: same backend
 * and unit as its disk -- which is the whole reason the subsystem exists -- plus
 * the table identity a rule can match on. */
static void publish_partition(uint8_t backend, uint8_t unit, uint32_t slot, const char* label,
                              uint32_t scheme, uint32_t fs_type, const uint8_t* type_guid) {
    wasmos_block_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = BLOCK_DESCRIPTOR_VERSION;
    desc.backend = backend;
    desc.unit = unit;
    desc.partition = slot;
    desc.scheme = scheme;
    desc.fs_type = fs_type;
    desc.sector_bytes = 512u;
    desc.lba_start = 2048u;
    desc.lba_count = 1000u;
    desc.flags = BLOCK_DESCRIPTOR_FLAG_PRESENT;
    if (label) {
        (void)snprintf(desc.label, sizeof(desc.label), "%s", label);
    }
    if (type_guid) {
        for (uint32_t i = 0; i < 16u; ++i) {
            desc.type_guid[i] = type_guid[i];
        }
    }
    (void)snprintf(desc.canonical_id,
                   sizeof(desc.canonical_id),
                   "block:%s:%up%u",
                   backend == (uint8_t)BLOCK_BACKEND_ATA ? "ata" : "virtio-blk",
                   (unsigned)unit,
                   (unsigned)slot);
    registry_add_block(&desc);
}

/* A volume on `unit`'s disk, as the volume manager publishes one. The backing
 * device is published first and named by FINGERPRINT, because that is how the
 * matcher resolves it: a helper that set `backing_instance` to anything else
 * would exercise a resolution path the service does not have. */
static void publish_volume(uint8_t backend, uint8_t unit, uint32_t fs_type, const char* label,
                           const uint8_t* uuid, uint32_t uuid_len) {
    wasmos_volume_descriptor_t desc;
    char backing_id[64];

    publish_block_device(backend, unit);
    (void)snprintf(backing_id,
                   sizeof(backing_id),
                   "block:%s:%u",
                   backend == (uint8_t)BLOCK_BACKEND_ATA ? "ata" : "virtio-blk",
                   (unsigned)unit);

    memset(&desc, 0, sizeof(desc));
    desc.version = VOLUME_DESCRIPTOR_VERSION;
    desc.fs_type = fs_type;
    desc.backing_instance = wasmos_block_fingerprint(backing_id);
    desc.sector_bytes = 512u;
    desc.lba_count = 32768u;
    desc.flags = VOLUME_DESCRIPTOR_FLAG_PRESENT;
    if (label) {
        desc.flags |= VOLUME_DESCRIPTOR_FLAG_HAS_LABEL;
        (void)snprintf(desc.label, sizeof(desc.label), "%s", label);
    }
    if (uuid && uuid_len) {
        desc.flags |= VOLUME_DESCRIPTOR_FLAG_HAS_UUID;
        desc.uuid_len = uuid_len;
        for (uint32_t i = 0; i < uuid_len && i < (uint32_t)VOLUME_DESCRIPTOR_UUID_MAX; ++i) {
            desc.uuid[i] = uuid[i];
        }
    }
    (void)snprintf(desc.canonical_id, sizeof(desc.canonical_id), "volume:%s", backing_id);
    registry_add_volume(&desc);
}

/* A volume on a PARTITION of `unit`, at a given place on the whole disk.
 *
 * Separate from publish_volume because the boot match is on the backing device's
 * LBA RANGE, and only a partition has one: a whole-disk record reports 0/0.
 * `lba_start`/`lba_count` are what the firmware's HARDDRIVE node is compared
 * against. */
static void publish_volume_on_partition(uint8_t backend, uint8_t unit, uint32_t slot,
                                        uint64_t lba_start, uint64_t lba_count, uint32_t fs_type) {
    wasmos_block_descriptor_t part;
    wasmos_volume_descriptor_t desc;
    char backing_id[64];

    memset(&part, 0, sizeof(part));
    part.version = BLOCK_DESCRIPTOR_VERSION;
    part.backend = backend;
    part.unit = unit;
    part.partition = slot;
    part.scheme = (uint32_t)PARTITION_SCHEME_MBR;
    part.sector_bytes = 512u;
    part.lba_start = lba_start;
    part.lba_count = lba_count;
    part.flags = BLOCK_DESCRIPTOR_FLAG_PRESENT;
    (void)snprintf(part.canonical_id,
                   sizeof(part.canonical_id),
                   "block:%s:%up%u",
                   backend == (uint8_t)BLOCK_BACKEND_ATA ? "ata" : "virtio-blk",
                   (unsigned)unit,
                   (unsigned)slot);
    registry_add_block(&part);
    (void)snprintf(backing_id, sizeof(backing_id), "%s", part.canonical_id);

    memset(&desc, 0, sizeof(desc));
    desc.version = VOLUME_DESCRIPTOR_VERSION;
    desc.fs_type = fs_type;
    desc.backing_instance = wasmos_block_fingerprint(backing_id);
    desc.sector_bytes = 512u;
    desc.lba_count = lba_count;
    desc.flags = VOLUME_DESCRIPTOR_FLAG_PRESENT;
    (void)snprintf(desc.canonical_id, sizeof(desc.canonical_id), "volume:%s", backing_id);
    registry_add_volume(&desc);
}

/* Load one rule line through the real parser, so these cases exercise the
 * parsing and the matching together -- a rule that parses into the wrong fields
 * and a matcher that reads the wrong fields are indistinguishable from either
 * side alone. */
static int load_rule(const char* line) {
    dm_rules_load_block_fs(&g_dm, line);
    return (int)g_dm.block_fs_rule_count;
}

/* -------------------------------------------------------------------- cases */

/* The ordinary ordering: the rule is loaded before the device publishes, so the
 * live publish path matches it. This is the case that always worked, asserted so
 * a fix for the re-scan cannot be mistaken for the whole contract. */
static void publish_after_rules_reports_the_match(void) {
    harness_reset();
    add_block_fs_rule((uint8_t)BLOCK_BACKEND_ATA, 0u, "boot");

    publish_block_device((uint8_t)BLOCK_BACKEND_ATA, 0u);

    check(out_has("block_fs rule queued spawn driver=ata unit=0"),
          "a device publishing after its rule is reported by the publish path");
    check(g_dm.block_fs_rules[0].queued == 1u, "and the rule is queued");
}

/* Regression: 2026-08-28-block-rule-match-silent-on-rescan.
 *
 * The override rule set lives on /boot, so it cannot load until the boot volume
 * is mounted -- which is itself the result of the first rule. A second disk that
 * publishes inside that window is registered with no rule to match, and only the
 * re-scan run at rule-load time can match it. Both halves are asserted: that the
 * rule is queued (the matcher was never broken) and that the match is REPORTED
 * (it was invisible), because a silent success is what made this look like a
 * device fault for as long as it did. */
static void rescan_after_late_rules_reports_the_match(void) {
    harness_reset();
    add_block_fs_rule((uint8_t)BLOCK_BACKEND_ATA, 0u, "boot");

    publish_block_device((uint8_t)BLOCK_BACKEND_ATA, 0u);
    /* The second disk arrives while only the bootstrap rules exist. */
    publish_block_device((uint8_t)BLOCK_BACKEND_ATA, 1u);

    check(!out_has("driver=ata unit=1"), "a device with no rule yet is not reported as matched");

    /* /boot is mounted; the override set naming unit 1 loads and the devices
     * already registered are re-scanned. */
    add_block_fs_rule((uint8_t)BLOCK_BACKEND_ATA, 1u, "user");
    queue_block_fs_rules_for_known_devices();

    check(g_dm.block_fs_rules[1].queued == 1u || g_dm.block_fs_rules[1].spawned == 1u,
          "the re-scan queues the late rule against the already-published device");
    check(out_has("block_fs rule queued spawn driver=ata unit=1"),
          "and the re-scan reports the match, as the publish path does");
}

/* A rule naming one backend must not be satisfied by a disk on another, whatever
 * path matched it. Both backends number their first disk 0. */
static void a_rule_matches_only_its_own_backend(void) {
    harness_reset();
    add_block_fs_rule((uint8_t)BLOCK_BACKEND_ATA, 0u, "boot");

    publish_block_device((uint8_t)BLOCK_BACKEND_VIRTIO_BLK, 0u);

    check(!out_has("rule queued spawn"), "a virtio disk does not satisfy an ata rule");
    check(g_dm.block_fs_rules[0].queued == 0u, "and the ata rule stays unqueued");

    publish_block_device((uint8_t)BLOCK_BACKEND_ATA, 0u);
    check(out_has("block_fs rule queued spawn driver=ata unit=0"),
          "while its own backend's disk does satisfy it");
}

/* Regression: 2026-08-28-disk-rule-matched-its-own-partition.
 *
 * A partition reports the SAME backend and unit as the disk it lives on, so a
 * disk rule naming (backend, unit) matched both and the winner was whichever
 * record published first. Ordering happened to favour the disk, so this never
 * misbehaved -- it was a coin the system had not yet lost. Both directions are
 * asserted, because a fix that simply stopped disk rules matching partitions
 * would leave partition rules matching disks. */
static void a_disk_rule_does_not_match_a_partition(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"block\", DRIVER==\"ata\", ATTR{unit}==\"0\", "
                    "ENV{MOUNT}=\"/boot\", RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "the disk rule parses");

    publish_partition((uint8_t)BLOCK_BACKEND_ATA,
                      0u,
                      1u,
                      "user",
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      NULL);
    check(g_dm.block_fs_rules[0].queued == 0u,
          "a partition does not satisfy a rule written for the whole disk");

    publish_block_device((uint8_t)BLOCK_BACKEND_ATA, 0u);
    check(g_dm.block_fs_rules[0].queued == 1u, "while the disk itself does");
}

/* The reverse direction, isolated. The rule names NO other matcher on purpose:
 * with a label or a GUID in it the disk would be rejected for want of that
 * attribute even if the subsystem split were removed, and the case would pass
 * while asserting nothing about the split it is named for. */
static void a_partition_rule_does_not_match_a_disk(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"partition\", RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "an unqualified partition rule parses");

    publish_block_device((uint8_t)BLOCK_BACKEND_ATA, 0u);
    check(g_dm.block_fs_rules[0].queued == 0u,
          "a whole disk does not satisfy a rule written for a partition");

    publish_partition((uint8_t)BLOCK_BACKEND_ATA,
                      0u,
                      1u,
                      "user",
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      NULL);
    check(g_dm.block_fs_rules[0].queued == 1u, "while any partition does");
}

/* The mount that names itself: a GPT partition labelled with a path is matched
 * without the rule naming a disk, a unit or a backend at all. */
static void a_partition_is_matched_by_its_label(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"partition\", ATTR{partlabel}==\"user\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "the label rule parses");

    publish_partition((uint8_t)BLOCK_BACKEND_ATA,
                      1u,
                      1u,
                      "scratch",
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      NULL);
    check(g_dm.block_fs_rules[0].queued == 0u, "a differently-labelled partition is skipped");

    publish_partition((uint8_t)BLOCK_BACKEND_VIRTIO_BLK,
                      40u,
                      2u,
                      "user",
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      NULL);
    check(g_dm.block_fs_rules[0].queued == 1u,
          "and the labelled one matches, on a different backend and unit entirely");
}

/* The ESP type GUID, in the canonical text a rule carries and the mixed-endian
 * bytes GPT actually stores. If the parser's byte order were wrong the rule
 * would parse cleanly and never match, which is why the two forms are written
 * out here rather than derived from each other. */
static const char ESP_GUID_TEXT[] = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
static const uint8_t ESP_GUID_BYTES[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

static void a_partition_is_matched_by_its_type_guid(void) {
    char line[192];
    harness_reset();
    (void)snprintf(line,
                   sizeof(line),
                   "SUBSYSTEM==\"partition\", ATTR{type}==\"%s\", ENV{MOUNT}=\"/boot\", "
                   "RUN+=\"system/drivers/fs_fat.wap\"\n",
                   ESP_GUID_TEXT);
    check(load_rule(line) == 1, "the type-GUID rule parses");

    publish_partition((uint8_t)BLOCK_BACKEND_ATA,
                      0u,
                      1u,
                      NULL,
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      NULL);
    check(g_dm.block_fs_rules[0].queued == 0u, "a partition with no type GUID is skipped");

    publish_partition((uint8_t)BLOCK_BACKEND_ATA,
                      0u,
                      2u,
                      NULL,
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      ESP_GUID_BYTES);
    check(g_dm.block_fs_rules[0].queued == 1u,
          "and the ESP type GUID matches its mixed-endian on-disk bytes");
}

/* Regression: 2026-08-30-volume-uuid-read-in-gpt-byte-order.
 *
 * ATTR{uuid} was parsed by parse_guid, which reads the GPT mixed-endian on-disk
 * form with its first three groups reversed. A volume uuid is not a GPT GUID: it
 * is whatever bytes the FORMAT stores, and mkfs_wfs writes and prints them in
 * order. A rule spelling the uuid mkfs printed therefore compared bytes
 * 3,2,1,0,5,4,7,6 against 0,1,2,3,4,5,6,7 and could never fire -- silently, since
 * a rule that matches nothing looks exactly like a device that is absent.
 *
 * The bytes below are deliberately ascending, so a reversal of any group shows
 * up as a mismatch rather than surviving a palindrome. */
static void a_volume_is_matched_by_the_uuid_its_formatter_printed(void) {
    static const uint8_t WFS_UUID[16] = {0x01,
                                         0x02,
                                         0x03,
                                         0x04,
                                         0x05,
                                         0x06,
                                         0x07,
                                         0x08,
                                         0x09,
                                         0x0a,
                                         0x0b,
                                         0x0c,
                                         0x0d,
                                         0x0e,
                                         0x0f,
                                         0x10};
    harness_reset();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{fstype}==\"wfs\", "
                    "ATTR{uuid}==\"01020304-0506-0708-090a-0b0c0d0e0f10\", "
                    "ENV{MOUNT}=\"/wfs\", RUN+=\"system/drivers/fs_wfs.wap\"\n") == 1,
          "the volume-uuid rule parses");

    publish_volume((uint8_t)BLOCK_BACKEND_ATA, 2u, (uint32_t)FS_TYPE_WFS, NULL, WFS_UUID, 16u);
    check(g_dm.block_fs_rules[0].queued == 1u,
          "a volume matches the uuid spelled in the order its formatter prints");
}

/* The hyphens are presentation. Accepting the bare form as well is what lets a
 * uuid be pasted from either mkfs_wfs's `--uuid` argument or its report without
 * the two disagreeing about which one a rule takes. */
static void a_volume_uuid_may_be_spelled_without_hyphens(void) {
    static const uint8_t WFS_UUID[16] = {0x01,
                                         0x02,
                                         0x03,
                                         0x04,
                                         0x05,
                                         0x06,
                                         0x07,
                                         0x08,
                                         0x09,
                                         0x0a,
                                         0x0b,
                                         0x0c,
                                         0x0d,
                                         0x0e,
                                         0x0f,
                                         0x10};
    harness_reset();
    check(load_rule("SUBSYSTEM==\"volume\", "
                    "ATTR{uuid}==\"0102030405060708090a0b0c0d0e0f10\", "
                    "ENV{MOUNT}=\"/wfs\", RUN+=\"system/drivers/fs_wfs.wap\"\n") == 1,
          "the unhyphenated form parses");

    publish_volume((uint8_t)BLOCK_BACKEND_ATA, 2u, (uint32_t)FS_TYPE_WFS, NULL, WFS_UUID, 16u);
    check(g_dm.block_fs_rules[0].queued == 1u, "and names the same volume");
}

/* A FAT volume serial is four bytes, not sixteen. Requiring a full GUID would
 * leave every FAT volume unaddressable by identity, which is most of them. */
static void a_short_format_identity_is_spelled_at_its_own_width(void) {
    static const uint8_t FAT_SERIAL[4] = {0x1a, 0x2b, 0x3c, 0x4d};
    harness_reset();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{fstype}==\"fat\", "
                    "ATTR{uuid}==\"1a2b3c4d\", "
                    "ENV{MOUNT}=\"/user\", RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "a four-byte volume serial parses");

    publish_volume((uint8_t)BLOCK_BACKEND_ATA, 1u, (uint32_t)FS_TYPE_FAT, "USER", FAT_SERIAL, 4u);
    check(g_dm.block_fs_rules[0].queued == 1u, "and matches the volume carrying it");
}

/* What the uuid matcher is FOR: two volumes of the same format, told apart by
 * identity alone. Without this the only discriminator left is the disk each sits
 * on, which is the naming mount policy exists to stop. */
static void two_volumes_of_one_format_are_told_apart_by_uuid(void) {
    static const uint8_t UUID_A[16] = {0xaa, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t UUID_B[16] = {0xbb, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    harness_reset();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{fstype}==\"wfs\", "
                    "ATTR{uuid}==\"bb000002-0000-0000-0000-000000000000\", "
                    "ENV{MOUNT}=\"/vwfs\", RUN+=\"system/drivers/fs_wfs.wap\"\n") == 1,
          "the rule names one of two identically-formatted volumes");

    publish_volume((uint8_t)BLOCK_BACKEND_ATA, 2u, (uint32_t)FS_TYPE_WFS, NULL, UUID_A, 16u);
    check(g_dm.block_fs_rules[0].queued == 0u, "the volume it does not name is skipped");

    publish_volume(
        (uint8_t)BLOCK_BACKEND_VIRTIO_BLK, 48u, (uint32_t)FS_TYPE_WFS, NULL, UUID_B, 16u);
    check(g_dm.block_fs_rules[0].queued == 1u, "and the one it names is matched");
}

/* ATTR{boot}: the volume this system was loaded from.
 *
 * The one identity nothing on the volume can supply -- an ESP is an ordinary FAT
 * volume, its label is firmware-specific, and an MBR gives it no partition label
 * and no PARTUUID. The firmware knows, the bootloader records it, the kernel
 * publishes `boot.partition`, and the match is on the backing partition's LBA
 * range because that is the one fact both sides state the same way.
 *
 * The range below is the ESP the shipped boot image actually reports (LBA 63,
 * 1032129 sectors), so a change that broke the hexadecimal parse would be caught
 * with the value it will really see. */
static void the_boot_volume_is_the_one_the_firmware_named(void) {
    harness_reset();
    (void)snprintf(g_test_env_boot_partition, sizeof(g_test_env_boot_partition), "%s", "3f:fbfc1");
    load_boot_partition();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{boot}==\"1\", "
                    "ENV{MOUNT}=\"/boot\", RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "the boot-volume rule parses");

    publish_volume_on_partition(
        (uint8_t)BLOCK_BACKEND_ATA, 0u, 2u, 2048u, 128991u, (uint32_t)FS_TYPE_FAT);
    check(g_dm.block_fs_rules[0].queued == 0u,
          "a volume elsewhere on the same disk is not the boot volume");

    publish_volume_on_partition(
        (uint8_t)BLOCK_BACKEND_ATA, 0u, 1u, 63u, 1032129u, (uint32_t)FS_TYPE_FAT);
    check(g_dm.block_fs_rules[0].queued == 1u,
          "and the volume covering the firmware's LBA range is");
}

/* A boot the firmware could not describe -- a network boot, or a whole disk with
 * no table -- publishes no variable. Nothing is then marked, so an ATTR{boot}
 * rule selects nothing rather than selecting whatever happens to be first. */
static void without_a_boot_partition_nothing_is_the_boot_volume(void) {
    harness_reset();
    g_test_env_boot_partition[0] = '\0';
    load_boot_partition();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{boot}==\"1\", "
                    "ENV{MOUNT}=\"/boot\", RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "the boot-volume rule parses");

    publish_volume_on_partition(
        (uint8_t)BLOCK_BACKEND_ATA, 0u, 1u, 63u, 1032129u, (uint32_t)FS_TYPE_FAT);
    check(g_dm.block_fs_rules[0].queued == 0u,
          "no volume is the boot volume when the firmware named none");
}

/* ATTR{boot} on a block or partition rule can never fire: the flag lives on a
 * volume. Refused at load, on the same terms as every other cross-subsystem
 * matcher. */
static void a_boot_matcher_outside_a_volume_rule_is_refused(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"block\", ATTR{boot}==\"1\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 0,
          "a disk rule carrying ATTR{boot} is refused");
    harness_reset();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{boot}==\"yes\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 0,
          "and so is a boot matcher that is neither 0 nor 1");
}

/* An EMPTY label is a label. A FAT volume may genuinely be named "", which is
 * not the same as a format that carries no label at all -- the descriptor has
 * always separated the two, and a rule has to be able to say it as well.
 *
 * Presence inferred from `label[0]` could not: ATTR{label}=="" was
 * indistinguishable from no matcher, so it matched every volume instead of the
 * blank-labelled ones, and slipped past the check that refuses a volume matcher
 * on a block rule. */
static void an_empty_label_matcher_selects_the_blank_label(void) {
    static const uint8_t SERIAL[4] = {0x11, 0x22, 0x33, 0x44};
    harness_reset();
    check(load_rule("SUBSYSTEM==\"volume\", ATTR{label}==\"\", "
                    "ENV{MOUNT}=\"/blank\", RUN+=\"system/drivers/fs_fat.wap\"\n") == 1,
          "an empty-label rule parses");

    publish_volume((uint8_t)BLOCK_BACKEND_ATA, 1u, (uint32_t)FS_TYPE_FAT, "USER", SERIAL, 4u);
    check(g_dm.block_fs_rules[0].queued == 0u, "a labelled volume does not satisfy it");

    publish_volume((uint8_t)BLOCK_BACKEND_ATA, 2u, (uint32_t)FS_TYPE_FAT, "", SERIAL, 4u);
    check(g_dm.block_fs_rules[0].queued == 1u, "and a blank-labelled one does");
}

/* The same flag on the refusal path: a volume matcher on a block rule can never
 * fire, and an empty value must not smuggle one past. */
static void an_empty_label_matcher_outside_a_volume_rule_is_refused(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"block\", ATTR{label}==\"\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 0,
          "a disk rule carrying an empty ATTR{label} is refused");
}

/* A malformed matcher rejects the RULE. Dropping the attribute instead would
 * widen the rule to everything its author did not ask for, and on the mount path
 * that means a filesystem on the wrong volume. */
static void a_malformed_matcher_rejects_the_rule(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"partition\", ATTR{type}==\"not-a-guid\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 0,
          "a rule whose GUID does not parse is refused entirely");

    harness_reset();
    check(load_rule("SUBSYSTEM==\"partition\", ATTR{fstype}==\"reiserfs\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 0,
          "and so is one naming a filesystem nothing probes for");
}

/* A partition matcher on a disk rule can never fire, so it is refused rather
 * than left as a rule that looks written and never runs. */
static void a_partition_matcher_on_a_disk_rule_is_refused(void) {
    harness_reset();
    check(load_rule("SUBSYSTEM==\"block\", ATTR{partlabel}==\"user\", "
                    "RUN+=\"system/drivers/fs_fat.wap\"\n") == 0,
          "a disk rule carrying a partition matcher is refused");
}

/* Regression: 2026-08-28-rule-line-truncated-into-a-wider-rule.
 *
 * copy_rule_line() filled its buffer and returned success, so a rule longer than
 * the buffer was parsed as its own prefix. That is not a smaller rule: a rule is
 * a conjunction, so dropping its tail REMOVES matchers, and a line whose RUN+=
 * survives while its ATTR{partlabel} is cut becomes a rule that spawns a
 * filesystem on every partition on the system.
 *
 * The line below is that shape deliberately -- action first, matcher last -- and
 * long enough to overflow the block-rule buffer. Both halves are asserted: that
 * the rule is refused, and that no partition satisfies whatever survived, since
 * a fix that merely stopped writing past the end would still leave the prefix
 * behind. */
static void an_overlong_rule_line_is_refused(void) {
    char line[1024];
    uint32_t n = 0;
    harness_reset();

    n = (uint32_t)snprintf(line,
                           sizeof(line),
                           "SUBSYSTEM==\"partition\", RUN+=\"system/drivers/fs_fat.wap\", "
                           "ENV{MOUNT}=\"/user\"");
    /* Pad with tokens the parser ignores, so the length -- not the content -- is
     * what pushes the matcher past the end. */
    while (n < 600u) {
        n += (uint32_t)snprintf(line + n, sizeof(line) - n, ", COMMENT==\"padding\"");
    }
    (void)snprintf(line + n, sizeof(line) - n, ", ATTR{partlabel}==\"user\"\n");

    check(load_rule(line) == 0, "a rule line too long for the parser is refused");

    publish_partition((uint8_t)BLOCK_BACKEND_ATA,
                      0u,
                      1u,
                      "scratch",
                      (uint32_t)PARTITION_SCHEME_GPT,
                      (uint32_t)FS_TYPE_FAT,
                      NULL);
    check(!out_has("rule queued spawn"),
          "and no partition is spawned on by whatever survived the cut");
}

int main(void) {
    publish_after_rules_reports_the_match();
    rescan_after_late_rules_reports_the_match();
    a_rule_matches_only_its_own_backend();
    a_disk_rule_does_not_match_a_partition();
    a_partition_rule_does_not_match_a_disk();
    a_partition_is_matched_by_its_label();
    a_partition_is_matched_by_its_type_guid();
    a_volume_is_matched_by_the_uuid_its_formatter_printed();
    a_volume_uuid_may_be_spelled_without_hyphens();
    a_short_format_identity_is_spelled_at_its_own_width();
    two_volumes_of_one_format_are_told_apart_by_uuid();
    the_boot_volume_is_the_one_the_firmware_named();
    without_a_boot_partition_nothing_is_the_boot_volume();
    a_boot_matcher_outside_a_volume_rule_is_refused();
    an_empty_label_matcher_selects_the_blank_label();
    an_empty_label_matcher_outside_a_volume_rule_is_refused();
    a_malformed_matcher_rejects_the_rule();
    a_partition_matcher_on_a_disk_rule_is_refused();
    an_overlong_rule_line_is_refused();

    char summary[128];
    (void)snprintf(summary,
                   sizeof(summary),
                   "test_device_manager_block_rules: %d checks, %d failures\n",
                   g_checks,
                   g_failures);
    report(summary);
    return g_failures == 0 ? 0 : 1;
}
