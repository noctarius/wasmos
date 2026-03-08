#ifndef WASMOS_BLOCK_ATA_H
#define WASMOS_BLOCK_ATA_H

#include <stdint.h>
#include "ipc.h"

int block_ata_init(uint32_t owner_context_id);
int block_ata_endpoint(uint32_t *out_endpoint);
int block_ata_service_once(void);

#endif
