/* capability.c - Per-context hardware access capability enforcement.
 * Stores one capability record per driver context_id.  The process manager
 * sets the spawn profile once at driver start; hardware hostcalls then call
 * capability_io_port_allowed / capability_irq_line_allowed / etc. before acting. */
#include "capability.h"
#include "list.h"
#include "memory.h"
#include "string.h"

#define CAP_ALL_MASK ((1u << 7) - 1u)
#define CAP_PAGE_SIZE 0x1000u

typedef struct {
    uint32_t context_id;
    uint8_t configured;
    uint8_t spawn_profile_configured;
    uint8_t io_port_range_valid;
    uint32_t io_range_count;
    wasmos_io_range_t io_ranges[CAPABILITY_IO_RANGE_LIMIT];
    uint16_t irq_mask;
    uint32_t dma_direction_flags;
    uint32_t dma_max_bytes;
    uint64_t dma_pinned_bytes; /* cumulative region_alloc bytes charged against dma_max_bytes */
    uint32_t dma_window_count;
    wasmos_dma_window_t dma_windows[CAPABILITY_DMA_WINDOW_LIMIT];
    uint32_t mask;
} capability_context_state_t;

/* TODO: The capability table has no lock, while grants (spawn/app load) and
 * checks (hardware host calls) run on any CPU. A grant that grows the list can
 * race a concurrent lookup. */
static list_t g_cap_ctx;

static uint32_t kind_to_mask(capability_kind_t kind) {
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
    case CAP_SUBSYSTEM_REGISTER:
        return 1u << 5;
    case CAP_SVC_CLASS_REGISTER:
        return 1u << 6;
    default:
        return 0;
    }
}

static capability_context_state_t* capability_state_for_context(uint32_t context_id,
                                                                uint8_t create_if_missing) {
    list_iter_t it;
    capability_context_state_t* ctx = (capability_context_state_t*)list_first(&g_cap_ctx, &it);
    while (ctx) {
        if (ctx->context_id == context_id) {
            return ctx;
        }
        ctx = (capability_context_state_t*)list_next(&it);
    }
    if (!create_if_missing) {
        return 0;
    }
    ctx = (capability_context_state_t*)list_alloc(&g_cap_ctx);
    if (!ctx) {
        return 0;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->context_id = context_id;
    return ctx;
}

/* Creates the capability table and seeds context 0 with every capability bit.
 * Must run before any grant or check; without it every lookup finds an empty
 * list and therefore denies, which is the safe direction but silently disables
 * the kernel context too.
 *
 * Reports nothing: a failed list_init or a failed kernel record simply returns.
 * Not idempotent — a second call re-inits the list and abandons every record. */
void capability_init(void) {
    if (list_init(
            &g_cap_ctx, (uint32_t)sizeof(capability_context_state_t), LIST_IMPL_ARRAY_CHUNK, 16) !=
        0) {
        return;
    }
    /* Kernel context has all capabilities by construction. */
    capability_context_state_t* kernel = capability_state_for_context(0, 1);
    if (!kernel) {
        return;
    }
    kernel->configured = 1;
    kernel->mask = CAP_ALL_MASK;
}

/* Grants one capability named by an unterminated byte string, creating the
 * context's record on first grant.  The accepted names are exactly "io.port",
 * "irq.route", "mmio.map", "dma.buffer", "system.control", "subsystem.register"
 * and "svc.class"; the match is exact, so a prefix or a differently-cased name
 * is refused.
 *
 * Grants accumulate — bits are OR-ed in and there is no revoke.
 *
 * flags is used ONLY by "dma.buffer", where it is a budget in 4 KiB PAGES, and
 * only when no DMA window has been declared yet: that path also opens the
 * platform-wide low-2-GiB window with bidirectional access, which a later
 * capability_set_spawn_profile can narrow.  Every other name ignores flags.
 *
 * Returns 0 on success, -1 for a NULL or empty name, an unrecognised name, or a
 * full record list. */
int capability_grant_name(uint32_t context_id, const uint8_t* name, uint32_t name_len,
                          uint32_t flags) {
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
    } else if (str_eq_bytes(name, name_len, "subsystem.register")) {
        mask = kind_to_mask(CAP_SUBSYSTEM_REGISTER);
    } else if (str_eq_bytes(name, name_len, "svc.class")) {
        mask = kind_to_mask(CAP_SVC_CLASS_REGISTER);
    } else {
        return -1;
    }

    capability_context_state_t* ctx = capability_state_for_context(context_id, 1);
    if (!ctx) {
        return -1;
    }
    ctx->configured = 1;
    ctx->mask |= mask;
    /* dma.buffer authorizes DMA. The manifest grant carries only a budget, so
     * the window is the platform-wide low-2-GiB DMA range rather than a
     * per-driver range; a spawn profile (capability_set_spawn_profile) is what
     * narrows it. */
    if (mask == kind_to_mask(CAP_DMA_BUFFER) && ctx->dma_window_count == 0) {
        ctx->dma_direction_flags = WASMOS_DMA_DIR_BIDIR;
        ctx->dma_max_bytes = flags * CAP_PAGE_SIZE; /* flags = budget in pages */
        ctx->dma_pinned_bytes = 0;
        ctx->dma_window_count = 1;
        ctx->dma_windows[0].base = 0;
        ctx->dma_windows[0].length = 0x80000000ull;
    }
    return 0;
}

/* 1 when the context holds the capability, 0 otherwise — a predicate, not a
 * status code.  An unknown context and an unrecognised kind both answer 0, so a
 * caller cannot distinguish "denied" from "asked the wrong question".  Checks
 * the capability bit ONLY: the per-resource narrowing lives in the
 * capability_*_allowed calls. */
int capability_has(uint32_t context_id, capability_kind_t kind) {
    capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx)
        return 0;
    uint32_t mask = kind_to_mask(kind);
    if (mask == 0) {
        return 0;
    }
    return (ctx->mask & mask) != 0;
}

/* 1 once the context has received at least one grant (or is the kernel context),
 * 0 for a context that was never granted anything.  Says nothing about WHICH
 * capabilities are held, and is independent of
 * capability_spawn_profile_configured. */
int capability_context_configured(uint32_t context_id) {
    capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    return ctx ? (ctx->configured != 0) : 0;
}

/* Installs the per-resource narrowing the process manager derives from a
 * driver's manifest at spawn.  It does NOT grant capability bits — those come
 * from capability_grant_name — but marking the profile configured is what turns
 * the capability_*_allowed checks from "capability alone suffices" into
 * "capability plus this window".
 *
 * cap_flags selects which sections of the argument list are honoured: bit 0 the
 * I/O ranges, bit 2 the IRQ mask, bit 3 the DMA windows and budget.  A section
 * whose bit is clear is cleared to deny-all — except DMA, which keeps whatever a
 * dma.buffer grant already put there.  io_ranges and dma_windows are copied by
 * value, so the caller's arrays are borrowed for the call only.
 *
 * Over-declared or malformed input is REFUSED rather than truncated: more than
 * CAPABILITY_IO_RANGE_LIMIT ranges, an inverted [first, last] range, and — when
 * DMA is declared — a zero window count, a count above
 * CAPABILITY_DMA_WINDOW_LIMIT, or a NULL window array all return -1.  A -1 can
 * leave the record partially updated, so the context must not be started.
 *
 * Also returns -1 when no record can be created.  Returns 0 on success. */
int capability_set_spawn_profile(uint32_t context_id, uint32_t cap_flags, uint32_t io_range_count,
                                 const wasmos_io_range_t* io_ranges, uint16_t irq_mask,
                                 uint32_t dma_direction_flags, uint32_t dma_max_bytes,
                                 uint32_t dma_window_count,
                                 const wasmos_dma_window_t* dma_windows) {
    capability_context_state_t* ctx = capability_state_for_context(context_id, 1);
    if (!ctx) {
        return -1;
    }
    ctx->spawn_profile_configured = 1;
    ctx->io_port_range_valid = (cap_flags & (1u << 0)) ? 1u : 0u;
    ctx->io_range_count = 0;
    if (ctx->io_port_range_valid && io_ranges) {
        /* Refuse rather than truncate, the same way dma_window_count is validated
         * below.  Silently dropping the tail would leave the driver's region
         * INDICES disagreeing with what the kernel recorded: every region past the
         * limit resolves to WASMOS_ERR_IO_BAD_REGION at run time, far from the
         * spawn that actually over-declared, and the driver has no way to learn
         * that its window list was shortened. */
        if (io_range_count > CAPABILITY_IO_RANGE_LIMIT) {
            return -1;
        }
        for (uint32_t i = 0; i < io_range_count; ++i) {
            if (io_ranges[i].first > io_ranges[i].last) {
                return -1; /* an inverted window would silently allow nothing */
            }
            ctx->io_ranges[ctx->io_range_count++] = io_ranges[i];
        }
    }
    ctx->irq_mask = (cap_flags & (1u << 2)) ? irq_mask : 0;
    if ((cap_flags & (1u << 3)) != 0) {
        if (dma_window_count == 0 || dma_window_count > CAPABILITY_DMA_WINDOW_LIMIT ||
            !dma_windows) {
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
    }
    /* With cap_flags bit 3 clear the spawner declares no DMA window, and the DMA
     * fields keep whatever the dma.buffer manifest grant (capability_grant_name)
     * put there. */
    return 0;
}

/* 1 once capability_set_spawn_profile has run for this context.  policy.c uses
 * it to decide whether a capability bit alone authorises an action or whether
 * the profile's windows must be consulted, so a context WITHOUT a profile is the
 * more permissive case for port I/O and MMIO, and the less permissive one for
 * IRQ lines and DMA. */
int capability_spawn_profile_configured(uint32_t context_id) {
    capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    return ctx ? (ctx->spawn_profile_configured != 0) : 0;
}

/* 1 when the port falls inside one of the context's granted I/O ranges, which
 * are inclusive of both endpoints.  0 for an unknown context, a context without
 * a spawn profile, and a profile that did not declare the I/O section — so this
 * answers the narrowing question only and does not check the CAP_IO_PORT bit.
 * It covers a single byte; use capability_io_region_port for a wider access. */
int capability_io_port_allowed(uint32_t context_id, uint16_t port) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || !ctx->io_port_range_valid) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->io_range_count; ++i) {
        if (port >= ctx->io_ranges[i].first && port <= ctx->io_ranges[i].last) {
            return 1;
        }
    }
    return 0;
}

/* Resolves a driver's (region index, offset) pair into an absolute I/O port,
 * checking that an access of access_width bytes fits entirely inside that
 * region.  This is the indirection that keeps a driver from naming raw ports:
 * region is an index into the profile's declared ranges, in declaration order.
 *
 * access_width is in bytes and must be 1..4.  Note the return convention differs
 * from the rest of this file: 0 on success with the port in *out_port, and a
 * packed error otherwise — WASMOS_ERR_IO_NOT_AUTHORIZED for a NULL out_port, an
 * unknown context, or no I/O profile; WASMOS_ERR_IO_BAD_REGION for an index past
 * the declared ranges; WASMOS_ERR_IO_OUT_OF_WINDOW for a bad width or an access
 * that would run past the region's last port. */
int capability_io_region_port(uint32_t context_id, uint32_t region, uint32_t offset,
                              uint32_t access_width, uint16_t* out_port) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    uint32_t span = 0;
    if (!out_port) {
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    if (!ctx || !ctx->spawn_profile_configured || !ctx->io_port_range_valid) {
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    if (region >= ctx->io_range_count) {
        return WASMOS_ERR_IO_BAD_REGION;
    }
    if (access_width == 0u || access_width > 4u) {
        return WASMOS_ERR_IO_OUT_OF_WINDOW;
    }
    span = (uint32_t)ctx->io_ranges[region].last - (uint32_t)ctx->io_ranges[region].first;
    /* The WHOLE access must land inside the window, not just its first byte.  A
     * 16- or 32-bit access resolved from the last granted offset would otherwise
     * have inw/inl touch 1 or 3 ports past `last` -- outside the capability, and
     * quite possibly another device's registers.  Checked as a subtraction on the
     * span so a large offset cannot overflow the addition. */
    if (offset > span || (span - offset) < (access_width - 1u)) {
        return WASMOS_ERR_IO_OUT_OF_WINDOW;
    }
    *out_port = (uint16_t)(ctx->io_ranges[region].first + offset);
    return 0;
}

/* 1 when the profile's 16-bit IRQ mask has the line's bit set.  Lines 16 and
 * above are always 0, since the mask cannot express them.  A context without a
 * spawn profile is denied outright — unlike port I/O and MMIO, an IRQ line
 * always needs an explicit profile. */
int capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line) {
    if (irq_line >= 16) {
        return 0;
    }
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    return ((ctx->irq_mask & (uint16_t)(1u << irq_line)) != 0) ? 1 : 0;
}

/* 1 when the context has a spawn profile AND holds CAP_MMIO_MAP.  There is no
 * per-address MMIO window in the profile, so this is a whole-capability answer
 * and any permitted context may map any MMIO address. */
int capability_mmio_allowed(uint32_t context_id) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    return (ctx->mask & (1u << 2)) != 0;
}

/* 1 when EVERY direction bit requested is present in the context's permitted set
 * — a subset test, not an intersection test, so asking for bidirectional access
 * against a to-device-only profile is denied rather than downgraded.  A zero
 * request is denied.  Requires both a spawn profile and CAP_DMA_BUFFER. */
int capability_dma_direction_allowed(uint32_t context_id, uint32_t direction_flags) {
    if (direction_flags == 0) {
        return 0;
    }
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || (ctx->mask & (1u << 3)) == 0) {
        return 0;
    }
    return (ctx->dma_direction_flags & direction_flags) == direction_flags;
}

/* 1 when [base, base+length) is contained in a SINGLE declared DMA window.  base
 * is a physical address.  A range that spans two adjacent windows is denied,
 * because containment is tested window by window.
 *
 * Denies a zero length, a range that wraps, an unknown context, a context
 * without a spawn profile or CAP_DMA_BUFFER, and a window list that is empty or
 * over the limit.  A window of zero length, or one that wraps, is skipped rather
 * than treated as matching. */
int capability_dma_range_allowed(uint32_t context_id, uint64_t base, uint64_t length) {
    if (length == 0) {
        return 0;
    }
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
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

/* The context's total DMA budget in bytes, or 0 when it has none — which also
 * covers an unknown context, a missing spawn profile and a missing
 * CAP_DMA_BUFFER.  0 means no driver-owned DMA region may be created at all, not
 * "unlimited". */
uint32_t capability_dma_max_bytes(uint32_t context_id) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || (ctx->mask & (1u << 3)) == 0) {
        return 0;
    }
    return ctx->dma_max_bytes;
}

/* 1 when charging `bytes` on top of what is already committed would stay within
 * the context's budget.  A pure test: it does not reserve anything, so the
 * caller must follow a successful allocation with capability_dma_commit, and
 * nothing serialises the two.  A zero `bytes`, a missing profile or capability,
 * and a zero budget are all denied. */
int capability_dma_within_budget(uint32_t context_id, uint64_t bytes) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx || bytes == 0) {
        return 0;
    }
    if (!ctx->spawn_profile_configured || (ctx->mask & (1u << 3)) == 0) {
        return 0;
    }
    if (ctx->dma_max_bytes == 0) {
        return 0; /* no budget declared -> no driver-owned DMA regions */
    }
    return (ctx->dma_pinned_bytes + bytes) <= (uint64_t)ctx->dma_max_bytes;
}

/* Charges `bytes` against the context's cumulative DMA usage.  Monotonic: there
 * is no matching release, so a driver that frees a DMA region does not get its
 * budget back, and the counter is neither clamped to dma_max_bytes nor checked
 * for overflow.  An unknown context is ignored. */
void capability_dma_commit(uint32_t context_id, uint64_t bytes) {
    capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return;
    }
    ctx->dma_pinned_bytes += bytes;
}
