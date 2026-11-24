/* capability.c - Per-context hardware access capability enforcement.
 * Stores one capability record per driver context_id.  The process manager
 * sets the spawn profile once at driver start; hardware hostcalls then call
 * capability_region_resolve / capability_irq_line_allowed / etc. before acting. */
#include "capability.h"
#include "list.h"
#include "memory.h"
#include "string.h"

#define CAP_ALL_MASK ((1u << 12) - 1u)
#define CAP_PAGE_SIZE 0x1000u

typedef struct {
    uint32_t context_id;
    uint8_t configured;
    uint8_t spawn_profile_configured;
    uint32_t region_count;
    capability_region_t regions[CAPABILITY_REGION_LIMIT];
    uint32_t irq_count;
    uint32_t irqs[WASMOS_IRQ_GRANT_LIMIT];
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
    case CAP_CONSOLE_OWNER:
        return 1u << 7;
    case CAP_SPAWN_GRANT:
        return 1u << 8;
    case CAP_SVC_CONTROL:
        return 1u << 9;
    case CAP_PCI_CONFIG:
        return 1u << 10;
    case CAP_RTC_EFI:
        return 1u << 11;
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

/* The manifest name of every capability kind; the match is exact, so a prefix
 * or a differently-cased name is unrecognised. */
static const struct {
    const char* name;
    capability_kind_t kind;
} k_capability_names[] = {
    {"io.port", CAP_IO_PORT},
    {"irq.route", CAP_IRQ_ROUTE},
    {"mmio.map", CAP_MMIO_MAP},
    {"dma.buffer", CAP_DMA_BUFFER},
    {"system.control", CAP_SYSTEM_CONTROL},
    {"subsystem.register", CAP_SUBSYSTEM_REGISTER},
    {"svc.class", CAP_SVC_CLASS_REGISTER},
    {"console.owner", CAP_CONSOLE_OWNER},
    {"spawn.grant", CAP_SPAWN_GRANT},
    {"svc.control", CAP_SVC_CONTROL},
    {"pci.config", CAP_PCI_CONFIG},
    {"rtc.efi", CAP_RTC_EFI},
};

int capability_kind_from_name(const uint8_t* name, uint32_t name_len, capability_kind_t* out_kind) {
    if (!name || name_len == 0 || !out_kind) {
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(k_capability_names) / sizeof(k_capability_names[0]); ++i) {
        if (str_eq_bytes(name, name_len, k_capability_names[i].name)) {
            *out_kind = k_capability_names[i].kind;
            return 0;
        }
    }
    return -1;
}

/* Grants one capability named by an unterminated byte string, creating the
 * context's record on first grant.  The accepted names are those of
 * capability_kind_from_name.
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
    capability_kind_t kind;
    if (capability_kind_from_name(name, name_len, &kind) != 0) {
        return -1;
    }
    uint32_t mask = kind_to_mask(kind);

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

/* A region the profile can hold exactly as stated: of a known kind, with the
 * capability bit that kind needs, non-empty, inside the 16-bit port space (IO)
 * or already mapped (MMIO). */
static int capability_region_valid(const capability_region_t* r, uint32_t cap_flags) {
    if (r->length == 0u) {
        return 0;
    }
    if (r->kind == WASMOS_REGION_GRANT_IO) {
        return (cap_flags & DEVMGR_CAP_IO_PORT) != 0 && r->base <= 0xFFFFu &&
               r->base + r->length - 1u <= 0xFFFFu;
    }
    if (r->kind == WASMOS_REGION_GRANT_MMIO) {
        return (cap_flags & DEVMGR_CAP_MMIO_MAP) != 0 && r->kva != 0u;
    }
    return 0;
}

/* Installs the per-resource narrowing the process manager derives from a
 * driver's manifest at spawn.  It does NOT grant capability bits — those come
 * from capability_grant_name — but marking the profile configured is what turns
 * the capability_*_allowed checks from "capability alone suffices" into
 * "capability plus this window".
 *
 * cap_flags (DEVMGR_CAP_*) selects which sections of the argument list are
 * honoured: bit 2 the IRQ mask, bit 3 the DMA windows and budget.  A section
 * whose bit is clear is cleared to deny-all — except DMA, which keeps whatever a
 * dma.buffer grant already put there.  Each region needs the bit of its own
 * kind instead (bit 0 for an I/O window, bit 1 for a memory-mapped one).
 * regions and dma_windows are copied by value, so the caller's arrays are
 * borrowed for the call only.
 *
 * Over-declared or malformed input is REFUSED rather than truncated: more than
 * CAPABILITY_REGION_LIMIT regions, a region capability_region_valid rejects,
 * and — when DMA is declared — a zero window count, a count above
 * CAPABILITY_DMA_WINDOW_LIMIT, or a NULL window array all return -1.  Dropping
 * a region would leave the driver's region INDICES disagreeing with what the
 * kernel recorded: every later region would resolve to a different window, far
 * from the spawn that actually over-declared, and the driver has no way to learn
 * that its window list was changed.  A -1 can leave the record partially
 * updated, so the context must not be started.
 *
 * Also returns -1 when no record can be created.  Returns 0 on success. */
int capability_set_spawn_profile(uint32_t context_id, uint32_t cap_flags, uint32_t region_count,
                                 const capability_region_t* regions, uint32_t irq_count,
                                 const uint32_t* irqs, uint32_t dma_direction_flags,
                                 uint32_t dma_max_bytes, uint32_t dma_window_count,
                                 const wasmos_dma_window_t* dma_windows) {
    capability_context_state_t* ctx = capability_state_for_context(context_id, 1);
    if (!ctx) {
        return -1;
    }
    ctx->spawn_profile_configured = 1;
    ctx->region_count = 0;
    if (region_count > CAPABILITY_REGION_LIMIT || (region_count > 0 && !regions)) {
        return -1;
    }
    for (uint32_t i = 0; i < region_count; ++i) {
        if (!capability_region_valid(&regions[i], cap_flags)) {
            return -1;
        }
        ctx->regions[ctx->region_count++] = regions[i];
    }
    ctx->irq_count = 0;
    if (irq_count > WASMOS_IRQ_GRANT_LIMIT || (irq_count > 0 && !irqs)) {
        return -1;
    }
    if (irq_count > 0 && (cap_flags & (1u << 2)) == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < irq_count; ++i) {
        ctx->irqs[ctx->irq_count++] = irqs[i];
    }
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

/* 1 when the port falls inside one of the context's granted I/O windows.  0 for
 * an unknown context, a context without a spawn profile, and a profile that
 * granted no I/O window — so this answers the narrowing question only and does
 * not check the CAP_IO_PORT bit.  It covers a single byte; use
 * capability_region_resolve for a wider access. */
int capability_io_port_allowed(uint32_t context_id, uint16_t port) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        const capability_region_t* r = &ctx->regions[i];
        if (r->kind == WASMOS_REGION_GRANT_IO && port >= r->base && port - r->base < r->length) {
            return 1;
        }
    }
    return 0;
}

/* Resolves a driver's (region index, offset) pair into where the access lands,
 * checking that an access of access_width bytes fits entirely inside that
 * region.  This is the indirection that keeps a driver from naming raw ports or
 * physical addresses: region is an index into the profile's declared windows,
 * in declaration order.
 *
 * access_width is in bytes and must be 1, 2 or 4.  Note the return convention
 * differs from the rest of this file: 0 on success with *out filled, and a
 * packed error otherwise — WASMOS_ERR_IO_NOT_AUTHORIZED for a NULL out, an
 * unknown context, or a profile granting no windows; WASMOS_ERR_IO_BAD_REGION
 * for an index past the declared windows; WASMOS_ERR_IO_OUT_OF_WINDOW for a bad
 * width or an access that would run past the region's end;
 * WASMOS_ERR_IO_MISALIGNED for a memory-mapped access whose offset is not a
 * multiple of its width. */
int capability_region_resolve(uint32_t context_id, uint32_t region, uint32_t offset,
                              uint32_t access_width, capability_region_access_t* out) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    const capability_region_t* r = 0;
    if (!out) {
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    if (!ctx || !ctx->spawn_profile_configured || ctx->region_count == 0) {
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    if (region >= ctx->region_count) {
        return WASMOS_ERR_IO_BAD_REGION;
    }
    if (access_width != 1u && access_width != 2u && access_width != 4u) {
        return WASMOS_ERR_IO_OUT_OF_WINDOW;
    }
    r = &ctx->regions[region];
    /* The WHOLE access must land inside the window, not just its first byte.  A
     * 16- or 32-bit access resolved from the last granted offset would otherwise
     * touch 1 or 3 bytes past the end -- outside the capability, and quite
     * possibly another device's registers.  Checked as a subtraction on the
     * length so a large offset cannot overflow the addition. */
    if (offset >= r->length || (r->length - offset) < access_width) {
        return WASMOS_ERR_IO_OUT_OF_WINDOW;
    }
    if (r->kind == WASMOS_REGION_GRANT_MMIO) {
        /* A device register is accessed at its own width and alignment; a
         * split or unaligned access is undefined for most devices, and arm64
         * device memory faults on it outright. */
        if ((offset & (access_width - 1u)) != 0u) {
            return WASMOS_ERR_IO_MISALIGNED;
        }
        out->kind = WASMOS_REGION_GRANT_MMIO;
        out->where = r->kva + offset;
        return 0;
    }
    out->kind = WASMOS_REGION_GRANT_IO;
    out->where = r->base + offset;
    return 0;
}

/* 1 when the profile's interrupt list names irq_line.  A context without a
 * spawn profile is denied outright — unlike port I/O and MMIO, an interrupt
 * always needs an explicit profile. */
int capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line) {
    const capability_context_state_t* ctx = capability_state_for_context(context_id, 0);
    if (!ctx) {
        return 0;
    }
    if (!ctx->spawn_profile_configured) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->irq_count; ++i) {
        if (ctx->irqs[i] == irq_line) {
            return 1;
        }
    }
    return 0;
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

void capability_release_context(uint32_t context_id) {
    capability_context_state_t* ctx;
    if (context_id == 0) {
        return; /* the kernel context is never reaped */
    }
    ctx = capability_state_for_context(context_id, 0);
    if (ctx) {
        (void)list_remove(&g_cap_ctx, ctx);
    }
}
