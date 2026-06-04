/* device_manager_rules.h - parser declarations for device-manager rule files */
#ifndef WASMOS_DEVICE_MANAGER_RULES_H
#define WASMOS_DEVICE_MANAGER_RULES_H

#include <stdint.h>
#include "device_manager_types.h"

/* Count non-blank, non-comment lines in text; used to pre-check rule budget. */
uint16_t dm_rules_count_active(const char *text);
/* Parse always_spawn rules (boot-time unconditional driver spawns) from text. */
void dm_rules_load_always_spawn(device_manager_state_t *state, const char *text);
/* Parse block_fs rules (block-device subsystem with mount points) from text. */
void dm_rules_load_block_fs(device_manager_state_t *state, const char *text);
/* Parse pci_match rules (PCI class/vendor/device driver binding) from text. */
void dm_rules_load_pci_match(device_manager_state_t *state, const char *text);
/* Parse acpi_match rules (ACPI/ISA class driver binding) from text. */
void dm_rules_load_acpi_match(device_manager_state_t *state, const char *text);

#endif
