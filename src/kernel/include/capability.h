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

#define CAPABILITY_DMA_WINDOW_LIMIT 16u  /* max DMA windows per driver context */

/* Hardware access kinds, each gated by a separate flag in the spawn profile. */
typedef enum {
    CAP_IO_PORT = 0,        /* x86 I/O port in/out instructions */
    CAP_IRQ_ROUTE = 1,      /* register and receive IRQ events via IPC */
    CAP_MMIO_MAP = 2,       /* map MMIO regions into the process address space */
    CAP_DMA_BUFFER = 3,     /* allocate and share DMA-coherent buffers */
    CAP_SYSTEM_CONTROL = 4  /* privileged kernel control (reboot, power off, etc.) */
} capability_kind_t;

/* Initialize the capability table; called once during kernel startup. */
void capability_init(void);

/* Grant the named-endpoint capability to context_id (used for IPC access control). */
int capability_grant_name(uint32_t context_id, const uint8_t *name, uint32_t name_len, uint32_t flags);

/* Return non-zero if context_id holds the given hardware capability kind. */
int capability_has(uint32_t context_id, capability_kind_t kind);

/* Return non-zero if a capability record exists for context_id. */
int capability_context_configured(uint32_t context_id);

/* Set the full hardware access profile for a newly spawned driver process.
 * cap_flags is a bitmask of (1 << capability_kind_t) values.
 * io_port_min/max bound the allowed I/O port range; irq_mask selects IRQ lines.
 * dma_windows defines physical address ranges the driver may use for DMA. */
int capability_set_spawn_profile(uint32_t context_id,
                                 uint32_t cap_flags,
                                 uint16_t io_port_min,
                                 uint16_t io_port_max,
                                 uint16_t irq_mask,
                                 uint32_t dma_direction_flags,
                                 uint32_t dma_max_bytes,
                                 uint32_t dma_window_count,
                                 const wasmos_dma_window_t *dma_windows);

/* Return non-zero if a spawn profile (hardware limits) has been set for context_id. */
int capability_spawn_profile_configured(uint32_t context_id);

/* Predicate checks called by hardware hostcalls before granting access. */
int capability_io_port_allowed(uint32_t context_id, uint16_t port);
int capability_irq_line_allowed(uint32_t context_id, uint32_t irq_line);
int capability_mmio_allowed(uint32_t context_id);
int capability_dma_direction_allowed(uint32_t context_id, uint32_t direction_flags);
int capability_dma_range_allowed(uint32_t context_id, uint64_t base, uint64_t length);
uint32_t capability_dma_max_bytes(uint32_t context_id);

#endif
