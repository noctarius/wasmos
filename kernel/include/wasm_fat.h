#ifndef WASMOS_WASM_FAT_H
#define WASMOS_WASM_FAT_H

#include <stdint.h>

int wasm_fat_init(uint32_t owner_context_id, uint32_t block_endpoint);
int wasm_fat_endpoint(uint32_t *out_endpoint);
int wasm_fat_service_once(void);

#endif
