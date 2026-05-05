#include "policy.h"
#include "capability.h"
#include "ipc.h"
#include "irq.h"
#include "process.h"

typedef struct {
    const char *app_name;
    uint16_t irq_mask;
} irq_route_policy_t;

/* Keep policy explicit and default-deny for userspace: capability ownership is
 * necessary but not sufficient for IRQ line routing. */
static const irq_route_policy_t g_irq_route_policy[] = {
    { "ata",             (uint16_t)((1u << 14) | (1u << 15)) },
    { "irq-route-allow", (uint16_t)(1u << 1) },
};

static int
str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int
policy_irq_route_allows(uint32_t context_id, uint32_t irq_line)
{
    if (context_id == IPC_CONTEXT_KERNEL) {
        return 1;
    }
    if (irq_line >= IRQ_COUNT) {
        return 0;
    }
    process_t *proc = process_find_by_context(context_id);
    if (!proc || !proc->name) {
        return 0;
    }
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_irq_route_policy) / sizeof(g_irq_route_policy[0])); ++i) {
        if (!str_eq(proc->name, g_irq_route_policy[i].app_name)) {
            continue;
        }
        return (g_irq_route_policy[i].irq_mask & (uint16_t)(1u << irq_line)) != 0;
    }
    return 0;
}

int
policy_authorize(uint32_t context_id, policy_action_t action, uint32_t arg0)
{
    switch (action) {
    case POLICY_ACTION_IO_PORT:
        return capability_has(context_id, CAP_IO_PORT) ? 0 : -1;
    case POLICY_ACTION_MMIO_MAP:
        return capability_has(context_id, CAP_MMIO_MAP) ? 0 : -1;
    case POLICY_ACTION_DMA_BUFFER:
        return capability_has(context_id, CAP_DMA_BUFFER) ? 0 : -1;
    case POLICY_ACTION_IRQ_CONTROL:
        return capability_has(context_id, CAP_IRQ_ROUTE) ? 0 : -1;
    case POLICY_ACTION_SYSTEM_CONTROL:
        return capability_has(context_id, CAP_SYSTEM_CONTROL) ? 0 : -1;
    case POLICY_ACTION_IRQ_ROUTE:
        if (policy_authorize(context_id, POLICY_ACTION_IRQ_CONTROL, 0) != 0) {
            return -1;
        }
        return policy_irq_route_allows(context_id, arg0) ? 0 : -1;
    default:
        return -1;
    }
}
