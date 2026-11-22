/* virtio_modern.h - the virtio 1.x PCI transport for wasm drivers.
 *
 * A modern device is driven through its common configuration structure
 * (virtio 1.2 §4.1.4.3), queue doorbells in its notify structure and an ISR
 * byte, all in device memory the driver addresses as a granted register window
 * (io_region_*; docs/architecture/10-capability-and-policy.md, Register
 * Windows). Where those structures sit is read once from the device's own
 * config space (virtio_pci_caps.h), which the kernel lets a driver read for the
 * function its spawn profile names.
 *
 * The driver's manifest declares the BAR holding the structures as a region
 * (`kind = "bar"`); virtio_modern_probe refuses a device that places any of
 * them elsewhere, since only that window was granted. Feature negotiation
 * always accepts VIRTIO_F_VERSION_1, which is what selects this transport.
 *
 * Every helper is internal to the driver: -1 means "this device cannot be
 * driven", and the driver turns it into its own packed error. */
#ifndef WASMOS_VIRTIO_MODERN_H
#define WASMOS_VIRTIO_MODERN_H

#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos/virtio_pci_caps.h"
#include "wasmos/vring.h"

/* virtio_pci_common_cfg (virtio 1.2 §4.1.4.3), byte offsets. */
#define VIRTIO_COMMON_DFSELECT 0x00u
#define VIRTIO_COMMON_DF 0x04u
#define VIRTIO_COMMON_GFSELECT 0x08u
#define VIRTIO_COMMON_GF 0x0Cu
#define VIRTIO_COMMON_MSIX 0x10u
#define VIRTIO_COMMON_NUMQ 0x12u
#define VIRTIO_COMMON_STATUS 0x14u
#define VIRTIO_COMMON_CFGGENERATION 0x15u
#define VIRTIO_COMMON_Q_SELECT 0x16u
#define VIRTIO_COMMON_Q_SIZE 0x18u
#define VIRTIO_COMMON_Q_MSIX 0x1Au
#define VIRTIO_COMMON_Q_ENABLE 0x1Cu
#define VIRTIO_COMMON_Q_NOFF 0x1Eu
#define VIRTIO_COMMON_Q_DESCLO 0x20u
#define VIRTIO_COMMON_Q_DESCHI 0x24u
#define VIRTIO_COMMON_Q_AVAILLO 0x28u
#define VIRTIO_COMMON_Q_AVAILHI 0x2Cu
#define VIRTIO_COMMON_Q_USEDLO 0x30u
#define VIRTIO_COMMON_Q_USEDHI 0x34u

/* Device status bits (virtio 1.2 §2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED 128u

/* VIRTIO_F_VERSION_1 (feature bit 32): the device is driven as virtio 1.x. */
#define VIRTIO_F_VERSION_1 (1ull << 32)

/* MSI-X "no vector" (virtio 1.2 §4.1.5.1.2). */
#define VIRTIO_MSI_NO_VECTOR 0xFFFFu

/* The modern queue layout has no alignment rule of its own; 4096 keeps the used
 * ring on its own page, as the legacy layout did. */
#define VIRTIO_MODERN_VRING_ALIGN 4096u

/* One device as the transport sees it. `region` is the io_region index of the
 * granted BAR window; every structure offset in `layout` lies inside it. */
typedef struct {
    int32_t region;
    virtio_pci_layout_t layout;
} virtio_modern_t;

static inline int32_t virtio_modern_cfg_read(void* user, uint32_t reg, uint32_t* out) {
    return wasmos_pci_config_read((int32_t)(uintptr_t)user, (int32_t)reg, out) == 0 ? 0 : -1;
}

/* Locates the device's structures from the config space of function `bdf` and
 * checks that each sits in BAR `bar`, which the driver's manifest declared as
 * region `region`. Returns 0, or -1 for a device without the virtio-modern
 * capabilities (a legacy-only device) or one that places a structure elsewhere. */
static inline int32_t virtio_modern_probe(virtio_modern_t* dev, uint32_t bdf, uint8_t bar,
                                          int32_t region) {
    if (!dev ||
        virtio_pci_find_layout(virtio_modern_cfg_read, (void*)(uintptr_t)bdf, &dev->layout) != 0) {
        return -1;
    }
    if (dev->layout.common.bar != bar || dev->layout.notify.bar != bar ||
        dev->layout.isr.bar != bar ||
        ((dev->layout.found & (1u << VIRTIO_PCI_CAP_DEVICE_CFG)) != 0u &&
         dev->layout.device.bar != bar)) {
        return -1;
    }
    dev->region = region;
    return 0;
}

static inline uint8_t virtio_modern_read8(const virtio_modern_t* dev, uint32_t reg) {
    uint32_t v = 0;
    (void)wasmos_io_region_in8(dev->region, (int32_t)(dev->layout.common.offset + reg), &v);
    return (uint8_t)v;
}

static inline uint16_t virtio_modern_read16(const virtio_modern_t* dev, uint32_t reg) {
    uint32_t v = 0;
    (void)wasmos_io_region_in16(dev->region, (int32_t)(dev->layout.common.offset + reg), &v);
    return (uint16_t)v;
}

static inline uint32_t virtio_modern_read32(const virtio_modern_t* dev, uint32_t reg) {
    uint32_t v = 0;
    (void)wasmos_io_region_in32(dev->region, (int32_t)(dev->layout.common.offset + reg), &v);
    return v;
}

static inline void virtio_modern_write8(const virtio_modern_t* dev, uint32_t reg, uint8_t v) {
    (void)wasmos_io_region_out8(dev->region, (int32_t)(dev->layout.common.offset + reg), v);
}

static inline void virtio_modern_write16(const virtio_modern_t* dev, uint32_t reg, uint16_t v) {
    (void)wasmos_io_region_out16(dev->region, (int32_t)(dev->layout.common.offset + reg), v);
}

static inline void virtio_modern_write32(const virtio_modern_t* dev, uint32_t reg, uint32_t v) {
    (void)wasmos_io_region_out32(
        dev->region, (int32_t)(dev->layout.common.offset + reg), (int32_t)v);
}

static inline void virtio_modern_set_status(const virtio_modern_t* dev, uint8_t status) {
    virtio_modern_write8(dev, VIRTIO_COMMON_STATUS, status);
}

/* Resets the device and waits until it reads back as reset, as the spec
 * requires before reinitialising (virtio 1.2 §4.1.4.3.1). QEMU completes the
 * reset within the write; the bound only keeps a wedged device from hanging the
 * driver. Returns 0, or -1 when the device never reports reset. */
static inline int32_t virtio_modern_reset(const virtio_modern_t* dev) {
    virtio_modern_set_status(dev, 0u);
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (virtio_modern_read8(dev, VIRTIO_COMMON_STATUS) == 0u) {
            return 0;
        }
    }
    return -1;
}

/* Resets the device, acknowledges it, and negotiates features: the driver
 * accepts the bits of `wanted` the device offers, plus VIRTIO_F_VERSION_1, which
 * a modern device must offer. Leaves the device in FEATURES_OK, ready for queue
 * setup. Returns 0 with *out_offered and *out_accepted (each optional) set, or
 * -1 -- with FAILED set -- for a device that does not offer VERSION_1 or
 * refuses the subset. */
static inline int32_t virtio_modern_negotiate(const virtio_modern_t* dev, uint64_t wanted,
                                              uint64_t* out_offered, uint64_t* out_accepted) {
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    uint64_t offered;
    uint64_t accepted;
    if (virtio_modern_reset(dev) != 0) {
        return -1;
    }
    virtio_modern_set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_modern_set_status(dev, status);
    virtio_modern_write32(dev, VIRTIO_COMMON_DFSELECT, 0u);
    offered = virtio_modern_read32(dev, VIRTIO_COMMON_DF);
    virtio_modern_write32(dev, VIRTIO_COMMON_DFSELECT, 1u);
    offered |= (uint64_t)virtio_modern_read32(dev, VIRTIO_COMMON_DF) << 32;
    if ((offered & VIRTIO_F_VERSION_1) == 0u) {
        virtio_modern_set_status(dev, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    accepted = (offered & wanted) | VIRTIO_F_VERSION_1;
    virtio_modern_write32(dev, VIRTIO_COMMON_GFSELECT, 0u);
    virtio_modern_write32(dev, VIRTIO_COMMON_GF, (uint32_t)accepted);
    virtio_modern_write32(dev, VIRTIO_COMMON_GFSELECT, 1u);
    virtio_modern_write32(dev, VIRTIO_COMMON_GF, (uint32_t)(accepted >> 32));
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_modern_set_status(dev, status);
    if ((virtio_modern_read8(dev, VIRTIO_COMMON_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0u) {
        virtio_modern_set_status(dev, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    /* The device has no MSI-X vector for configuration changes; a driver that
     * wants one writes it after this. */
    virtio_modern_write16(dev, VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);
    if (out_offered) {
        *out_offered = offered;
    }
    if (out_accepted) {
        *out_accepted = accepted;
    }
    return 0;
}

/* Routes configuration-change interrupts to MSI-X table entry `entry`. Returns
 * 0, or -1 when the device refuses the vector (virtio reports that by reading
 * back VIRTIO_MSI_NO_VECTOR). */
static inline int32_t virtio_modern_set_config_vector(const virtio_modern_t* dev, uint16_t entry) {
    virtio_modern_write16(dev, VIRTIO_COMMON_MSIX, entry);
    return virtio_modern_read16(dev, VIRTIO_COMMON_MSIX) == entry ? 0 : -1;
}

/* One byte of the device-specific configuration structure. */
static inline uint8_t virtio_modern_device_read8(const virtio_modern_t* dev, uint32_t off) {
    uint32_t v = 0;
    (void)wasmos_io_region_in8(dev->region, (int32_t)(dev->layout.device.offset + off), &v);
    return (uint8_t)v;
}

/* One naturally aligned 16-bit field of the device-specific configuration. */
static inline uint16_t virtio_modern_device_read16(const virtio_modern_t* dev, uint32_t off) {
    uint32_t v = 0;
    (void)wasmos_io_region_in16(dev->region, (int32_t)(dev->layout.device.offset + off), &v);
    return (uint16_t)v;
}

/* Copies `len` bytes of the device-specific configuration starting at `off` into
 * out, as one consistent snapshot: the device bumps config_generation whenever
 * it changes the structure, so a read that straddles a change is retried
 * (virtio 1.2 §4.1.4.3.1). Returns 0, or -1 when the device has no
 * device-specific structure, the range runs past it, or it keeps changing. */
static inline int32_t virtio_modern_device_read(const virtio_modern_t* dev, uint32_t off,
                                                uint8_t* out, uint32_t len) {
    if ((dev->layout.found & (1u << VIRTIO_PCI_CAP_DEVICE_CFG)) == 0u ||
        off > dev->layout.device.length || len > dev->layout.device.length - off) {
        return -1;
    }
    for (uint32_t attempt = 0; attempt < 8u; ++attempt) {
        uint8_t before = virtio_modern_read8(dev, VIRTIO_COMMON_CFGGENERATION);
        for (uint32_t i = 0; i < len; ++i) {
            out[i] = virtio_modern_device_read8(dev, off + i);
        }
        if (virtio_modern_read8(dev, VIRTIO_COMMON_CFGGENERATION) == before) {
            return 0;
        }
    }
    return -1;
}

/* The size queue `index` offers, or 0 for a queue the device does not have. */
static inline uint16_t virtio_modern_queue_size(const virtio_modern_t* dev, uint16_t index) {
    virtio_modern_write16(dev, VIRTIO_COMMON_Q_SELECT, index);
    return virtio_modern_read16(dev, VIRTIO_COMMON_Q_SIZE);
}

/* Hands queue `index` to the device: its size (vq->num, which must not exceed
 * what virtio_modern_queue_size offered), the device addresses of the three
 * ring areas vring_layout placed, and its MSI-X entry (VIRTIO_MSI_NO_VECTOR for
 * none), then enables it. Stores the queue's doorbell offset inside the notify
 * structure in *out_notify. Returns 0, or -1 when the device refuses the
 * vector. */
static inline int32_t virtio_modern_queue_enable(const virtio_modern_t* dev, uint16_t index,
                                                 const vring_t* vq, uint16_t msix_entry,
                                                 uint32_t* out_notify) {
    uint64_t desc = vq->region_phys;
    uint64_t avail = vq->region_phys + (uint64_t)((uint8_t*)vq->avail_flags - vq->region_base);
    uint64_t used = vq->region_phys + (uint64_t)((uint8_t*)vq->used_flags - vq->region_base);
    virtio_modern_write16(dev, VIRTIO_COMMON_Q_SELECT, index);
    virtio_modern_write16(dev, VIRTIO_COMMON_Q_SIZE, vq->num);
    virtio_modern_write32(dev, VIRTIO_COMMON_Q_DESCLO, (uint32_t)desc);
    virtio_modern_write32(dev, VIRTIO_COMMON_Q_DESCHI, (uint32_t)(desc >> 32));
    virtio_modern_write32(dev, VIRTIO_COMMON_Q_AVAILLO, (uint32_t)avail);
    virtio_modern_write32(dev, VIRTIO_COMMON_Q_AVAILHI, (uint32_t)(avail >> 32));
    virtio_modern_write32(dev, VIRTIO_COMMON_Q_USEDLO, (uint32_t)used);
    virtio_modern_write32(dev, VIRTIO_COMMON_Q_USEDHI, (uint32_t)(used >> 32));
    if (msix_entry != VIRTIO_MSI_NO_VECTOR) {
        virtio_modern_write16(dev, VIRTIO_COMMON_Q_MSIX, msix_entry);
        if (virtio_modern_read16(dev, VIRTIO_COMMON_Q_MSIX) != msix_entry) {
            return -1;
        }
    }
    if (out_notify) {
        *out_notify =
            dev->layout.notify.offset + (uint32_t)virtio_modern_read16(dev, VIRTIO_COMMON_Q_NOFF) *
                                            dev->layout.notify_off_multiplier;
    }
    virtio_modern_write16(dev, VIRTIO_COMMON_Q_ENABLE, 1u);
    return 0;
}

/* Rings queue `index`'s doorbell at the offset virtio_modern_queue_enable
 * reported. */
static inline void virtio_modern_notify(const virtio_modern_t* dev, uint32_t notify,
                                        uint16_t index) {
    (void)wasmos_io_region_out16(dev->region, (int32_t)notify, index);
}

/* Reads (and so clears) the ISR status byte: bit 0 a used-buffer interrupt,
 * bit 1 a configuration change. Only an INTx driver needs it. */
static inline uint8_t virtio_modern_isr(const virtio_modern_t* dev) {
    uint32_t v = 0;
    (void)wasmos_io_region_in8(dev->region, (int32_t)dev->layout.isr.offset, &v);
    return (uint8_t)v;
}

#endif
