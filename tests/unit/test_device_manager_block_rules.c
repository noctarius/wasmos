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
#include <string.h>

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
        report("  [FAIL] "); report(what); report("\n");
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

/* The publish an ATA disk sends: present, one unit, a plausible capacity. */
static void publish_block_device(uint8_t backend, uint8_t unit) {
    registry_add_block_from_ipc((int32_t)unit, (int32_t)1032192, (int32_t)1, (int32_t)backend);
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

    check(!out_has("driver=ata unit=1"),
          "a device with no rule yet is not reported as matched");

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

int main(void) {
    publish_after_rules_reports_the_match();
    rescan_after_late_rules_reports_the_match();
    a_rule_matches_only_its_own_backend();

    char summary[128];
    (void)snprintf(summary,
                   sizeof(summary),
                   "test_device_manager_block_rules: %d checks, %d failures\n",
                   g_checks,
                   g_failures);
    report(summary);
    return g_failures == 0 ? 0 : 1;
}
