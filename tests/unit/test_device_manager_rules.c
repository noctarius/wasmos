#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_manager_rules.h"

static void
test_always_spawn_rule(void)
{
    device_manager_state_t state;
    const char *rules =
        "SUBSYSTEM==\"boot\", ACTION==\"add\", RUN+=\"system/drivers/ata.wap\"\n"
        "SUBSYSTEM==\"boot\", RUN+=\"system/drivers/keyboard.wap\"\n";
    memset(&state, 0, sizeof(state));
    dm_rules_load_always_spawn(&state, rules);
    assert(state.always_spawn_rule_count == 2u);
    assert(strcmp(state.always_spawn_rules[0].spawn_path, "system/drivers/ata.wap") == 0);
    assert(strcmp(state.always_spawn_rules[1].spawn_path, "system/drivers/keyboard.wap") == 0);
}

static void
test_block_fs_rule(void)
{
    device_manager_state_t state;
    const char *rules =
        "SUBSYSTEM==\"block\", ATTR{unit}==\"0\", ENV{MOUNT}=\"/boot\", RUN+=\"system/drivers/fs_fat.wap\"\n"
        "SUBSYSTEM==\"block\", ATTR{unit}==\"1\", ENV{MOUNT}=\"/user\", RUN+=\"system/drivers/fs_fat.wap\"\n";
    memset(&state, 0, sizeof(state));
    dm_rules_load_block_fs(&state, rules);
    assert(state.block_fs_rule_count == 2u);
    assert(state.block_fs_rules[0].unit == 0u);
    assert(strcmp(state.block_fs_rules[0].mount, "/boot") == 0);
    assert(strcmp(state.block_fs_rules[0].spawn_path, "system/drivers/fs_fat.wap") == 0);
    assert(state.block_fs_rules[1].unit == 1u);
    assert(strcmp(state.block_fs_rules[1].mount, "/user") == 0);
}

static void
test_pci_match_rule(void)
{
    device_manager_state_t state;
    const char *rules =
        "SUBSYSTEM==\"pci\", ATTR{bus}==\"0x00\", ATTR{slot}==\"0x02\", ATTR{function}==\"0x00\", ATTR{class}==\"0x03\", ATTR{subclass}==\"0x00\", ATTR{prog_if}==\"0x00\", RUN+=\"system/drivers/fbpci.wap\"\n";
    memset(&state, 0, sizeof(state));
    dm_rules_load_pci_match(&state, rules);
    assert(state.pci_match_rule_count == 1u);
    assert(state.pci_match_rules[0].bus == 0x00u);
    assert(state.pci_match_rules[0].slot == 0x02u);
    assert(state.pci_match_rules[0].function == 0x00u);
    assert(state.pci_match_rules[0].class_code == 0x03u);
    assert(state.pci_match_rules[0].subclass == 0x00u);
    assert(state.pci_match_rules[0].prog_if == 0x00u);
    assert(strcmp(state.pci_match_rules[0].spawn_path, "system/drivers/fbpci.wap") == 0);
}

static void
test_legacy_rule_is_rejected(void)
{
    device_manager_state_t state;
    const char *rules =
        "always_spawn spawn_path=system/drivers/ata.wap\n"
        "pci_match class=03 spawn_path=system/drivers/fbpci.wap\n"
        "block_fs unit=0 mount=/boot spawn_path=system/drivers/fs_fat.wap\n";
    memset(&state, 0, sizeof(state));
    dm_rules_load_always_spawn(&state, rules);
    dm_rules_load_pci_match(&state, rules);
    dm_rules_load_block_fs(&state, rules);
    assert(state.always_spawn_rule_count == 0u);
    assert(state.pci_match_rule_count == 0u);
    assert(state.block_fs_rule_count == 0u);
}

int
main(void)
{
    test_always_spawn_rule();
    test_block_fs_rule();
    test_pci_match_rule();
    test_legacy_rule_is_rejected();
    printf("test_device_manager_rules: ok\n");
    return 0;
}
