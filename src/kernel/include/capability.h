/* capability.h - Per-context hardware access capability checks.
 *
 * A capability record is attached to each process context by the process manager
 * when a driver is spawned.  The PM reads allowed resources from linker.metadata
 * in the WASMOS-APP package and calls capability_set_spawn_profile().
 * All hardware hostcalls (I/O port, IRQ, MMIO, DMA) then call the corresponding
 * capability_*_allowed() function before touching hardware. */
#ifndef WASMOS_CAPABILITY_H
#define WASMOS_CAPABILITY_H

#include <stdint.h>
#include "wasmos_driver_abi.h"

/* Sizes the fixed per-context arrays a spawn profile is copied into. A profile
 * declaring more windows than either limit is REFUSED, not truncated: silently
 * dropping the tail would leave the driver's region indices disagreeing with
 * the kernel's, so every window past the limit would fail far from the spawn
 * that over-declared it. PM_DMA_WINDOW_LIMIT must not exceed the DMA one. */
#define CAPABILITY_DMA_WINDOW_LIMIT 16u                 /* max DMA windows per driver context */
#define CAPABILITY_IO_RANGE_LIMIT WASMOS_IO_RANGE_LIMIT /* max disjoint I/O windows */

/* Hardware access kinds, each gated by a separate flag in the spawn profile. */
typedef enum {
    CAP_IO_PORT = 0,            /* x86 I/O port in/out instructions */
    CAP_IRQ_ROUTE = 1,          /* register and receive IRQ events via IPC */
    CAP_MMIO_MAP = 2,           /* map MMIO regions into the process address space */
    CAP_DMA_BUFFER = 3,         /* allocate and share DMA-coherent buffers */
    CAP_SYSTEM_CONTROL = 4,     /* privileged kernel control (reboot, power off, etc.) */
    CAP_SUBSYSTEM_REGISTER = 5, /* register broker subsystems + executable-format handlers */
    CAP_SVC_CLASS_REGISTER = 6  /* register a service under a virtual class (anti-spoof) */
} capability_kind_t;

/* Initialize the capability table; called once during kernel startup.
 * Also seeds the record for context 0 (the kernel) with every capability set. */
void capability_init(void);

/* Grant the named-endpoint capability to context_id (used for IPC access control).
 * `name`/`name_len` is a manifest capability name -- "io.port", "irq.route",
 * "mmio.map", "dma.buffer", "system.control", "subsystem.register",
 * "svc.class"; anything else is refused. Creates the context's record if
 * absent, and ORs the bit in, so grants accumulate. `flags` is used only by
 * "dma.buffer", where it is the pinning budget IN PAGES and additionally
 * installs the platform-wide low-2-GiB DMA window (a spawn profile is what
 * narrows that). Returns 0 on success, -1 for a NULL/empty or unrecognised
 * name, or when the record cannot be allocated. */
int capability_grant_name(uint32_t context_id, const uint8_t* name, uint32_t name_len,
                          uint32_t flags);

/* Return non-zero if context_id holds the given hardware capability kind.
 * Holding a kind is necessary but not sufficient for access: the per-action
 * predicates below additionally require a spawn profile and check the
 * allowlist. Returns 0 for an unknown context. */
int capability_has(uint32_t context_id, capability_kind_t kind);

/* Return non-zero if a capability record exists for context_id. */
int capability_context_configured(uint32_t context_id);

/* Set the full hardware access profile for a newly spawned driver process.
 * cap_flags is a bitmask of (1 << capability_kind_t) values.
 * io_ranges lists the allowed I/O port windows (a device's registers are not
 * always contiguous); irq_mask selects IRQ lines.
 * dma_windows defines physical address ranges the driver may use for DMA.
 *
 * Each group is applied only if its cap_flags bit is set, so clearing a bit
 * means "declare nothing", NOT "revoke": the DMA fields in particular keep
 * whatever a dma.buffer manifest grant left there. Returns 0, or -1 for a
 * context whose record cannot be created, more windows than the limits above,
 * an inverted I/O window (first > last), or a DMA bit set with no windows.
 * Called once per driver at spawn, before the child first runs. */
int capability_set_spawn_profile(uint32_t context_id, uint32_t cap_flags, uint32_t io_range_count,
                                 const wasmos_io_range_t* io_ranges, uint16_t irq_mask,
                                 uint32_t dma_direction_flags, uint32_t dma_max_bytes,
                                 uint32_t dma_window_count, const wasmos_dma_window_t* dma_windows);

/* Return non-zero if a spawn profile (hardware limits) has been set for context_id. */
int capability_spawn_profile_configured(uint32_t context_id);

/* Predicate checks called by hardware hostcalls before granting access.
 *
 * All of these return NON-ZERO FOR ALLOWED and 0 for denied -- the opposite
 * polarity to the 0-on-success functions above, so do not mix them up. All deny
 * by default: an unknown context, or one without a spawn profile, is refused,
 * and so is a zero-length or zero-direction request. capability_io_region_port
 * is the exception and returns a packed error code; it is documented
 * separately. */
int capability_io_port_allowed(uint32_t context_id, uint16_t port);

/* Resolve (region, offset) to an absolute port for a context, where `region` is
 * an index into the I/O windows the spawn profile granted, in the order they
 * were declared, and all `access_width` bytes (1, 2 or 4) must lie inside that
 * window. Returns 0 and writes *out_port on success, else a negative
 * WASMOS_ERR_IO_* code naming which check refused it -- "no such region" and
 * "offset past the end" are different bugs and a caller should be able to tell
 * them apart.
 *
 * This is what lets a driver address its device without ever naming an absolute
 * port: it cannot express an access outside the window it was granted, because
 * it does not supply the base. */
int capability_io_region_port(uint32_t context_id, uint32_t region, uint32_t offset,
                              uint32_t access_width, uint16_t* out_port);
/* Only legacy PIC lines 0..15 can be granted; anything higher is denied. */
int capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line);
/* Whether the context may map MMIO at all. There is no per-range MMIO
 * allowlist, so this is the only gate on which physical ranges it can reach. */
int capability_mmio_allowed(uint32_t context_id);
/* Every bit of direction_flags must be granted, not just one. */
int capability_dma_direction_allowed(uint32_t context_id, uint32_t direction_flags);
/* [base, base+length) must lie ENTIRELY within a single granted window;
 * a range spanning two adjacent windows is denied. Overflowing ranges too. */
int capability_dma_range_allowed(uint32_t context_id, uint64_t base, uint64_t length);
/* Declared pinning budget in bytes, or 0 when none was declared -- which the
 * budget check treats as "no driver-owned DMA regions at all". */
uint32_t capability_dma_max_bytes(uint32_t context_id);

/* Per-context DMA budget for driver-owned pinned regions (region_alloc). The
 * budget (dma_max_bytes) is declared by the driver's dma.buffer manifest
 * capability. capability_dma_within_budget returns non-zero if pinning `bytes`
 * more would stay within it; capability_dma_commit records `bytes` as pinned
 * after a successful allocation. Region allocations are pinned for the context's
 * lifetime (freed at reap when the context is torn down), so there is no
 * uncharge. */
int capability_dma_within_budget(uint32_t context_id, uint64_t bytes);
void capability_dma_commit(uint32_t context_id, uint64_t bytes);

#endif
