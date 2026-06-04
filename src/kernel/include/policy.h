/* policy.h - Capability-backed policy authorization for hardware access.
 * Thin wrapper over the capability layer: maps a (context, action, arg) tuple
 * to an allow/deny decision.  All hardware hostcalls go through policy_authorize
 * before touching hardware. */
#ifndef WASMOS_POLICY_H
#define WASMOS_POLICY_H

#include <stdint.h>

/* The hardware action being requested by a driver hostcall. */
typedef enum {
    POLICY_ACTION_IO_PORT = 0,       /* inb/outb to a specific port */
    POLICY_ACTION_MMIO_MAP = 1,      /* map a physical MMIO range */
    POLICY_ACTION_DMA_BUFFER = 2,    /* allocate a DMA-coherent buffer */
    POLICY_ACTION_IRQ_CONTROL = 3,   /* mask/unmask an IRQ line */
    POLICY_ACTION_IRQ_ROUTE = 4,     /* register to receive IRQ events */
    POLICY_ACTION_SYSTEM_CONTROL = 5 /* privileged system operations */
} policy_action_t;

/* Return 0 if context_id is authorized to perform action with arg0, or -1 to deny. */
int policy_authorize(uint32_t context_id, policy_action_t action, uint32_t arg0);

#endif
