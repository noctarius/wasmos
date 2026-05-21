#ifndef WASMOS_DEVICE_MANAGER_RULES_H
#define WASMOS_DEVICE_MANAGER_RULES_H

#include <stdint.h>
#include "device_manager_types.h"

uint16_t dm_rules_count_active(const char *text);
int dm_rules_extract_spawn_path(const char *text, char *out_path, uint32_t out_len);
void dm_rules_load_block_fs(device_manager_state_t *state, const char *text);

#endif
