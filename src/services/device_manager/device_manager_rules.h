#ifndef WASMOS_DEVICE_MANAGER_RULES_H
#define WASMOS_DEVICE_MANAGER_RULES_H

#include <stdint.h>
#include "device_manager_types.h"

uint16_t dm_rules_count_active(const char *text);
void dm_rules_load_always_spawn(device_manager_state_t *state, const char *text);
void dm_rules_load_block_fs(device_manager_state_t *state, const char *text);
void dm_rules_load_pci_match(device_manager_state_t *state, const char *text);
void dm_rules_load_acpi_match(device_manager_state_t *state, const char *text);

#endif
