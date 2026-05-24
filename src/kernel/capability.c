#include "capability.h"
#include "list.h"
#include "memory.h"
#include "string.h"

#define CAP_ALL_MASK ((1u << 5) - 1u)

typedef struct {
    uint32_t context_id;
    uint8_t configured;
    uint8_t spawn_profile_configured;
    uint8_t io_port_range_valid;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
    uint32_t dma_direction_flags;
    uint32_t dma_max_bytes;
    uint32_t dma_window_count;
    wasmos_dma_window_t dma_windows[CAPABILITY_DMA_WINDOW_LIMIT];
    uint32_t mask;
} capability_context_state_t;

static list_t g_cap_ctx;

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

static capability_context_state_t *
capability_state_for_context(uint32_t context_id, uint8_t create_if_missing)
{
    list_iter_t it;
    capability_context_state_t *ctx = (capability_context_state_t *)list_first(&g_cap_ctx, &it);
    while (ctx) {
        if (ctx->context_id == context_id) {
            return ctx;
        }
        ctx = (capability_context_state_t *)list_next(&it);
    }
    if (!create_if_missing) {
        return 0;
    }
    ctx = (capability_context_state_t *)list_alloc(&g_cap_ctx);
    if (!ctx) {
        return 0;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->context_id = context_id;
    return ctx;
}

void
capability_init(void)
{
    if (list_init(&g_cap_ctx, (uint32_t)sizeof(capability_context_state_t), LIST_IMPL_ARRAY_CHUNK, 16) != 0) {
        return;
    }
    /* Kernel context has all capabilities by construction. */
    capability_context_state_t *kernel = capability_state_for_context(0, 1);
    if (!kernel) {
        return;
    }
    kernel->configured = 1;
    kernel->mask = CAP_ALL_MASK;
}

int
capability_grant_name(uint32_t context_id, const uint8_t *name, uint32_t name_len, uint32_t flags)
{
    (void)flags;
    if (!name || name_len == 0) {
        return -1;
    }

    uint32_t mask = 0;
    if (str_eq_bytes(name, name_len, "io.port")) {
        mask = kind_to_mask(CAP_IO_PORT);
    } else if (str_eq_bytes(name, name_len, "irq.route")) {
        mask = kind_to_mask(CAP_IRQ_ROUTE);
    } else if (str_eq_bytes(name, name_len, "mmio.map")) {
        mask = kind_to_mask(CAP_MMIO_MAP);
    } else if (str_eq_bytes(name, name_len, "dma.buffer")) {
        mask = kind_to_mask(CAP_DMA_BUFFER);
    } else if (str_eq_bytes(name, name_len, "system.control")) {
        mask = kind_to_mask(CAP_SYSTEM_CONTROL);
    } else {
        return -1;
    }

    capability_context_state_t *ctx = capability_state_for_context(context_id, 1);
    if (!ctx) {
        return -1;
    }
    ctx->configured = 1;
    ctx->mask |= mask;
    return 0;
}

int
capability_has(uint32_t context_id, capability_kind_t kind)
{
    capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) return 0;
    uint32_t mask = kind_to_mask(kind);
    if (mask == 0) {
        return 0;
    }
    return (ctx->mask & mask) != 0;
}

int
capability_context_configured(uint32_t context_id)
{
    capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    return ctx ? (ctx->configured != 0) : 0;
}

int
capability_set_spawn_profile(uint32_t context_id,
                             uint32_t cap_flags,
                             uint16_t io_port_min,
                             uint16_t io_port_max,
                             uint16_t irq_mask,
                             uint32_t dma_direction_flags,
                             uint32_t dma_max_bytes,
                             uint32_t dma_window_count,
                             const wasmos_dma_window_t *dma_windows)
{
    capability_context_state_t *ctx = capability_state_for_context(context_id, 1);
    if (!ctx) {
        return -1;
    }
    ctx->spawn_profile_configured = 1;
    ctx->io_port_range_valid = (cap_flags & (1u << 0)) ? 1u : 0u;
    ctx->io_port_min = io_port_min;
    ctx->io_port_max = io_port_max;
    ctx->irq_mask = (cap_flags & (1u << 2)) ? irq_mask : 0;
    if ((cap_flags & (1u << 3)) != 0) {
        if (dma_window_count == 0 || dma_window_count > CAPABILITY_DMA_WINDOW_LIMIT || !dma_windows) {
            return -1;
        }
        ctx->dma_direction_flags = dma_direction_flags;
        ctx->dma_max_bytes = dma_max_bytes;
        ctx->dma_window_count = dma_window_count;
        for (uint32_t w = 0; w < CAPABILITY_DMA_WINDOW_LIMIT; ++w) {
            if (dma_windows && w < dma_window_count) {
                ctx->dma_windows[w] = dma_windows[w];
            } else {
                ctx->dma_windows[w].base = 0;
                ctx->dma_windows[w].length = 0;
            }
        }
    } else {
        ctx->dma_direction_flags = 0;
        ctx->dma_max_bytes = 0;
        ctx->dma_window_count = 0;
        for (uint32_t w = 0; w < CAPABILITY_DMA_WINDOW_LIMIT; ++w) {
            ctx->dma_windows[w].base = 0;
            ctx->dma_windows[w].length = 0;
        }
    }
    return 0;
}

int
capability_spawn_profile_configured(uint32_t context_id)
{
    capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    return ctx ? (ctx->spawn_profile_configured != 0) : 0;
}

int
capability_io_port_allowed(uint32_t context_id, uint16_t port)
{
    const capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || !ctx->io_port_range_valid) {
        return 0;
    }
    return (port >= ctx->io_port_min && port <= ctx->io_port_max) ? 1 : 0;
}

int
capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line)
{
    if (irq_line >= 16) {
        return 0;
    }
    const capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    return ((ctx->irq_mask & (uint16_t)(1u << irq_line)) != 0) ? 1 : 0;
}

int
capability_mmio_allowed(uint32_t context_id)
{
    const capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    return (ctx->mask & (1u << 2)) != 0;
}

int
capability_dma_direction_allowed(uint32_t context_id, uint32_t direction_flags)
{
    if (direction_flags == 0) {
        return 0;
    }
    const capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || (ctx->mask & (1u << 3)) == 0) {
        return 0;
    }
    return (ctx->dma_direction_flags & direction_flags) == direction_flags;
}

int
capability_dma_range_allowed(uint32_t context_id, uint64_t base, uint64_t length)
{
    if (length == 0) {
        return 0;
    }
    const capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || (ctx->mask & (1u << 3)) == 0) {
        return 0;
    }
    if (ctx->dma_window_count == 0 || ctx->dma_window_count > CAPABILITY_DMA_WINDOW_LIMIT) {
        return 0;
    }
    uint64_t end = base + length;
    if (end < base) {
        return 0;
    }
    for (uint32_t w = 0; w < ctx->dma_window_count; ++w) {
        uint64_t win_base = ctx->dma_windows[w].base;
        uint64_t win_len = ctx->dma_windows[w].length;
        uint64_t win_end = win_base + win_len;
        if (win_len == 0 || win_end < win_base) {
            continue;
        }
        if (base >= win_base && end <= win_end) {
            return 1;
        }
    }
    return 0;
}

uint32_t
capability_dma_max_bytes(uint32_t context_id)
{
    const capability_context_state_t *ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || (ctx->mask & (1u << 3)) == 0) {
        return 0;
    }
    return ctx->dma_max_bytes;
}
