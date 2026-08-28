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

int main(void) {
    publish_after_rules_reports_the_match();
    rescan_after_late_rules_reports_the_match();
    a_rule_matches_only_its_own_backend();
    a_disk_rule_does_not_match_a_partition();
    a_partition_rule_does_not_match_a_disk();
    a_partition_is_matched_by_its_label();
    a_partition_is_matched_by_its_type_guid();
    a_malformed_matcher_rejects_the_rule();
    a_partition_matcher_on_a_disk_rule_is_refused();

    char summary[128];
    (void)snprintf(summary,
                   sizeof(summary),
                   "test_device_manager_block_rules: %d checks, %d failures\n",
                   g_checks,
                   g_failures);
    report(summary);
    return g_failures == 0 ? 0 : 1;
}
