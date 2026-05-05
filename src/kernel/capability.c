#include "capability.h"
#include "memory.h"

#define CAP_ALL_MASK ((1u << 5) - 1u)

typedef struct {
    uint8_t configured;
    uint32_t mask;
} capability_context_state_t;

static capability_context_state_t g_cap_ctx[MM_MAX_CONTEXTS + 1];

static int
bytes_eq(const uint8_t *a, uint32_t len, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    uint32_t i = 0;
    while (b[i]) {
        if (i >= len || a[i] != (const uint8_t)b[i]) {
            return 0;
        }
        i++;
    }
    return i == len;
}

static uint32_t
kind_to_mask(capability_kind_t kind)
{
    switch (kind) {
    case CAP_IO_PORT:
        return 1u << 0;
    case CAP_IRQ_ROUTE:
        return 1u << 1;
    case CAP_MMIO_MAP:
        return 1u << 2;
    case CAP_DMA_BUFFER:
        return 1u << 3;
    case CAP_SYSTEM_CONTROL:
        return 1u << 4;
    default:
        return 0;
    }
}

void
capability_init(void)
{
    for (uint32_t i = 0; i <= MM_MAX_CONTEXTS; ++i) {
        g_cap_ctx[i].configured = 0;
        g_cap_ctx[i].mask = 0;
    }
    /* Kernel context has all capabilities by construction. */
    g_cap_ctx[0].configured = 1;
    g_cap_ctx[0].mask = CAP_ALL_MASK;
}

int
capability_grant_name(uint32_t context_id, const uint8_t *name, uint32_t name_len, uint32_t flags)
{
    (void)flags;
    if (context_id > MM_MAX_CONTEXTS || !name || name_len == 0) {
        return -1;
    }

    uint32_t mask = 0;
    if (bytes_eq(name, name_len, "io.port")) {
        mask = kind_to_mask(CAP_IO_PORT);
    } else if (bytes_eq(name, name_len, "irq.route")) {
        mask = kind_to_mask(CAP_IRQ_ROUTE);
    } else if (bytes_eq(name, name_len, "mmio.map")) {
        mask = kind_to_mask(CAP_MMIO_MAP);
    } else if (bytes_eq(name, name_len, "dma.buffer")) {
        mask = kind_to_mask(CAP_DMA_BUFFER);
    } else if (bytes_eq(name, name_len, "system.control")) {
        mask = kind_to_mask(CAP_SYSTEM_CONTROL);
    } else {
        return -1;
    }

    g_cap_ctx[context_id].configured = 1;
    g_cap_ctx[context_id].mask |= mask;
    return 0;
}

int
capability_has(uint32_t context_id, capability_kind_t kind)
{
    if (context_id > MM_MAX_CONTEXTS) {
        return 0;
    }
    uint32_t mask = kind_to_mask(kind);
    if (mask == 0) {
        return 0;
    }
    return (g_cap_ctx[context_id].mask & mask) != 0;
}

int
capability_context_configured(uint32_t context_id)
{
    if (context_id > MM_MAX_CONTEXTS) {
        return 0;
    }
    return g_cap_ctx[context_id].configured != 0;
}
