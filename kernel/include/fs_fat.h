#ifndef WASMOS_FS_FAT_H
#define WASMOS_FS_FAT_H

#include <stdint.h>
#include "ipc.h"

int fs_fat_init(uint32_t owner_context_id, uint32_t block_endpoint);
int fs_fat_endpoint(uint32_t *out_endpoint);
int fs_fat_service_once(void);
int fs_fat_list_root(void);
int fs_fat_cat_root(const char *filename);

#endif
