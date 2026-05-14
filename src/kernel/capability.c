#include "capability.h"
#include "memory.h"

#define CAP_ALL_MASK ((1u << 5) - 1u)

typedef struct {
    uint8_t configured;
    uint8_t spawn_profile_configured;
    uint8_t io_port_range_valid;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
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
        g_cap_ctx[i].spawn_profile_configured = 0;
        g_cap_ctx[i].io_port_range_valid = 0;
        g_cap_ctx[i].io_port_min = 0;
        g_cap_ctx[i].io_port_max = 0;
        g_cap_ctx[i].irq_mask = 0;
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

int
capability_set_spawn_profile(uint32_t context_id,
                             uint32_t cap_flags,
                             uint16_t io_port_min,
                             uint16_t io_port_max,
                             uint16_t irq_mask)
{
    if (context_id > MM_MAX_CONTEXTS) {
        return -1;
    }
    capability_context_state_t *ctx = &g_cap_ctx[context_id];
    ctx->spawn_profile_configured = 1;
    ctx->io_port_range_valid = (cap_flags & (1u << 0)) ? 1u : 0u;
    ctx->io_port_min = io_port_min;
    ctx->io_port_max = io_port_max;
    ctx->irq_mask = (cap_flags & (1u << 2)) ? irq_mask : 0;
    return 0;
}

int
capability_spawn_profile_configured(uint32_t context_id)
{
    if (context_id > MM_MAX_CONTEXTS) {
        return 0;
    }
    return g_cap_ctx[context_id].spawn_profile_configured != 0;
}

int
capability_io_port_allowed(uint32_t context_id, uint16_t port)
{
    if (context_id > MM_MAX_CONTEXTS) {
        return 0;
    }
    const capability_context_state_t *ctx = &g_cap_ctx[context_id];
    if (!ctx->spawn_profile_configured || !ctx->io_port_range_valid) {
        return 0;
    }
    return (port >= ctx->io_port_min && port <= ctx->io_port_max) ? 1 : 0;
}

int
capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line)
{
    if (context_id > MM_MAX_CONTEXTS || irq_line >= 16) {
        return 0;
    }
    const capability_context_state_t *ctx = &g_cap_ctx[context_id];
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    return ((ctx->irq_mask & (uint16_t)(1u << irq_line)) != 0) ? 1 : 0;
}

int
capability_mmio_allowed(uint32_t context_id)
{
    if (context_id > MM_MAX_CONTEXTS) {
        return 0;
    }
    const capability_context_state_t *ctx = &g_cap_ctx[context_id];
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    return (ctx->mask & (1u << 2)) != 0;
}
