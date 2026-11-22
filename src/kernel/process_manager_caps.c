#include "capability.h"
#include "process_manager_internal.h"

/* The packed-arg spawn opcodes each describe exactly one I/O window; the
 * descriptor path (pm_caps_from_v2) may carry several. Centralised so the "no
 * io.port capability means no windows at all" rule cannot drift between the
 * call sites. Returns 0, or BAD_CAPS for an inverted window. */
int pm_caps_set_io_window(pm_spawn_caps_t* caps, uint16_t first, uint16_t last) {
    caps->region_count = 0;
    if ((caps->cap_flags & DEVMGR_CAP_IO_PORT) == 0) {
        return 0;
    }
    if (first > last) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    caps->regions[0].kind = WASMOS_REGION_GRANT_IO;
    caps->regions[0].base = first;
    caps->regions[0].length = (uint32_t)last - (uint32_t)first + 1u;
    caps->region_count = 1;
    return 0;
}

/* A grant the kernel can honour exactly as stated. Every grant is checked
 * against the capability bit its kind needs, and refused rather than dropped
 * without it: the grant's position is its region index, so dropping one would
 * hand every later window to the wrong index. */
static int pm_region_grant_valid(const wasmos_region_grant_t* g, uint32_t cap_flags) {
    if (g->length == 0u) {
        return 0;
    }
    if (g->kind == WASMOS_REGION_GRANT_IO) {
        return (cap_flags & DEVMGR_CAP_IO_PORT) != 0 && g->base <= 0xFFFFu &&
               g->base + g->length - 1u <= 0xFFFFu;
    }
    if (g->kind == WASMOS_REGION_GRANT_MMIO) {
        return (cap_flags & DEVMGR_CAP_MMIO_MAP) != 0 && (g->base & 0xFFFu) == 0u &&
               (g->length & 0xFFFu) == 0u && g->base != 0u && g->base + g->length > g->base;
    }
    return 0;
}

int pm_caps_from_v2(const uint8_t* payload, uint32_t payload_size, pm_spawn_caps_t* caps) {
    wasmos_spawn_caps_v2_t in_caps;
    uint32_t known_cap_mask = DEVMGR_CAP_IO_PORT | DEVMGR_CAP_MMIO_MAP | DEVMGR_CAP_IRQ |
                              DEVMGR_CAP_DMA | DEVMGR_CAP_PCI_FUNCTION;
    uint32_t expected_size = 0;
    uint64_t win_end = 0;

    if (!payload || !caps || payload_size < (uint32_t)sizeof(in_caps)) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    __builtin_memcpy(&in_caps, payload, sizeof(in_caps));
    __builtin_memset(caps, 0, sizeof(*caps));

    if ((in_caps.cap_flags & ~known_cap_mask) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((in_caps.cap_flags & DEVMGR_CAP_IO_PORT) != 0 && in_caps.region_count == 0 &&
        in_caps.io_port_min > in_caps.io_port_max) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if (in_caps.region_count > WASMOS_REGION_LIMIT) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }

    caps->valid = 1;
    caps->cap_flags = in_caps.cap_flags;
    caps->irq_mask = in_caps.irq_mask;
    if ((caps->cap_flags & DEVMGR_CAP_PCI_FUNCTION) != 0) {
        if (in_caps.dma.pci_function > 0xFFFFu) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        caps->pci_function = (uint16_t)in_caps.dma.pci_function;
    }

    /* A non-zero region_count supersedes the single io_port_min/max window;
     * the grants follow the fixed header, ahead of the DMA windows. */
    if (in_caps.region_count > 0) {
        expected_size = WASMOS_SPAWN_CAPS_V2_SIZE(in_caps.region_count, 0);
        if (payload_size < expected_size) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        __builtin_memcpy(caps->regions,
                         payload + sizeof(in_caps),
                         (size_t)in_caps.region_count * sizeof(wasmos_region_grant_t));
        for (uint32_t i = 0; i < in_caps.region_count; ++i) {
            if (!pm_region_grant_valid(&caps->regions[i], caps->cap_flags)) {
                return WASMOS_ERR_PROC_PM_BAD_CAPS;
            }
        }
        caps->region_count = in_caps.region_count;
    } else if (pm_caps_set_io_window(caps, in_caps.io_port_min, in_caps.io_port_max) != 0) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    if ((caps->cap_flags & DEVMGR_CAP_DMA) != 0) {
        if ((in_caps.dma.direction_flags & ~WASMOS_DMA_DIR_BIDIR) != 0 ||
            in_caps.dma.direction_flags == 0 || in_caps.dma.max_bytes == 0 ||
            in_caps.dma.window_count == 0 || in_caps.dma.window_count > PM_DMA_WINDOW_LIMIT) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        expected_size = WASMOS_SPAWN_CAPS_V2_SIZE(in_caps.region_count, in_caps.dma.window_count);
        if (payload_size != expected_size) {
            return WASMOS_ERR_PROC_PM_BAD_CAPS;
        }
        __builtin_memcpy(caps->dma_windows,
                         payload + sizeof(in_caps) +
                             (size_t)in_caps.region_count * sizeof(wasmos_region_grant_t),
                         (size_t)in_caps.dma.window_count * sizeof(wasmos_dma_window_t));
        for (uint32_t i = 0; i < in_caps.dma.window_count; ++i) {
            if (caps->dma_windows[i].length == 0) {
                return WASMOS_ERR_PROC_PM_BAD_CAPS;
            }
            win_end = caps->dma_windows[i].base + caps->dma_windows[i].length;
            if (win_end < caps->dma_windows[i].base) {
                return WASMOS_ERR_PROC_PM_BAD_CAPS;
            }
        }
        caps->dma_direction_flags = in_caps.dma.direction_flags;
        caps->dma_max_bytes = in_caps.dma.max_bytes;
        caps->dma_window_count = in_caps.dma.window_count;
    } else if (payload_size != WASMOS_SPAWN_CAPS_V2_SIZE(in_caps.region_count, 0)) {
        return WASMOS_ERR_PROC_PM_BAD_CAPS;
    }
    return 0;
}

int pm_caps_grant_allowed(uint32_t requester_context, const pm_spawn_caps_t* caps) {
    if (!caps || !caps->valid || caps->cap_flags == 0) {
        return 1;
    }
    return capability_has(requester_context, CAP_SPAWN_GRANT) ? 1 : 0;
}
