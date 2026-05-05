#ifndef WASMOS_CAPABILITY_H
#define WASMOS_CAPABILITY_H

#include <stdint.h>

typedef enum {
    CAP_IO_PORT = 0,
    CAP_IRQ_ROUTE = 1,
    CAP_MMIO_MAP = 2,
    CAP_DMA_BUFFER = 3,
    CAP_SYSTEM_CONTROL = 4
} capability_kind_t;

void capability_init(void);
int capability_grant_name(uint32_t context_id, const uint8_t *name, uint32_t name_len, uint32_t flags);
int capability_has(uint32_t context_id, capability_kind_t kind);
int capability_context_configured(uint32_t context_id);

#endif
