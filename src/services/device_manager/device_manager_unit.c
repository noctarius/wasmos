#include "device_manager_unit.h"

#include <stdio.h>
#include <string.h>

#include "wasmos_driver_abi.h"

int32_t dm_unit_spec(const char* rule_path, const char* instance, const char* path,
                     const char* restart, const char* args, char* out, uint32_t out_cap) {
    const char* base;
    uint32_t base_len;
    char name[SUPERVISOR_UNIT_NAME_MAX + 1u];
    int n;

    if (!rule_path || rule_path[0] == '\0' || rule_path[0] == '/' || !instance ||
        instance[0] == '\0' || !path || path[0] != '/' || !restart || restart[0] == '\0' || !out ||
        out_cap == 0u) {
        return -1;
    }
    base = strrchr(rule_path, '/');
    base = base ? base + 1 : rule_path;
    base_len = (uint32_t)strlen(base);
    if (base_len >= 4u && strcmp(base + base_len - 4u, ".wap") == 0) {
        base_len -= 4u;
    }
    if (base_len == 0u) {
        return -1;
    }
    /* No "%.*s": the wasm libc's printf does not implement a precision on %s. */
    if (base_len >= sizeof(name)) {
        return -1;
    }
    memcpy(name, base, base_len);
    n = snprintf(name + base_len, sizeof(name) - base_len, "@%s", instance);
    if (n <= 0 || (uint32_t)n >= sizeof(name) - base_len) {
        return -1;
    }
    if (args && args[0] != '\0') {
        n = snprintf(out, out_cap, "%s %s restart=%s -- %s", name, path, restart, args);
    } else {
        n = snprintf(out, out_cap, "%s %s restart=%s", name, path, restart);
    }
    if (n <= 0 || (uint32_t)n >= out_cap) {
        return -1;
    }
    return n;
}

int32_t dm_caps_v2_encode(const spawn_caps_t* caps, uint8_t* out, uint32_t out_cap) {
    wasmos_spawn_caps_v2_t header;
    wasmos_dma_window_t window;
    uint32_t window_count = 0;
    uint32_t size;
    uint32_t offset;

    if (!caps || !out || caps->region_count > WASMOS_REGION_LIMIT) {
        return -1;
    }
    memset(&header, 0, sizeof(header));
    header.cap_flags = caps->cap_flags;
    header.irq_mask = caps->irq_mask;
    header.io_port_min = caps->io_port_min;
    header.io_port_max = caps->io_port_max;
    header.region_count = (uint16_t)caps->region_count;
    if (caps->pci_function_valid) {
        header.cap_flags |= DEVMGR_CAP_PCI_FUNCTION;
        header.dma.pci_function = caps->pci_function;
    }
    memset(&window, 0, sizeof(window));
    if ((caps->cap_flags & DEVMGR_CAP_DMA) != 0) {
        header.dma.direction_flags = WASMOS_DMA_DIR_BIDIR;
        header.dma.max_bytes = 4096u;
        header.dma.window_count = 1u;
        window.base = 0;
        window.length = 0x80000000ull;
        window_count = 1u;
    }
    size = WASMOS_SPAWN_CAPS_V2_SIZE(caps->region_count, window_count);
    if (size > out_cap) {
        return -1;
    }
    memcpy(out, &header, sizeof(header));
    offset = (uint32_t)sizeof(header);
    memcpy(out + offset, caps->regions, caps->region_count * sizeof(wasmos_region_grant_t));
    offset += caps->region_count * (uint32_t)sizeof(wasmos_region_grant_t);
    if (window_count > 0u) {
        memcpy(out + offset, &window, sizeof(window));
    }
    return (int32_t)size;
}

/* A BAR becomes a grant of its own kind: an I/O BAR a port window, a memory
 * BAR a memory-mapped one. A memory window is mapped by whole pages, so a BAR
 * that does not start and end on a page boundary would expose whatever shares
 * its first or last page; it is refused instead. Returns 0, or -1 when the BAR
 * is absent, unsized, or cannot be expressed as a grant. */
static int dm_grant_from_bar(const wasmos_pci_bar_t* bar, wasmos_region_grant_t* out) {
    if (bar->base == 0u || bar->size == 0u || bar->size > 0xFFFFFFFFull) {
        return -1;
    }
    if (bar->kind == WASMOS_PCI_BAR_IO) {
        if (bar->base + bar->size - 1u > 0xFFFFu) {
            return -1;
        }
        out->kind = WASMOS_REGION_GRANT_IO;
    } else if (bar->kind == WASMOS_PCI_BAR_MEM32 || bar->kind == WASMOS_PCI_BAR_MEM64) {
        if ((bar->base & 0xFFFu) != 0u || (bar->size & 0xFFFu) != 0u) {
            return -1;
        }
        out->kind = WASMOS_REGION_GRANT_MMIO;
    } else {
        return -1;
    }
    out->base = bar->base;
    out->length = (uint32_t)bar->size;
    return 0;
}

int dm_resolve_declared_regions(const wasmos_module_meta_desc_t* meta,
                                const pci_device_record_t* rec, spawn_caps_t* caps) {
    if (!meta || !rec || !caps) {
        return -1;
    }
    caps->region_count = 0;
    if (meta->region_count > WASMOS_REGION_LIMIT) {
        return -1;
    }
    for (uint32_t i = 0; i < meta->region_count; ++i) {
        const wasmos_region_desc_t* decl = &meta->regions[i];
        wasmos_region_grant_t* grant = &caps->regions[i];
        if (decl->kind == WASMOS_APP_REGION_IO) {
            if (decl->first > decl->last) {
                return -1;
            }
            grant->kind = WASMOS_REGION_GRANT_IO;
            grant->base = decl->first;
            grant->length = (uint32_t)decl->last - (uint32_t)decl->first + 1u;
        } else if (decl->kind != WASMOS_APP_REGION_BAR || decl->bar_index >= WASMOS_PCI_BAR_COUNT ||
                   dm_grant_from_bar(&rec->bars[decl->bar_index], grant) != 0) {
            return -1;
        }
    }
    caps->region_count = meta->region_count;
    return 0;
}

int32_t dm_watch_spawn_encode(const char* path, const char* args, const spawn_caps_t* caps,
                              int32_t notify_endpoint, uint8_t* out, uint32_t out_cap) {
    wasmos_spawn_watch_desc_t desc;
    uint32_t path_len;
    uint32_t args_len;
    int32_t caps_len = 0;

    if (!path || !out) {
        return -1;
    }
    path_len = (uint32_t)strlen(path);
    args_len = args ? (uint32_t)strlen(args) : 0u;
    if (path_len == 0u || path_len > SPAWN_WATCH_PATH_MAX || args_len > SPAWN_WATCH_ARGS_MAX ||
        out_cap < (uint32_t)sizeof(desc) + path_len + args_len) {
        return -1;
    }
    memset(&desc, 0, sizeof(desc));
    desc.version = SPAWN_WATCH_DESC_VERSION;
    desc.notify_endpoint = (uint32_t)notify_endpoint;
    desc.path_off = (uint32_t)sizeof(desc);
    desc.path_len = path_len;
    desc.args_off = desc.path_off + path_len;
    desc.args_len = args_len;
    desc.caps_off = desc.args_off + args_len;
    if (caps) {
        caps_len = dm_caps_v2_encode(caps, out + desc.caps_off, out_cap - desc.caps_off);
        if (caps_len < 0) {
            return -1;
        }
    }
    desc.caps_len = (uint32_t)caps_len;
    memcpy(out, &desc, sizeof(desc));
    memcpy(out + desc.path_off, path, path_len);
    if (args_len > 0u) {
        memcpy(out + desc.args_off, args, args_len);
    }
    return (int32_t)(desc.caps_off + desc.caps_len);
}
