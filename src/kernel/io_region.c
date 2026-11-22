/* io_region.c - region-addressed device register access. See io_region.h. */
#include "io_region.h"

#include "arch/arch_mmio.h"
#include "capability.h"
#include "io.h"
#include "wasmos_status.h"

int io_region_read(uint32_t context_id, uint32_t region, uint32_t offset, uint32_t width,
                   uint32_t* out_value) {
    capability_region_access_t access;
    int rc = capability_region_resolve(context_id, region, offset, width, &access);
    if (rc != 0) {
        return rc;
    }
    if (!out_value) {
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    if (access.kind == WASMOS_REGION_GRANT_MMIO) {
        uintptr_t va = (uintptr_t)access.where;
        *out_value = (width == 1u)   ? (uint32_t)arch_mmio_read8(va)
                     : (width == 2u) ? (uint32_t)arch_mmio_read16(va)
                                     : arch_mmio_read32(va);
        return 0;
    }
    uint16_t port = (uint16_t)access.where;
    *out_value = (width == 1u)   ? (uint32_t)inb(port)
                 : (width == 2u) ? (uint32_t)inw(port)
                                 : inl(port);
    return 0;
}

int io_region_write(uint32_t context_id, uint32_t region, uint32_t offset, uint32_t width,
                    uint32_t value) {
    capability_region_access_t access;
    int rc = capability_region_resolve(context_id, region, offset, width, &access);
    if (rc != 0) {
        return rc;
    }
    if (access.kind == WASMOS_REGION_GRANT_MMIO) {
        uintptr_t va = (uintptr_t)access.where;
        if (width == 1u) {
            arch_mmio_write8(va, (uint8_t)(value & 0xFFu));
        } else if (width == 2u) {
            arch_mmio_write16(va, (uint16_t)(value & 0xFFFFu));
        } else {
            arch_mmio_write32(va, value);
        }
        return 0;
    }
    uint16_t port = (uint16_t)access.where;
    if (width == 1u) {
        outb(port, (uint8_t)(value & 0xFFu));
    } else if (width == 2u) {
        outw(port, (uint16_t)(value & 0xFFFFu));
    } else {
        outl(port, value);
    }
    return 0;
}
