#ifndef WASMOS_POLICY_H
#define WASMOS_POLICY_H

#include <stdint.h>

typedef enum {
    POLICY_ACTION_IO_PORT = 0,
    POLICY_ACTION_MMIO_MAP = 1,
    POLICY_ACTION_DMA_BUFFER = 2,
    POLICY_ACTION_IRQ_CONTROL = 3,
    POLICY_ACTION_IRQ_ROUTE = 4,
    POLICY_ACTION_SYSTEM_CONTROL = 5
} policy_action_t;

int policy_authorize(uint32_t context_id, policy_action_t action, uint32_t arg0);

#endif
